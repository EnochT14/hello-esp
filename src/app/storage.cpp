#include "storage.h"

#include <esp_system.h>

namespace fsx {
namespace {

// --- crash-loop guard -----------------------------------------------------
// Armed at the top of setup(), disarmed at the bottom. A panic restarts the
// chip through esp_restart(), which preserves RTC memory; a real power cut
// clears it. So a stamp that survives into the next boot proves the previous
// boot never finished.
constexpr uint32_t kStampMagic = 0xA5C0FFEEUL;
constexpr uint8_t  kCrashLimit = 2;
RTC_NOINIT_ATTR uint32_t rtcBootStamp = 0;
RTC_NOINIT_ATTR uint32_t rtcCrashRuns = 0;

enum SuspendReason : uint8_t {
  kSuspendNone = 0,
  kSuspendPanicLoop,
  kSuspendWriteFault,
  kSuspendMountFailed,
};

SuspendReason g_suspend = kSuspendNone;
bool g_mounted = false;
char g_health[64] = "uninitialised";

// --- blob framing ---------------------------------------------------------
// magic(4) version(1) length(2) crc32(4) payload(length)
constexpr size_t kBlobHeader = 11;

void setHealth(const char* s) { snprintf(g_health, sizeof(g_health), "%s", s); }

bool validName(const char* path) {
  if (!path || path[0] != '/') return false;
  if (strstr(path, "..") != nullptr) return false;
  for (const char* p = path; *p; ++p) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '/' || c == '.' ||
                    c == '_' || c == '-' || c == '+';
    if (!ok) return false;
  }
  // Reject a bare "/" and trailing slash: they are directories, not files.
  const size_t n = strlen(path);
  return n > 1 && path[n - 1] != '/';
}

}  // namespace

uint32_t crc32(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; ++i) {
    c ^= p[i];
    for (int b = 0; b < 8; ++b) {
      c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
    }
  }
  return c ^ 0xFFFFFFFFUL;
}

// --- mount ----------------------------------------------------------------

// Called first thing in setup(), before any filesystem access.
void armBootGuard() {
  if (rtcBootStamp == kStampMagic) {
    rtcCrashRuns++;
  } else {
    rtcCrashRuns = 0;
  }
  rtcBootStamp = kStampMagic;

  if (rtcCrashRuns >= kCrashLimit && g_suspend == kSuspendNone) {
    g_suspend = kSuspendPanicLoop;
    Serial.printf("[fs] writes suspended: %u consecutive boots never completed\n",
                  (unsigned)rtcCrashRuns);
  }
}

// Called once setup() has finished; the device is demonstrably not looping.
void disarmBootGuard() { rtcBootStamp = 0; }

unsigned bootFailures() { return rtcCrashRuns; }

bool begin() {
  if (g_suspend != kSuspendPanicLoop) {
    // formatOnFail rescues a bad superblock only. A torn directory pair still
    // mounts clean, which is exactly the case the boot guard exists for.
    g_mounted = LittleFS.begin(true);
  }

  if (!g_mounted && g_suspend == kSuspendPanicLoop) {
    // Read-only rescue: mount without formatting so the site can still serve
    // whatever is intact instead of looping on a freshly wiped partition.
    g_mounted = LittleFS.begin(false);
    if (g_mounted) {
      setHealth("read-only (panic loop)");
      Serial.println("[fs] mounted read-only; writes stay suspended");
    }
    return g_mounted;
  }

  if (!g_mounted) {
    // Genuinely unusable. Formatting here is safe precisely because it only
    // fires when there was nothing to read anyway.
    g_mounted = LittleFS.begin(true);
  }

  if (!g_mounted) {
    if (g_suspend == kSuspendNone) g_suspend = kSuspendMountFailed;
    setHealth("mount failed");
    return false;
  }

  if (g_suspend != kSuspendNone) {
    setHealth(g_suspend == kSuspendPanicLoop ? "read-only (panic loop)"
                                             : "read-only (write fault)");
  } else {
    setHealth("ok");
  }
  return true;
}

bool writable() { return g_mounted && g_suspend == kSuspendNone; }
bool writesSuspended() { return g_suspend != kSuspendNone; }
const char* health() { return g_health; }
size_t totalBytes() { return LittleFS.totalBytes(); }
size_t usedBytes() { return LittleFS.usedBytes(); }

// LittleFS has no size(path); open the file and ask.
static size_t fileSize(const char* path) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return 0;
  const size_t n = f.size();
  f.close();
  return n;
}

void suspendWrites(const char* why) {
  if (g_suspend != kSuspendNone) return;
  g_suspend = kSuspendWriteFault;
  snprintf(g_health, sizeof(g_health), "read-only (%s)", why ? why : "write fault");
  Serial.printf("[fs] writes suspended: %s\n", why ? why : "write fault");
}

