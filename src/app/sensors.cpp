#include "sensors.h"

#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_CCS811.h>
#include <U8g2lib.h>

#include "state.h"
#include "storage.h"
#include "util.h"

namespace sensors {
namespace {

constexpr int kSdaPin = 21;
constexpr int kSclPin = 22;

constexpr uint8_t kBmeAddr  = 0x76;
constexpr uint8_t kCcsAddr  = 0x5A;

constexpr uint32_t kSensorStaleMs   = 120000;  // 2 min without a good read
constexpr uint32_t kRecoverCooldown = 60000;   // one bus re-seat per minute
constexpr uint8_t  kRetireCycles    = 30;      // ~2.5 h of 5-min samples

Adafruit_BME280 g_bme;
Adafruit_CCS811 g_ccs;
U8G2_SH1106_128X64_NONAME_F_HW_I2C g_display(U8G2_R0, U8X8_PIN_NONE, kSclPin, kSdaPin);

bool     g_bmeOk = false, g_ccsOk = false, g_oledOk = false;
bool     g_bmeRetired = false, g_ccsRetired = false;
uint8_t  g_bmeBad = 0, g_ccsBad = 0;
uint32_t g_bmeLastGood = 0, g_ccsLastGood = 0;
uint32_t g_lastRecover = 0;
uint32_t g_lastCcsReset = 0;
uint32_t g_readMs = 0;
bool     g_recovered = false;

// Last known-good values. A degraded sensor reports its previous real reading
// rather than a seeded placeholder, so a transient fault is visible as a stale
// value instead of a fictional one.
float    g_tempC = 20.0f, g_hum = 50.0f, g_press = 1013.25f, g_alt = 0.0f;
uint16_t g_co2 = 0, g_voc = 0;
uint8_t  g_page = 0;
uint32_t g_lastPageSwitch = 0;

// --- persisted retirement state -------------------------------------------
constexpr uint8_t kHealthVersion = 1;
constexpr size_t  kHealthSize = 2;

void loadHealth() {
  uint8_t version = 0;
  uint8_t buf[kHealthSize];
  if (!fsx::readBlob("/sensor_health.bin", &version, buf, sizeof(buf))) return;
  if (version != kHealthVersion) return;
  g_bmeRetired = buf[0] != 0;
  g_ccsRetired = buf[1] != 0;
}

void saveHealth() {
  uint8_t buf[kHealthSize] = {0};
  buf[0] = g_bmeRetired ? 1 : 0;
  buf[1] = g_ccsRetired ? 1 : 0;
  fsx::writeBlob("/sensor_health.bin", kHealthVersion, buf, sizeof(buf));
}

inline bool inRange(float v, float lo, float hi) { return !isnan(v) && v >= lo && v <= hi; }

// --- display --------------------------------------------------------------
void printRow(uint8_t row, const char* a, const char* b = nullptr) {
  g_display.setFont(u8g2_font_6x12_tf);
  g_display.setCursor(0, row * 12);
  g_display.print(a);
  if (b) {
    g_display.setCursor(128 - 6 * static_cast<int>(strlen(b)), row * 12);
    g_display.print(b);
  }
}

void pageClock() {
  g_display.clearBuffer();
  struct tm t = {};
  char buf[24];
  if (getLocalTime(&t, 0)) {
    strftime(buf, sizeof(buf), "%H:%M", &t);
  } else {
    snprintf(buf, sizeof(buf), "--:--");
  }
  g_display.setFont(u8g2_font_10x20_tf);
  g_display.drawStr(24, 26, buf);
  if (getLocalTime(&t, 0)) {
    strftime(buf, sizeof(buf), "%a %d %b", &t);
    g_display.setFont(u8g2_font_6x12_tf);
    g_display.drawStr(20, 44, buf);
  }
  g_display.sendBuffer();
}

void pageSensors() {
  g_display.clearBuffer();
  char l[24], r[24];
  if (!g_bmeOk) {
    g_display.setFont(u8g2_font_6x12_tf);
    g_display.setCursor(0, 16);
    g_display.print("BME280 not ready");
    g_display.setCursor(0, 36);
    g_display.print(g_bmeRetired ? "retired" : "checking");
    g_display.sendBuffer();
    return;
  }
  snprintf(l, sizeof(l), "%.1f C", g_tempC);
  snprintf(r, sizeof(r), "%.0f %%", g_hum);
  printRow(0, "Temperature", l);
  printRow(1, "Humidity", r);
  snprintf(l, sizeof(l), "%.0f hPa", g_press);
  printRow(2, l);
  snprintf(r, sizeof(r), "%.0f ft", g_alt);
  printRow(3, "Altitude", r);
  g_display.sendBuffer();
}

void pageAir() {
  g_display.clearBuffer();
  char buf[24];
  if (!g_ccsOk) {
    g_display.setFont(u8g2_font_6x12_tf);
    g_display.setCursor(0, 16);
    g_display.print("CCS811 not ready");
    g_display.setCursor(0, 36);
    g_display.print(g_ccsRetired ? "retired" : "checking");
    g_display.sendBuffer();
    return;
  }
  snprintf(buf, sizeof(buf), "%u ppm", g_co2);
  printRow(0, "eCO2", buf);
  snprintf(buf, sizeof(buf), "%u ppb", g_voc);
  printRow(1, "TVOC", buf);

  // 400 (baseline) to 2000ppm (ventilation threshold) across the panel.
  const int lo = 400, hi = 2000;
  const int pct = (g_co2 <= lo) ? 0
                                : (g_co2 >= hi ? 100 : (g_co2 - lo) * 100 / (hi - lo));
  g_display.drawBox(0, 18, 128, 10);
  g_display.drawBox(1, 19, 126, 8);
  g_display.drawBox(2, 20, (126 * pct) / 100, 6);
  g_display.setFont(u8g2_font_6x12_tf);
  g_display.setCursor(0, 40);
  g_display.print(pct > 66 ? "poor air" : (pct > 33 ? "fair air" : "good air"));
  g_display.sendBuffer();
}

void pageStats() {
  g_display.clearBuffer();
  char buf[24];
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(state::visitors()));
  printRow(0, "Visitors", buf);
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(state::dailyVisitors()));
  printRow(1, "Today", buf);
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(state::guestbookApproved()));
  printRow(2, "Messages", buf);
  snprintf(buf, sizeof(buf), "%uK/%uK",
           static_cast<unsigned>(fsx::usedBytes() / 1024),
           static_cast<unsigned>(fsx::totalBytes() / 1024));
  printRow(3, "Flash", buf);
  g_display.sendBuffer();
}

