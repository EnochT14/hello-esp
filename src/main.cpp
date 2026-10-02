// HelloESP - a website on a single ESP32.
//
// Boot order matters and is deliberate:
//   1. arm the crash guard        (before anything can touch flash)
//   2. mount storage              (read-only if we are crash-looping)
//   3. load config, state, guestbook, sensors
//   4. bring up the network and the Worker relay
//   5. disarm the crash guard     (we reached the end of setup)
//
// Nothing before step 2 writes to flash, so a torn filesystem can never be
// made worse by the boot sequence itself.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_log.h>

#include "app/adsb.h"
#include "app/backup.h"
#include "app/board.h"
#include "app/config.h"
#include "app/guestbook.h"
#include "app/relay.h"
#include "app/sensors.h"
#include "app/state.h"
#include "app/storage.h"
#include "app/web.h"

namespace {

constexpr uint32_t kNtpTimeoutMs = 8000;
constexpr uint32_t kLogIntervalMs = 300000UL;      // 5 min
constexpr uint32_t kStatsPushMs = 15000UL;         // 15 s
constexpr uint32_t kHeartbeatMs = 300000UL;        // 5 min
constexpr uint32_t kCommitMs = 60000UL;            // state blob flush
constexpr uint32_t kPruneEveryMs = 86400000UL;     // once a day
constexpr uint32_t kWifiRetryMs = 10000UL;

uint32_t g_lastLog = 0;
uint32_t g_lastStatsPush = 0;
uint32_t g_lastHeartbeat = 0;
uint32_t g_lastCommit = 0;
uint32_t g_lastPrune = 0;
uint32_t g_lastWifiRetry = 0;
// 0xFFFFFFFF is an impossible minute number, so the first boundary fires.
uint32_t lastLoggedMinute = 0xFFFFFFFF;
time_t g_bootTime = 0;

// Samples once per log interval and folds the result into the period stats.
void takeSample() {
  sensors::Reading s{};
  sensors::read(&s);

  const uint32_t reqs = state::requestsThisInterval();
  state::week().addSample(s.tempC, s.humidity, s.co2, s.voc, reqs);
  state::month().addSample(s.tempC, s.humidity, s.co2, s.voc, reqs);
  state::year().addSample(s.tempC, s.humidity, s.co2, s.voc, reqs);

  state::updateRecords(s.tempC, s.co2);
  state::logSample(s.tempC, s.tempF, s.humidity, s.pressure, s.altitude, s.co2,
                   s.voc, s.heatIndex, WiFi.RSSI(), reqs);
  sensors::noteCycleResult(s.bmeOk, s.ccsOk);
  state::resetIntervalRequests();
}

// True once per minute on the wall clock, so logging lands on round boundaries
// and survives an NTP correction better than a "every 5 minutes since boot".
bool onLogBoundary() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;
  const uint32_t minute = static_cast<uint32_t>(mktime(&t) / 60);
  const bool due = (minute != lastLoggedMinute);
  lastLoggedMinute = minute;
  return due;
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  // g_lastWifiRetry starts at 0, which would suppress the very first attempt
  // for kWifiRetryMs; treat "never tried" as explicitly allowed.
  static bool triedOnce = false;
  if (triedOnce && (millis() - g_lastWifiRetry) < kWifiRetryMs) return;
  triedOnce = true;
  g_lastWifiRetry = millis();

  Serial.printf("[net] connecting to '%s'\n", config::v.wifiSsid);
  WiFi.begin(config::v.wifiSsid, config::v.wifiPass);
  WiFi.setSleep(false);       // the relay is latency sensitive
  WiFi.setAutoReconnect(true);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[net] connected %s\n", WiFi.localIP().toString().c_str());
    // mDNS needs a working interface, so it can only start once we have one.
    if (MDNS.begin("helloesp")) {
      MDNS.addService("http", "tcp", 80);
    }
  }
}

// Starts SNTP. Must be called at most once per boot: every configTime() call
// allocates a UDP PCB for the SNTP client, and calling it in a retry loop
// exhausts lwIP's mempools (sntp_init aborts with "Failed to allocate udp pcb",
// and unrelated lwIP paths then abort on MEMP_SYS_TIMEOUT).
void startClock() {
  config::applyTimezone();
  configTime(0, 0, "pool.ntp.org");
  configTime(0, 0, "time.nist.gov");
  configTime(0, 0, "time.google.com");
}

// Cheap poll: no allocation, safe to call every loop iteration.
bool pollClock() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;
  if (t.tm_year < 120) return false;  // 1970-ish means the sync has not landed
  g_bootTime = mktime(&t);
  return true;
}