// --- primitives -----------------------------------------------------------

bool exists(const char* path) { return g_mounted && validName(path) && LittleFS.exists(path); }

bool remove(const char* path) {
  if (!writable() || !validName(path)) return false;
  return LittleFS.remove(path);
}

bool rename(const char* from, const char* to) {
  if (!writable() || !validName(from) || !validName(to)) return false;
  return LittleFS.rename(from, to);
}

bool mkdirs(const char* dir) {
  if (!writable() || !dir || dir[0] != '/') return false;
  char tmp[96];
  snprintf(tmp, sizeof(tmp), "%s", dir);
  for (char* p = tmp + 1; *p; ++p) {
    if (*p != '/') continue;
    *p = '\0';
    if (!LittleFS.exists(tmp)) LittleFS.mkdir(tmp);
    *p = '/';
  }
  const size_t n = strlen(tmp);
  if (n && tmp[n - 1] == '/' && n > 1) tmp[n - 1] = '\0';
  if (!LittleFS.exists(tmp)) LittleFS.mkdir(tmp);
  return LittleFS.exists(tmp);
}

bool writeFile(const char* path, const void* data, size_t len) {
  if (!writable() || !validName(path)) return false;

  char tmp[112];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  File f = LittleFS.open(tmp, FILE_WRITE);
  if (!f) {
    suspendWrites("open tmp failed");
    return false;
  }
  const size_t wrote = (len > 0) ? f.write(static_cast<const uint8_t*>(data), len) : 0;
  f.close();
  if (wrote != len) {
    LittleFS.remove(tmp);
    suspendWrites("short write");
    return false;
  }

  // tmp is complete; only now disturb the live file.
  char bak[112];
  snprintf(bak, sizeof(bak), "%s.bak", path);
  LittleFS.remove(bak);
  if (LittleFS.exists(path)) LittleFS.rename(path, bak);
  if (!LittleFS.rename(tmp, path)) {
    LittleFS.remove(tmp);
    if (LittleFS.exists(bak)) LittleFS.rename(bak, path);  // roll back
    suspendWrites("rename failed");
    return false;
  }
  LittleFS.remove(bak);
  return true;
}

bool writeText(const char* path, const char* s) {
  return writeFile(path, s, s ? strlen(s) : 0);
}

bool append(const char* path, const void* data, size_t len) {
  if (!writable() || !validName(path) || len == 0) return false;
  const size_t before = fileSize(path);
  File f = LittleFS.open(path, FILE_APPEND);
  if (!f) {
    suspendWrites("append open failed");
    return false;
  }
  const size_t wrote = f.write(static_cast<const uint8_t*>(data), len);
  f.close();
  if (wrote != len) {
    suspendWrites("short append");
    return false;
  }
  const size_t after = fileSize(path);
  if (before + len != after) {
    suspendWrites("append size mismatch");
    return false;
  }
  return true;
}

bool appendText(const char* path, const char* s) {
  return append(path, s, s ? strlen(s) : 0);
}

bool readAll(const char* path, std::string& out, size_t maxLen) {
  out.clear();
  if (!g_mounted || !validName(path)) return false;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  const size_t sz = f.size();
  if (sz > maxLen) {
    f.close();
    return false;
  }
  out.reserve(sz);
  uint8_t buf[256];
  size_t got;
  while ((got = f.read(buf, sizeof(buf))) > 0) out.append(reinterpret_cast<char*>(buf), got);
  f.close();
  return true;
}

bool readLine(const char* path, char* buf, size_t bufLen) {
  if (!bufLen) return false;
  buf[0] = '\0';
  if (!g_mounted || !validName(path)) return false;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  // Read a bounded prefix and cut at the newline ourselves: this avoids
  // depending on the String-returning readStringUntil overload, which pulls a
  // heap allocation into every read.
  const size_t cap = bufLen - 1;
  const size_t got = f.read(reinterpret_cast<uint8_t*>(buf), cap);
  buf[got] = '\0';
  f.close();
  char* nl = strchr(buf, '\n');
  if (nl) *nl = '\0';
  size_t n = strlen(buf);
  while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
    buf[--n] = '\0';
  }
  return true;
}

bool readTrimmed(const char* path, char* buf, size_t bufLen) {
  if (!readLine(path, buf, bufLen)) return false;
  char* p = buf;
  while (*p == ' ' || *p == '\t') ++p;
  if (p != buf) memmove(buf, p, strlen(p) + 1);
  return true;
}

// --- crc-protected blob ---------------------------------------------------