void pagePower() {
  g_display.clearBuffer();
  char buf[24];
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(state::outagesTotal()));
  printRow(0, "Outages", buf);
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(state::outagesMonth()));
  printRow(1, "This month", buf);
  printRow(2, "Downtime");
  snprintf(buf, sizeof(buf), "%lum", static_cast<unsigned long>(state::outageSecondsTotal() / 60));
  printRow(3, "total", buf);
  if (!fsx::writable()) {
    g_display.setFont(u8g2_font_6x12_tf);
    g_display.setCursor(0, 48);
    g_display.print("STORAGE READ-ONLY");
  }
  g_display.sendBuffer();
}

void pageStatus() {
  g_display.clearBuffer();
  char buf[24];
  printRow(0, "HelloESP");
  snprintf(buf, sizeof(buf), "heap %uK",
           static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  printRow(1, buf);
  snprintf(buf, sizeof(buf), "%dd %dh",
           static_cast<int>(millis() / 86400000UL),
           static_cast<int>((millis() / 3600000UL) % 24));
  printRow(2, "Uptime", buf);
  g_display.setFont(u8g2_font_6x12_tf);
  g_display.setCursor(0, 48);
  g_display.print(fsx::health());
  g_display.sendBuffer();
}

}  // namespace

// --- lifecycle ------------------------------------------------------------

void begin() {
  Wire.begin(kSdaPin, kSclPin);
  Wire.setTimeOut(100);

  g_display.begin();
  g_display.setContrast(200);
  g_display.clearDisplay();
  g_display.sendBuffer();
  g_oledOk = true;

  loadHealth();

  // A missing sensor is reported, never fatal: the site still serves.
  g_bmeOk = g_bme.begin(kBmeAddr);
  if (!g_bmeOk) Serial.println("[sensor] BME280 not found at 0x76");

  g_ccsOk = g_ccs.begin(kCcsAddr, &Wire);
  if (!g_ccsOk) Serial.println("[sensor] CCS811 not found at 0x5A");
}

bool bmeHealthy() { return g_bmeOk && !g_bmeRetired; }
bool ccsHealthy() { return g_ccsOk && !g_ccsRetired; }
bool bmeRetired() { return g_bmeRetired; }
bool ccsRetired() { return g_ccsRetired; }
bool displayOk() { return g_oledOk; }

void clearRetired() {
  g_bmeRetired = false;
  g_ccsRetired = false;
  g_bmeBad = 0;
  g_ccsBad = 0;
  saveHealth();
}