// Feeds queued Worker requests to the web layer and ships responses back.
void serveRelayQueue() {
  int32_t id = 0;
  char method[8], path[192], body[2048];
  while (relay::nextRequest(&id, method, sizeof(method), path, sizeof(path),
                            body, sizeof(body))) {
    web::handleRelayedRequest(id, method, path, body);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  // Silence the IDF's own logging before WiFi starts. The WiFi driver emits
  // log lines from its timer task, and on this core that path aborts inside
  // lock_init_generic() when it reaches the UART VFS from a non-loop task.
  // We only need our own Serial output, so the IDF verbosity buys nothing.
  esp_log_level_set("*", ESP_LOG_NONE);

  // (1) Before anything else: decide whether this boot is a repeat failure.
  fsx::armBootGuard();

  // (2) Storage. A mount failure here is fatal but bounded: the device reports
  // it on the OLED rather than looping silently.
  const bool mounted = fsx::begin();
  Serial.printf("[fs] mounted=%d health=%s\n", mounted, fsx::health());

  sensors::displayBegin();
  if (!mounted) {
    Serial.println("[boot] no usable filesystem; check the flash partition");
  }

  // (3) Configuration and persisted state.
  if (!config::load()) {
    Serial.println("[boot] FATAL: /config.txt missing or unreadable");
    // Deliberately stop here. Continuing would run a public web server with no
    // credentials, which is worse than being visibly broken.
    while (true) {
      delay(1000);
    }
  }
  config::applyTimezone();

  state::begin();
  guestbook::begin();
  sensors::begin();
  adsb::begin();
  backup::begin();
  relay::begin();

  // (4) Network. SNTP is started exactly once; the loop only polls it.
  connectWifi();
  startClock();

  web::begin();
  takeSample();

  // (5) We made it. Any future panic starts the count from zero again.
  fsx::disarmBootGuard();
  Serial.printf("[boot] %s ready, free heap %u\n", FIRMWARE_VERSION_STR,
                static_cast<unsigned>(ESP.getFreeHeap()));
}

void loop() {
  connectWifi();

  // Polls the LAN receiver on its own interval and pushes a compact extract.
  adsb::tick();

  // Late NTP: poll the already-started client (no re-init), then reconcile
  // anything that was held back while the clock was unknown.
  if (!state::clockOk() && WiFi.status() == WL_CONNECTED) {
    if (pollClock()) {
      state::setClockOk(true);
      {
        struct tm ct;
        getLocalTime(&ct, 0);
        Serial.print("[net] clock set ");
        Serial.println(asctime(&ct));
      }
      state::dumsorBootCheck();
      state::rollPeriods();
    }
  }

  relay::tick();
  serveRelayQueue();
  backup::tick();
  sensors::displayCycle();

  const uint32_t now = millis();

  // Late-arriving outage check: the first boot after NTP reports the gap, but if
  // the clock was not ready yet this retries.
  if (state::clockOk()) state::dumsorBootCheck();

  if (state::clockOk() && onLogBoundary() && (now - g_lastLog) > kLogIntervalMs) {
    g_lastLog = now;
    takeSample();
    state::rollPeriods();
    if (now - g_lastPrune > kPruneEveryMs) {
      g_lastPrune = now;
      state::pruneLogs(10);
    }
  }

  if (state::clockOk() && (now - g_lastHeartbeat) > kHeartbeatMs) {
    g_lastHeartbeat = now;
    state::dumsorHeartbeat();
    state::commit(false, kCommitMs);
  }

  // Debounced: a burst of visitors produces one write, not one per request.
  if (state::dirty() && (now - g_lastCommit) > kCommitMs) {
    g_lastCommit = now;
    state::commit(false, kCommitMs);
  }

  if (relay::connected() && (now - g_lastStatsPush) > kStatsPushMs) {
    g_lastStatsPush = now;
    static char buf[1400];
    const size_t n = web::buildStatsJson(buf, sizeof(buf));
    if (n) {
      char frame[1600];
      const int fn = snprintf(frame, sizeof(frame),
                              "{\"type\":\"event\",\"event\":\"stats_update\",\"data\":%s}", buf);
      if (fn > 0 && fn < static_cast<int>(sizeof(frame))) {
        relay::sendJson(frame, static_cast<size_t>(fn));
      }
    }
  }

  // The heap watchdog from v1.5 is deliberately gone: rebooting on low memory
  // turned transient pressure into a visible outage. A slow self-recovery is
  // better than a reboot the owner did not ask for.
  delay(2);
}