bool writeBlob(const char* path, uint8_t version, const void* data, size_t len) {
  if (!validName(path)) return false;
  const size_t total = kBlobHeader + len;
  if (total > 1024) return false;

  uint8_t buf[1024];
  memcpy(buf, "3ESB", 4);
  buf[4] = version;
  buf[5] = static_cast<uint8_t>(len & 0xFF);
  buf[6] = static_cast<uint8_t>((len >> 8) & 0xFF);
  memcpy(buf + 7, data, len);
  const uint32_t sum = crc32(data, len);
  memcpy(buf + 7 + len, &sum, 4);
  return writeFile(path, buf, total);
}

bool readBlob(const char* path, uint8_t* version, void* out, size_t len) {
  if (!validName(path)) return false;

  auto tryOne = [&](const char* p) -> bool {
    File f = LittleFS.open(p, FILE_READ);
    if (!f) return false;

    // Read the header first: it carries the real payload length. Requiring the
    // file to be exactly `len` bytes would fail for any blob whose payload is
    // shorter than the buffer it was written from.
    uint8_t head[kBlobHeader];
    if (f.read(head, kBlobHeader) != kBlobHeader) {
      f.close();
      return false;
    }
    if (memcmp(head, "3ESB", 4) != 0) {
      f.close();
      return false;
    }
    const size_t stored = static_cast<size_t>(head[5]) | (static_cast<size_t>(head[6]) << 8);
    if (stored > len || head[7 - 3] != head[7 - 3]) {  // length sanity
      f.close();
      return false;
    }
    const size_t total = kBlobHeader + stored;
    if (f.size() != total) {
      f.close();
      return false;
    }

    uint8_t tail[kBlobHeader + 512];
    if (kBlobHeader + stored > sizeof(tail)) {
      f.close();
      return false;
    }
    // Re-read from the start so header and payload are contiguous.
    f.close();
    f = LittleFS.open(p, FILE_READ);
    if (!f) return false;
    const size_t got = f.read(tail, total);
    f.close();
    if (got != total) return false;

    uint32_t sum;
    memcpy(&sum, tail + 7 + stored, 4);
    if (crc32(tail + 7, stored) != sum) return false;
    if (version) *version = tail[4];
    memcpy(out, tail + 7, stored);
    return true;
  };

  if (tryOne(path)) return true;

  // Torn or stale primary: promote the backup if it is sound.
  char bak[112];
  snprintf(bak, sizeof(bak), "%s.bak", path);
  if (tryOne(bak)) {
    Serial.println("[fs] recovered state from .bak");
    return true;
  }
  return false;
}

// --- iteration ------------------------------------------------------------

void walk(const std::function<bool(const char*, const char*, size_t)>& fn) {
  if (!g_mounted) return;

  // Explicit stack rather than recursion: the loop task's stack is precious.
  struct Level {
    char dir[64];
  };
  // Deep enough for /logs/2026 and /stats/weekly/2026 with slack. The previous 6
  // filled up and every remaining directory was dropped without a word, which
  // is why the backup saw only the root files and never a log or archive CSV.
  Level stack[12];
  int sp = 0;
  snprintf(stack[sp].dir, sizeof(stack[0].dir), "/");
  ++sp;

  while (sp > 0) {
    const int idx = --sp;
    char dir[64];
    snprintf(dir, sizeof(dir), "%s", stack[idx].dir);

    File d = LittleFS.open(dir);
    if (!d) continue;

    // Canonical ESP32 iteration: the loop variable is advanced by
    // openNextFile() itself. The previous form called child.close() and then
    // re-assigned a copy-initialised File, which left the iteration's file
    // handle in a bad state and stopped the listing after 17 entries - exactly
    // the number of root files before the first subdirectory.
    for (File child = d.openNextFile(); child; child = d.openNextFile()) {
      String name = child.name();
      const bool isDir = child.isDirectory();
      const size_t size = child.size();
      const int slash = name.lastIndexOf('/');
      String bare = (slash >= 0) ? name.substring(slash + 1) : name;

      char abs[160];
      if (strcmp(dir, "/") == 0) {
        snprintf(abs, sizeof(abs), "/%s", bare.c_str());
      } else {
        snprintf(abs, sizeof(abs), "%s/%s", dir, bare.c_str());
      }

      if (isDir) {
        if (sp < static_cast<int>(sizeof(stack) / sizeof(stack[0]))) {
          snprintf(stack[sp].dir, sizeof(stack[0].dir), "%s", abs);
          ++sp;
        } else {
          // Dropping a directory silently loses everything beneath it.
          Serial.printf("[fs] walk depth exceeded, skipping %s\n", abs);
        }
      } else if (!fn(abs, bare.c_str(), size)) {
        break;
      }
    }
    d.close();
  }
}

fs::FS& vol() { return LittleFS; }

}  // namespace fsx