bool recoverBus() {
  const uint32_t now = millis();
  if (g_lastRecover && (now - g_lastRecover) < kRecoverCooldown) return false;
  g_lastRecover = now;
  g_recovered = true;

  // A slave stuck mid-transmission keeps SDA low. Nine clock pulses make it
  // finish and release the line, which is cheaper and faster than a reboot.
  Wire.end();
  pinMode(kSdaPin, INPUT_PULLUP);
  pinMode(kSclPin, INPUT_PULLUP);
  for (int i = 0; i < 9; ++i) {
    digitalWrite(kSclPin, HIGH);
    delayMicroseconds(5);
    digitalWrite(kSclPin, LOW);
    delayMicroseconds(5);
  }
  Wire.begin(kSdaPin, kSclPin);
  Wire.setTimeOut(100);

  g_bmeOk = g_bme.begin(kBmeAddr);
  g_ccsOk = g_ccs.begin(kCcsAddr, &Wire);
  g_bmeLastGood = 0;
  g_ccsLastGood = 0;
  Serial.println("[hw] I2C bus re-seated");
  return true;
}

void read(Reading* out) {
  const uint32_t t0 = millis();

  if (!g_bmeRetired) {
    const float t = g_bme.readTemperature();
    const float h = g_bme.readHumidity();
    const float p = g_bme.readPressure() / 100.0f;
    // Accept only fully plausible triples. Partial acceptance is worse than
    // none: it would mix a fresh temperature with a stale humidity.
    if (inRange(t, kTempMinC, kTempMaxC) && inRange(h, kHumMin, kHumMax) &&
        inRange(p, kPressMin, kPressMax)) {
      g_tempC = t;
      g_hum = h;
      g_press = p;
      g_alt = util::altitudeFt(p);
      g_bmeLastGood = t0;
      g_bmeOk = true;
    } else if (!g_bmeLastGood || (t0 - g_bmeLastGood) > kSensorStaleMs) {
      g_bmeOk = false;
    }
  }

  if (!g_ccsRetired) {
    // The CCS811 needs humidity/temperature to compensate its output, and it
    // latches an internal error flag after a power glitch or bus disturbance.
    if (g_bmeOk) g_ccs.setEnvironmentalData(g_hum, g_tempC);
    const uint8_t err = g_ccs.readData();  // 0 = healthy
    if (err == 0) {
      const uint16_t eco2 = g_ccs.geteCO2();
      const uint16_t tvoc = g_ccs.getTVOC();
      if (eco2 <= kCo2Max && tvoc <= kVocMax) {
        g_co2 = eco2;
        g_voc = tvoc;
        g_ccsLastGood = t0;
        g_ccsOk = true;
      } else {
        g_ccsOk = false;
      }
    } else {
      g_ccsOk = false;
      // Throttle on a dedicated timestamp. Using g_ccsLastGood here would fire
      // on every single read whenever the sensor has never produced a good
      // value at all (that field is 0), spamming SWReset and the serial log.
      if (!g_lastCcsReset || (t0 - g_lastCcsReset) > kRecoverCooldown) {
        g_lastCcsReset = t0;
        Serial.printf("[sensor] CCS811 error 0x%02x, software reset\n", err);
        g_ccs.SWReset();
        delay(100);
        g_ccsOk = g_ccs.begin(kCcsAddr, &Wire);
      }
    }
    if (g_ccsLastGood && (t0 - g_ccsLastGood) > kSensorStaleMs) g_ccsOk = false;
  }

  g_readMs = millis() - t0;

  out->tempC = g_tempC;
  out->tempF = g_tempC * 9.0f / 5.0f + 32.0f;
  out->humidity = g_hum;
  out->pressure = g_press;
  out->altitude = g_alt;
  out->co2 = g_co2;
  out->voc = g_voc;
  out->heatIndex = util::heatIndexC(g_tempC, g_hum);
  out->bmeOk = g_bmeOk;
  out->ccsOk = g_ccsOk;
}

void noteCycleResult(bool bmeOkNow, bool ccsOkNow) {
  bool dirty = false;
  auto tick = [&](bool okNow, bool& retired, uint8_t& bad, const char* name) {
    if (retired) return;
    if (okNow) {
      bad = 0;
      return;
    }
    if (++bad >= kRetireCycles) {
      retired = true;
      Serial.printf("[sensor] %s retired after %u bad cycles\n", name, (unsigned)bad);
    }
  };
  tick(bmeOkNow, g_bmeRetired, g_bmeBad, "BME280");
  tick(ccsOkNow, g_ccsRetired, g_ccsBad, "CCS811");
  if (dirty) saveHealth();
}

// --- display --------------------------------------------------------------

void displayBegin() { g_oledOk = true; }

void displayCycle() {
  if (!g_oledOk) return;
  const uint32_t now = millis();
  if (now - g_lastPageSwitch < 10000) return;
  g_lastPageSwitch = now;
  switch (g_page) {
    case 0: pageClock();   break;
    case 1: pageSensors(); break;
    case 2: pageAir();     break;
    case 3: pageStats();   break;
    case 4: pagePower();    break;
    default: pageStatus(); break;
  }
  g_page = (g_page + 1) % 6;
}

}  // namespace sensors