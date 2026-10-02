#include "backup.h"

#include "board.h"

#include "relay.h"
#include "state.h"
#include "storage.h"
#include "util.h"

namespace backup {
namespace {

constexpr int  kHourLocal = 4;
constexpr uint32_t kIdleGapMs = 60000;

bool     g_running = false;
bool     g_requested = false;
bool     g_lastOk = false;
char     g_lastError[96] = "";
char     g_lastDate[16] = "";
uint32_t g_lastRunMs = 0;
uint32_t g_seq = 1;

// Exclusions, matched on the path and then the basename.
bool excludedPath(const char* abs, const char* base) {
  if (strcmp(abs, "/config.txt") == 0) return true;      // secrets stay local
  if (strcmp(base, "state.bin") == 0) return true;       // runtime blob, rebuilt
  if (strcmp(base, "sensor_health.bin") == 0) return true;
  const size_t n = strlen(base);
  if (n > 4 && (strcmp(base + n - 4, ".tmp") == 0 ||
                strcmp(base + n - 4, ".bak") == 0)) {
    return true;                                         // atomic-write debris
  }
  return false;
}

// Only the last N days of per-day sensor CSVs go into the bundle; the device
// keeps its own retention window and the Worker keeps a GFS rotation.
bool logInWindow(const char* abs, const char* base) {
  if (strncmp(abs, "/logs/", 6) != 0) return true;
  if (strlen(base) != 14 || strcmp(base + 10, ".csv") != 0) return true;
  struct tm t;
  if (!getLocalTime(&t, 0)) return true;
  const time_t cutoff = mktime(&t) - 6 * 86400;
  struct tm ct;
  localtime_r(&cutoff, &ct);
  char label[12];
  strftime(label, sizeof(label), "%Y-%m-%d", &ct);
  return memcmp(base, label, 10) >= 0;
}

void setError(const char* why) {
  snprintf(g_lastError, sizeof(g_lastError), "%s", why);
  g_lastOk = false;
}

void encodeB64(const uint8_t* in, size_t len, char* out, size_t outCap) {
  static const char kTbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < len && o + 5 < outCap; i += 3) {
    const uint32_t a = in[i];
    const uint32_t b = (i + 1 < len) ? in[i + 1] : 0;
    const uint32_t c = (i + 2 < len) ? in[i + 2] : 0;
    const uint32_t v = (a << 16) | (b << 8) | c;
    out[o++] = kTbl[(v >> 18) & 0x3F];
    out[o++] = kTbl[(v >> 12) & 0x3F];
    out[o++] = (i + 1 < len) ? kTbl[(v >> 6) & 0x3F] : '=';
    out[o++] = (i + 2 < len) ? kTbl[v & 0x3F] : '=';
  }
  out[o] = '\0';
}

// Total bytes we intend to send, for the manifest header.
size_t totalSize(const char* cutoff) {
  size_t total = 0;
  fsx::walk([&](const char* abs, const char* base, size_t size) {
    if (excludedPath(abs, base)) return true;
    if (!logInWindow(abs, base)) return true;
    total += size;
    return true;
  });
  return total;
}

void sendFile(const char* abs, const char* base, size_t size, uint32_t seq) {
  char safeBase[80];
  snprintf(safeBase, sizeof(safeBase), "%s", base);

  char name[256];
  snprintf(name, sizeof(name), "%s", abs + 1);  // strip the leading slash

  relay::pushBackupFileStart(seq, name, size);

  File f = fsx::vol().open(abs, FILE_READ);
  if (!f) {
    relay::pushBackupFileSkipped(seq, name, size, "unreadable");
    return;
  }
  static uint8_t block[576];
  static char b64[800];
  while (f.available()) {
    const size_t n = f.read(block, sizeof(block));
    if (n == 0) break;
    encodeB64(block, n, b64, sizeof(b64));
    relay::pushBackupFileChunk(seq, b64);
  }
  f.close();
  relay::pushBackupFileEnd(seq, name);
}

void runOnce() {
  if (!relay::connected()) {
    setError("worker offline");
    return;
  }
  // Push a fresh state snapshot first so the copy in R2 matches the device.
  state::commit(true);

  g_running = true;
  g_lastError[0] = '\0';

  char generatedAt[32];
  util::timestamp(generatedAt, sizeof(generatedAt));

  char date[12];
  snprintf(date, sizeof(date), "%.10s", generatedAt);

  char firmware[16];
  snprintf(firmware, sizeof(firmware), "%s", FIRMWARE_VERSION_STR);

  char uptime[40];
  const unsigned long s = millis() / 1000UL;
  snprintf(uptime, sizeof(uptime), "%lu days, %lu hours, %lu minutes, %lu seconds",
           s / 86400UL, (s / 3600UL) % 24, (s / 60UL) % 60, s % 60);

  const size_t total = totalSize(date);
  relay::pushBackupStart(g_seq, generatedAt, firmware, uptime, total);

  fsx::walk([&](const char* abs, const char* base, size_t size) {
    if (excludedPath(abs, base)) return true;
    if (!logInWindow(abs, base)) return true;
    if (!relay::connected()) return false;  // abort the walk
    sendFile(abs, base, size, g_seq);
    return true;
  });

  relay::pushBackupEnd(g_seq, total);
  ++g_seq;
  g_running = false;
  g_lastOk = relay::connected();
  if (!g_lastOk) setError("socket dropped mid-transfer");
  g_lastRunMs = millis();
  snprintf(g_lastDate, sizeof(g_lastDate), "%s", date);
  Serial.printf("[backup] %s (%u bytes)\n", g_lastOk ? "committed" : "incomplete",
                (unsigned)total);
}

}  // namespace

// Called by relay when the Worker confirms the R2 write landed.
void onCommitted(const char* date) {
  if (!date) return;
  snprintf(g_lastDate, sizeof(g_lastDate), "%s", date);
  fsx::writeText("/stats/last_backup.txt", g_lastDate);
  g_lastOk = true;
  snprintf(g_lastError, sizeof(g_lastError), "%s", "none");
}

void begin() {
  g_running = false;
  g_requested = false;
  g_lastOk = false;
  snprintf(g_lastError, sizeof(g_lastError), "%s", "none");
  char buf[16];
  if (fsx::readTrimmed("/stats/last_backup.txt", buf, sizeof(buf))) {
    snprintf(g_lastDate, sizeof(g_lastDate), "%s", buf);
  }
}

bool running() { return g_running; }
bool lastSucceeded() { return g_lastOk; }
const char* lastError() { return g_lastError[0] ? g_lastError : "none"; }
void request() { g_requested = true; }

void tick() { tick(false); }

void tick(bool forceNow) {
  if (!forceNow && !g_requested) {
    // Daily schedule: once the local clock passes 04:00 and today's date has
    // not been sent yet.
    if (!relay::connected()) return;
    struct tm t;
    if (!getLocalTime(&t, 0)) return;
    if (t.tm_hour < kHourLocal) return;
    char today[12];
    strftime(today, sizeof(today), "%Y-%m-%d", &t);
    if (strcmp(today, g_lastDate) == 0) return;
  }
  if (g_running) return;
  if (!relay::connected()) return;
  if (!forceNow && !g_requested && g_lastRunMs &&
      (millis() - g_lastRunMs) < kIdleGapMs) {
    return;
  }
  g_requested = false;
  runOnce();
}

}  // namespace backup