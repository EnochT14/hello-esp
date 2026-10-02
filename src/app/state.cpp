#include "state.h"

#include <math.h>
#include <time.h>

#include "config.h"
#include "storage.h"
#include "util.h"

namespace state {
namespace {

// Fixed-width little-endian blob. The size is a compile-time constant so a
// state.bin written by one build always loads in the next, regardless of
// struct padding changes.
constexpr uint8_t  kBlobVersion = 2;
constexpr uint32_t kBlobMagic   = 0x42534548UL;  // "HESB"
constexpr size_t   kBlobCap     = 512;
constexpr uint16_t kMaxCountries = 160;
constexpr uint32_t kOutageMinSecs = 90;
constexpr uint32_t kOutageMaxSecs = 180u * 86400u;
constexpr uint16_t kPowerEventsMaxLines = 500;

struct Rec { float value; char at[28]; uint8_t set; uint8_t pad[3]; };

struct Ram {
  uint32_t visitors, dailyVisitors, dailyDate, lastSeen;
  uint8_t  gbApproved, gbPending, gbAll, pad0;
  uint32_t outagesTotal, outagesMonth, lastOutageSecs, lastOutageAt;
  uint64_t outageSecsTotal, outageSecsMonth;
  Rec      rec[5];
  Period   week, month, year;
  char     weekLabel[10], monthLabel[9], yearLabel[6];
};

Ram        g;
bool       g_dirty = false;
bool       g_clockOk = false;
bool       g_dumsorChecked = false;
bool       g_countriesDirty = false;
unsigned long g_lastCommitMs = 0;
uint32_t   g_intervalReqs = 0;
uint32_t   g_intervalPeak = 0;

char g_countries[kMaxCountries][3];
uint32_t g_countryHits[kMaxCountries];
uint16_t g_countryCount = 0;

const char kStatePath[]      = "/state.bin";
const char kCountriesPath[]  = "/countries.csv";
const char kPowerEventsPath[] = "/power_events.csv";

// --- explicit little-endian codec ----------------------------------------
inline void put32(uint8_t*& p, uint32_t v) { memcpy(p, &v, 4); p += 4; }
inline void put16(uint8_t*& p, uint16_t v) { memcpy(p, &v, 2); p += 2; }
inline void put64(uint8_t*& p, uint64_t v) { memcpy(p, &v, 8); p += 8; }
inline void putf32(uint8_t*& p, float v)    { memcpy(p, &v, 4); p += 4; }
inline void putf64(uint8_t*& p, double v)   { memcpy(p, &v, 8); p += 8; }
inline void putstr(uint8_t*& p, const char* s, size_t n) {
  memcpy(p, s, n);
  p += n;
}
inline uint32_t get32(const uint8_t*& p) { uint32_t v; memcpy(&v, p, 4); p += 4; return v; }
inline uint16_t get16(const uint8_t*& p) { uint16_t v; memcpy(&v, p, 2); p += 2; return v; }
inline uint64_t get64(const uint8_t*& p) { uint64_t v; memcpy(&v, p, 8); p += 8; return v; }
inline float    getf32(const uint8_t*& p) { float v;    memcpy(&v, p, 4); p += 4; return v; }
inline double   getf64(const uint8_t*& p) { double v;   memcpy(&v, p, 8); p += 8; return v; }
inline void getstr(const uint8_t*& p, char* dst, size_t n) {
  memcpy(dst, p, n);
  dst[n - 1] = '\0';
  p += n;
}

inline void putPeriod(uint8_t*& p, const Period& s) {
  put32(p, s.visitors); put32(p, s.guestbook); put32(p, s.peakReqs);
  putf32(p, s.tempMin); putf32(p, s.tempMax); putf64(p, s.tempSum);
  putf32(p, s.humMin); putf32(p, s.humMax); putf64(p, s.humSum);
  put16(p, s.co2Min); put16(p, s.co2Max); put32(p, s.co2Sum);
  put16(p, s.vocMin); put16(p, s.vocMax); put32(p, s.vocSum);
  put32(p, s.samples); put32(p, s.startedUnix);
}
inline Period getPeriod(const uint8_t*& p) {
  Period s;
  s.visitors = get32(p); s.guestbook = get32(p); s.peakReqs = get32(p);
  s.tempMin = getf32(p); s.tempMax = getf32(p); s.tempSum = getf64(p);
  s.humMin = getf32(p); s.humMax = getf32(p); s.humSum = getf64(p);
  s.co2Min = get16(p); s.co2Max = get16(p); s.co2Sum = get32(p);
  s.vocMin = get16(p); s.vocMax = get16(p); s.vocSum = get32(p);
  s.samples = get32(p); s.startedUnix = get32(p);
  return s;
}

size_t serialize(uint8_t* buf) {
  uint8_t* p = buf;
  put32(p, kBlobMagic);
  put16(p, kBlobVersion);
  put32(p, g.visitors);
  put32(p, g.dailyVisitors);
  put32(p, g.dailyDate);
  put32(p, g.lastSeen);
  put32(p, ((uint32_t)g.gbApproved) | ((uint32_t)g.gbPending << 8) |
              ((uint32_t)g.gbAll << 16) | ((uint32_t)g.pad0 << 24));
  put32(p, g.outagesTotal);
  put32(p, g.outagesMonth);
  put32(p, g.lastOutageSecs);
  put32(p, g.lastOutageAt);
  put64(p, g.outageSecsTotal);
  put64(p, g.outageSecsMonth);
  for (int i = 0; i < 5; ++i) {
    putf32(p, g.rec[i].value);
    putstr(p, g.rec[i].at, 28);
    const uint32_t s = g.rec[i].set ? 1u : 0u;
    put32(p, s);
  }
  putPeriod(p, g.week);
  putPeriod(p, g.month);
  putPeriod(p, g.year);
  putstr(p, g.weekLabel, 10);
  putstr(p, g.monthLabel, 9);
  putstr(p, g.yearLabel, 6);
  return static_cast<size_t>(p - buf);
}

bool deserialize(const uint8_t* buf, size_t len) {
  if (len < 16) return false;
  const uint8_t* p = buf;
  if (get32(p) != kBlobMagic) return false;
  if (get16(p) != kBlobVersion) return false;
  g.visitors = get32(p);
  g.dailyVisitors = get32(p);
  g.dailyDate = get32(p);
  g.lastSeen = get32(p);
  const uint32_t gb = get32(p);
  g.gbApproved = static_cast<uint8_t>(gb & 0xFF);
  g.gbPending  = static_cast<uint8_t>((gb >> 8) & 0xFF);
  g.gbAll      = static_cast<uint8_t>((gb >> 16) & 0xFF);
  g.outagesTotal = get32(p);
  g.outagesMonth = get32(p);
  g.lastOutageSecs = get32(p);
  g.lastOutageAt = get32(p);
  g.outageSecsTotal = get64(p);
  g.outageSecsMonth = get64(p);
  if (static_cast<size_t>(p - buf) + 5 * 36 + 3 * 76 + 25 > len) return false;
  for (int i = 0; i < 5; ++i) {
    g.rec[i].value = getf32(p);
    getstr(p, g.rec[i].at, 28);
    g.rec[i].set = get32(p) != 0;
  }
  g.week = getPeriod(p);
  g.month = getPeriod(p);
  g.year = getPeriod(p);
  getstr(p, g.weekLabel, 10);
  getstr(p, g.monthLabel, 9);
  getstr(p, g.yearLabel, 6);
  return true;
}

// --- countries ------------------------------------------------------------
void saveCountries() {
  char buf[1024];
  size_t o = 0;
  for (uint16_t i = 0; i < g_countryCount; ++i) {
    const int n = snprintf(buf + o, sizeof(buf) - o, "%s,%u\r\n",
                           g_countries[i], (unsigned)g_countryHits[i]);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(buf) - o) break;
    o += static_cast<size_t>(n);
  }
  if (fsx::writeText(kCountriesPath, buf)) {
    g_countriesDirty = false;
  }
}

void loadCountries() {
  std::string raw;
  if (!fsx::readAll(kCountriesPath, raw, 4096)) return;
  const char* p = raw.c_str();
  while (*p && g_countryCount < kMaxCountries) {
    const char* eol = strchr(p, '\n');
    const size_t len = eol ? static_cast<size_t>(eol - p) : strlen(p);
    if (len >= 4 && len < 24 && p[2] == ',') {
      const uint32_t hits = strtoul(p + 3, nullptr, 10);
      if (hits > 0) {
        g_countries[g_countryCount][0] = p[0];
        g_countries[g_countryCount][1] = p[1];
        g_countries[g_countryCount][2] = '\0';
        g_countryHits[g_countryCount] = hits;
        ++g_countryCount;
      }
    }
    if (!eol) break;
    p = eol + 1;
  }
}

// --- period helpers -------------------------------------------------------
// Days since 1970-01-01 from a broken-down local time. Hand-rolled instead of
// timegm(), which is not declared on this toolchain.
uint32_t daysSinceEpoch() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return 0;
  int32_t y = t.tm_year + 1900;
  const uint32_t m = static_cast<uint32_t>(t.tm_mon + 1);
  const uint32_t d = static_cast<uint32_t>(t.tm_mday);
  y -= (m <= 2) ? 1 : 0;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<uint32_t>(era * 146097 + static_cast<int32_t>(doe) - 719468);
}

void labelFor(char* out, size_t cap, const char* fmt) {
  struct tm t;
  if (!getLocalTime(&t, 0)) {
    out[0] = '\0';
    return;
  }
  strftime(out, cap, fmt, &t);
}

void resetPeriod(Period& p) {
  p.reset();
  p.startedUnix = 0;
  struct tm t;
  if (getLocalTime(&t, 0)) {
    struct tm midnight = t;
    midnight.tm_hour = 0;
    midnight.tm_min = 0;
    midnight.tm_sec = 0;
    p.startedUnix = static_cast<uint32_t>(daysSinceEpoch() * 86400);
  }
}

void flushArchive(const char* dir, const char* label, const Period& p) {
  if (label[0] == '\0') return;
  fsx::mkdirs(dir);
  char file[112];
  snprintf(file, sizeof(file), "%s/%s.json", dir, label);
  char json[640];
  p.toJson(json, sizeof(json), label);
  fsx::writeText(file, json);
}

// --- power_events trimming ------------------------------------------------
void trimPowerEvents() {
  std::string raw;
  if (!fsx::readAll(kPowerEventsPath, raw, 128 * 1024)) return;
  // Count lines, then keep the header plus the newest kPowerEventsMaxLines-1.
  uint16_t lines = 0;
  for (char c : raw) {
    if (c == '\n') ++lines;
  }
  if (lines <= kPowerEventsMaxLines) return;

  const uint16_t skip = static_cast<uint16_t>(lines - (kPowerEventsMaxLines - 1));
  size_t start = 0;
  uint16_t seen = 0;
  // Always keep the first (header) line verbatim.
  const size_t headerEnd = raw.find('\n');
  if (headerEnd == std::string::npos) return;
  std::string out = raw.substr(0, headerEnd + 1);
  start = headerEnd + 1;
  for (uint16_t i = 0; i < skip; ++i) {
    const size_t nl = raw.find('\n', start);
    if (nl == std::string::npos) return;
    start = nl + 1;
    ++seen;
  }
  (void)seen;
  out.append(raw, start, std::string::npos);
  fsx::writeText(kPowerEventsPath, out.c_str());
}

// --- legacy migration -----------------------------------------------------
// Reads the v1.5 file set so an existing device keeps its numbers across the
// rewrite. Runs once, only when /state.bin is missing.

// v1.5 PeriodStats: a 72-byte little-endian struct with 4 bytes of padding at
// offset 20 before the double-aligned tempSum. Field offsets are hardcoded
// rather than memcpy'd from a struct so the read cannot drift with our own
// compiler layout.
struct LegacyPeriod {
  bool ok = false;
  uint32_t visitors, guestbook, peakReqs;
  float tempMin, tempMax; double tempSum;
  float humMin, humMax; double humSum;
  uint16_t co2Min, co2Max; uint32_t co2Sum;
  uint16_t vocMin, vocMax; uint32_t vocSum;
  uint32_t samples, started;
};

LegacyPeriod readLegacyPeriod(const uint8_t* p) {
  LegacyPeriod l;
  auto u32 = [&](size_t off) { uint32_t v; memcpy(&v, p + off, 4); return v; };
  auto f32 = [&](size_t off) { float v; memcpy(&v, p + off, 4); return v; };
  auto f64 = [&](size_t off) { double v; memcpy(&v, p + off, 8); return v; };
  auto u16 = [&](size_t off) { uint16_t v; memcpy(&v, p + off, 2); return v; };
  l.visitors = u32(0); l.guestbook = u32(4); l.peakReqs = u32(8);
  l.tempMin = f32(12); l.tempMax = f32(16); l.tempSum = f64(24);
  l.humMin = f32(32); l.humMax = f32(36); l.humSum = f64(40);
  l.co2Min = u16(48); l.co2Max = u16(50); l.co2Sum = u32(52);
  l.vocMin = u16(56); l.vocMax = u16(58); l.vocSum = u32(60);
  l.samples = u32(64); l.started = u32(68);
  l.ok = true;
  return l;
}

void applyLegacy(const LegacyPeriod& l, Period& out) {
  if (!l.ok) return;
  out.visitors = l.visitors;
  out.guestbook = l.guestbook;
  out.peakReqs = l.peakReqs;
  out.tempMin = l.tempMin; out.tempMax = l.tempMax; out.tempSum = l.tempSum;
  out.humMin = l.humMin; out.humMax = l.humMax; out.humSum = l.humSum;
  out.co2Min = l.co2Min; out.co2Max = l.co2Max; out.co2Sum = l.co2Sum;
  out.vocMin = l.vocMin; out.vocMax = l.vocMax; out.vocSum = l.vocSum;
  out.samples = l.samples;
  out.startedUnix = l.started;
}

void legacyRecord(const std::string& json, const char* key, Rec& out) {
  const std::string k = std::string("\"") + key + "\":";
  size_t at = json.find(k);
  if (at == std::string::npos) return;
  const size_t v = json.find("\"value\":", at);
  const size_t a = json.find("\"at\":\"", at);
  if (v == std::string::npos || a == std::string::npos) return;
  const size_t vStart = v + 8;
  const size_t vEnd = json.find_first_of(",}", vStart);
  if (vEnd == std::string::npos || vEnd <= vStart) return;
  const size_t aStart = a + 6;
  const size_t aEnd = json.find('"', aStart);
  if (aEnd == std::string::npos || aEnd <= aStart) return;
  out.value = strtof(json.substr(vStart, vEnd - vStart).c_str(), nullptr);
  snprintf(out.at, sizeof(out.at), "%s", json.substr(aStart, aEnd - aStart).c_str());
  out.set = 1;
}

void tallyLegacyPowerEvents() {
  std::string raw;
  if (!fsx::readAll(kPowerEventsPath, raw, 128 * 1024)) return;
  const time_t nowSec = time(nullptr);
  struct tm nowTm;
  localtime_r(&nowSec, &nowTm);

  size_t pos = 0;
  while (pos < raw.size()) {
    size_t eol = raw.find('\n', pos);
    if (eol == std::string::npos) eol = raw.size();
    const std::string line = raw.substr(pos, eol - pos);
    pos = eol + 1;
    if (line.size() < 8 || line.size() > 48 || line[0] == 'o') continue;

    const size_t c1 = line.find(',');
    if (c1 == std::string::npos) continue;
    const size_t c2 = line.find(',', c1 + 1);
    if (c2 == std::string::npos) continue;
    const uint32_t start = strtoul(line.substr(0, c1).c_str(), nullptr, 10);
    const uint32_t restored = strtoul(line.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10);
    const uint32_t secs = strtoul(line.substr(c2 + 1).c_str(), nullptr, 10);
    if (start == 0 || restored <= start || secs != restored - start) continue;
    if (secs < kOutageMinSecs || secs > kOutageMaxSecs) continue;

    ++g.outagesTotal;
    g.outageSecsTotal += secs;
    const time_t st = static_cast<time_t>(start);
    struct tm et;
    localtime_r(&st, &et);
    if (et.tm_year == nowTm.tm_year && et.tm_mon == nowTm.tm_mon) {
      ++g.outagesMonth;
      g.outageSecsMonth += secs;
    }
    if (restored > g.lastOutageAt) {
      g.lastOutageAt = restored;
      g.lastOutageSecs = secs;
    }
  }
}

void migrateLegacy() {
  char buf[40];

  if (fsx::readTrimmed("/visitors.txt", buf, sizeof(buf))) {
    g.visitors = static_cast<uint32_t>(strtoul(buf, nullptr, 10));
  }
  if (fsx::readTrimmed("/daily.txt", buf, sizeof(buf))) {
    const char* comma = strchr(buf, ',');
    if (comma) g.dailyVisitors = static_cast<uint32_t>(strtoul(comma + 1, nullptr, 10));
  }
  if (fsx::readTrimmed("/lastseen.txt", buf, sizeof(buf))) {
    g.lastSeen = static_cast<uint32_t>(strtoul(buf, nullptr, 10));
  }

  // Legacy checkpoint: magic(4) ver(2) week@6 month@78 year@150 labels@222.
  std::string ck;
  if (fsx::readAll("/stats/checkpoint.bin", ck, 512)) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(ck.data());
    if (ck.size() >= 222 && memcmp(p, "3SEH", 4) == 0) {
      applyLegacy(readLegacyPeriod(p + 6), g.week);
      applyLegacy(readLegacyPeriod(p + 78), g.month);
      applyLegacy(readLegacyPeriod(p + 150), g.year);
      memcpy(g.weekLabel, p + 222, 9);
      g.weekLabel[9] = '\0';
      memcpy(g.monthLabel, p + 231, 8);
      g.monthLabel[8] = '\0';
      memcpy(g.yearLabel, p + 239, 5);
      g.yearLabel[5] = '\0';
    }
  }

  std::string recs;
  if (fsx::readAll("/stats/records.json", recs, 8192)) {
    legacyRecord(recs, "highest_co2", g.rec[0]);
    legacyRecord(recs, "highest_temp_f", g.rec[1]);
    legacyRecord(recs, "lowest_temp_f", g.rec[2]);
    legacyRecord(recs, "most_visitors_day", g.rec[3]);
    legacyRecord(recs, "longest_uptime_d", g.rec[4]);
  }

  tallyLegacyPowerEvents();
  loadCountries();

  Serial.printf("[state] migrated v1.5: visitors=%u countries=%u week=%u month=%u\n",
                (unsigned)g.visitors, (unsigned)g_countryCount,
                (unsigned)g.week.visitors, (unsigned)g.month.visitors);
}

}  // namespace

// --- Period ---------------------------------------------------------------

void Period::reset() {
  visitors = guestbook = peakReqs = 0;
  tempMin = 1000.0f; tempMax = -1000.0f; tempSum = 0.0;
  humMin = 1000.0f; humMax = -1000.0f; humSum = 0.0;
  co2Min = 0xFFFF; co2Max = 0; co2Sum = 0;
  vocMin = 0xFFFF; vocMax = 0; vocSum = 0;
  samples = 0;
}

void Period::addSample(float t, float h, uint16_t co2, uint16_t voc, uint32_t reqs) {
  // Reject NaN outright: it compares false against everything, so the min/max
  // guards would pass it through and then poison every later average.
  if (isnan(t) || isnan(h)) return;
  if (t < tempMin) tempMin = t;
  if (t > tempMax) tempMax = t;
  tempSum += t;
  if (h < humMin) humMin = h;
  if (h > humMax) humMax = h;
  humSum += h;
  if (co2 > 0 && co2 < co2Min) co2Min = co2;
  if (co2 > co2Max) co2Max = co2;
  co2Sum += co2;
  if (voc > 0 && voc < vocMin) vocMin = voc;
  if (voc > vocMax) vocMax = voc;
  vocSum += voc;
  ++samples;
  if (reqs > peakReqs) peakReqs = reqs;
}

void Period::toJson(char* out, size_t cap, const char* label) const {
  int n = snprintf(out, cap,
      "{\"label\":\"%s\",\"visitors\":%u,\"guestbook\":%u,\"peak_reqs\":%u,\"samples\":%u",
      label ? label : "", (unsigned)visitors, (unsigned)guestbook,
      (unsigned)peakReqs, (unsigned)samples);
  if (n < 0 || static_cast<size_t>(n) >= cap) return;
  if (samples > 0) {
    if (tempMin <= tempMax) {
      n += snprintf(out + n, cap - n,
          ",\"temp_c\":{\"min\":%.2f,\"max\":%.2f,\"avg\":%.2f}",
          tempMin, tempMax, (float)(tempSum / samples));
    }
    if (humMin <= humMax) {
      n += snprintf(out + n, cap - n,
          ",\"humidity\":{\"min\":%.1f,\"max\":%.1f,\"avg\":%.1f}",
          humMin, humMax, (float)(humSum / samples));
    }
    if (co2Min != 0xFFFF && co2Max > 0) {
      n += snprintf(out + n, cap - n,
          ",\"co2_ppm\":{\"min\":%u,\"max\":%u,\"avg\":%lu}",
          (unsigned)co2Min, (unsigned)co2Max, (unsigned long)(co2Sum / samples));
    }
    if (vocMin != 0xFFFF && vocMax > 0) {
      n += snprintf(out + n, cap - n,
          ",\"voc_ppb\":{\"min\":%u,\"max\":%u,\"avg\":%lu}",
          (unsigned)vocMin, (unsigned)vocMax, (unsigned long)(vocSum / samples));
    }
  }
  snprintf(out + n, cap - n, ",\"started\":%lu}", (unsigned long)startedUnix);
}

// --- lifecycle ------------------------------------------------------------

void begin() {
  g = Ram();
  g_countriesDirty = false;
  resetPeriod(g.week);
  resetPeriod(g.month);
  resetPeriod(g.year);

  uint8_t version = 0;
  static uint8_t buf[kBlobCap];
  if (fsx::readBlob(kStatePath, &version, buf, sizeof(buf))) {
    if (deserialize(buf, sizeof(buf))) {
      // Countries live in their own file, so they are loaded on every boot -
      // not only during the one-off migration.
      loadCountries();
      Serial.printf("[state] loaded: visitors=%u countries=%u\n",
                    (unsigned)g.visitors, (unsigned)g_countryCount);
      return;
    }
    Serial.println("[state] corrupt state blob, falling back to migration");
  }

  migrateLegacy();
  g_dirty = true;
  commit(true);
}

bool dirty() { return g_dirty; }

bool commit(bool force, unsigned long minIntervalMs) {
  if (!g_dirty && !g_countriesDirty && !force) return false;
  if (!force && (millis() - g_lastCommitMs) < minIntervalMs) return false;
  if (!fsx::writable()) return false;

  if (g_countriesDirty) saveCountries();

  if (g_dirty || force) {
    uint8_t buf[kBlobCap];
    const size_t len = serialize(buf);
    if (!fsx::writeBlob(kStatePath, kBlobVersion, buf, len)) return false;
    g_dirty = false;
  }

  // Mirror the hot counters back out in the v1.5 file shapes so a restore from
  // an older R2 bundle still lands on a device that understands it.
  char tmp[24];
  if (force || g.visitors != 0) {
    snprintf(tmp, sizeof(tmp), "%u\r\n", (unsigned)g.visitors);
    fsx::writeText("/visitors.txt", tmp);
  }
  if (g.lastSeen) {
    snprintf(tmp, sizeof(tmp), "%u\n", (unsigned)g.lastSeen);
    fsx::writeText("/lastseen.txt", tmp);
  }

  g_lastCommitMs = millis();
  return true;
}

void setClockOk(bool ok) { g_clockOk = ok; }
bool clockOk() { return g_clockOk; }

// --- visitors -------------------------------------------------------------

uint32_t visitors() { return g.visitors; }
uint32_t dailyVisitors() { return g.dailyVisitors; }
uint32_t countriesTracked() { return g_countryCount; }

void resetVisitors() {
  g.visitors = 0;
  g.dailyVisitors = 0;
  g.week.visitors = 0;
  g.month.visitors = 0;
  g.year.visitors = 0;
  g.rec[3].set = 0;
  g.rec[3].value = 0;
  g.rec[3].at[0] = '\0';
  for (uint16_t i = 0; i < kMaxCountries; ++i) g_countryHits[i] = 0;
  g_countryCount = 0;
  g_countriesDirty = true;
  g_dirty = true;
  commit(true);
}

void countVisit(const char* country) {
  ++g.visitors;
  ++g.dailyVisitors;
  ++g.week.visitors;
  ++g.month.visitors;
  ++g.year.visitors;
  g_dirty = true;

  if (country && country[0] == '?') return;
  for (uint16_t i = 0; i < g_countryCount; ++i) {
    if (memcmp(g_countries[i], country, 2) == 0) {
      if (g_countryHits[i] < 0xFFFFFFFFUL) {
        ++g_countryHits[i];
        g_countriesDirty = true;
      }
      return;
    }
  }
  if (g_countryCount < kMaxCountries) {
    g_countries[g_countryCount][0] = country[0];
    g_countries[g_countryCount][1] = country[1];
    g_countries[g_countryCount][2] = '\0';
    g_countryHits[g_countryCount] = 1;
    ++g_countryCount;
    g_countriesDirty = true;
  }
}

// --- guestbook counters ---------------------------------------------------

uint32_t guestbookApproved() { return g.gbApproved; }
uint32_t guestbookPending() { return g.gbPending; }
uint32_t guestbookAll() { return g.gbAll; }

void adjustGuestbook(int32_t pending, int32_t approved, int32_t all) {
  g.gbPending = static_cast<uint8_t>(std::max<int32_t>(0, g.gbPending + pending));
  g.gbApproved = static_cast<uint8_t>(std::max<int32_t>(0, g.gbApproved + approved));
  g.gbAll = static_cast<uint8_t>(std::max<int32_t>(0, g.gbAll + all));
  g_dirty = true;
}

// --- periods --------------------------------------------------------------

Period& week() { return g.week; }
Period& month() { return g.month; }
Period& year() { return g.year; }
const char* weekLabel() { return g.weekLabel; }
const char* monthLabel() { return g.monthLabel; }
const char* yearLabel() { return g.yearLabel; }

void rollPeriods() {
  char newWeek[10] = "", newMonth[9] = "", newYear[6] = "";
  labelFor(newWeek, sizeof(newWeek), "%G-W%V");
  labelFor(newMonth, sizeof(newMonth), "%Y-%m");
  labelFor(newYear, sizeof(newYear), "%Y");

  if (g.weekLabel[0] == '\0') {
    snprintf(g.weekLabel, sizeof(g.weekLabel), "%s", newWeek);
    g_dirty = true;
  } else if (strcmp(g.weekLabel, newWeek) != 0) {
    flushArchive("/stats/weekly", g.weekLabel, g.week);
    snprintf(g.weekLabel, sizeof(g.weekLabel), "%s", newWeek);
    resetPeriod(g.week);
    g_dirty = true;
  }

  if (g.monthLabel[0] == '\0') {
    snprintf(g.monthLabel, sizeof(g.monthLabel), "%s", newMonth);
    g_dirty = true;
  } else if (strcmp(g.monthLabel, newMonth) != 0) {
    flushArchive("/stats/monthly", g.monthLabel, g.month);
    snprintf(g.monthLabel, sizeof(g.monthLabel), "%s", newMonth);
    resetPeriod(g.month);
    g_dirty = true;
  }

  if (g.yearLabel[0] == '\0') {
    snprintf(g.yearLabel, sizeof(g.yearLabel), "%s", newYear);
    g_dirty = true;
  } else if (strcmp(g.yearLabel, newYear) != 0) {
    // Yearly archives are flat: /stats/yearly/YYYY.json
    fsx::mkdirs("/stats/yearly");
    char file[64];
    snprintf(file, sizeof(file), "/stats/yearly/%s.json", g.yearLabel);
    char json[640];
    g.year.toJson(json, sizeof(json), g.yearLabel);
    fsx::writeText(file, json);
    snprintf(g.yearLabel, sizeof(g.yearLabel), "%s", newYear);
    resetPeriod(g.year);
    g_dirty = true;
  }

  // Daily counter rollover, keyed on the date so a corrected clock or a skipped
  // cycle cannot fire it twice.
  const uint32_t today = daysSinceEpoch();
  if (today != 0 && g.dailyDate != today) {
    if (g.dailyDate != 0) {
      // Yesterday's total competes for "busiest day".
      if (!g.rec[3].set || g.dailyVisitors > g.rec[3].value) {
        const time_t yEnd = static_cast<time_t>(today) * 86400 - 1;
        struct tm y;
        localtime_r(&yEnd, &y);
        g.rec[3].value = static_cast<float>(g.dailyVisitors);
        strftime(g.rec[3].at, sizeof(g.rec[3].at), "%Y-%m-%d", &y);
        g.rec[3].set = 1;
      }
      g.dailyVisitors = 0;
    }
    g.dailyDate = today;
    g_dirty = true;
  }
}

// --- records --------------------------------------------------------------

void updateRecords(float tempC, uint16_t co2) {
  char stamp[32];
  util::timestamp(stamp, sizeof(stamp));
  bool changed = false;

  if (co2 >= 1 && co2 <= 32768 && (!g.rec[0].set || co2 > g.rec[0].value)) {
    g.rec[0].value = static_cast<float>(co2);
    snprintf(g.rec[0].at, sizeof(g.rec[0].at), "%s", stamp);
    g.rec[0].set = 1;
    changed = true;
  }

  const float tempF = tempC * 9.0f / 5.0f + 32.0f;
  if (!isnan(tempF) && tempF >= -40.0f && tempF <= 185.0f) {
    if (!g.rec[1].set || tempF > g.rec[1].value) {
      g.rec[1].value = tempF;
      snprintf(g.rec[1].at, sizeof(g.rec[1].at), "%s", stamp);
      g.rec[1].set = 1;
      changed = true;
    }
    if (!g.rec[2].set || tempF < g.rec[2].value) {
      g.rec[2].value = tempF;
      snprintf(g.rec[2].at, sizeof(g.rec[2].at), "%s", stamp);
      g.rec[2].set = 1;
      changed = true;
    }
  }
  if (changed) g_dirty = true;
}

void noteUptime(float days, const char* when) {
  if (days <= 0) return;
  if (g.rec[4].set && days <= g.rec[4].value) return;
  g.rec[4].value = days;
  snprintf(g.rec[4].at, sizeof(g.rec[4].at), "%s", when);
  g.rec[4].set = 1;
  g_dirty = true;
}

void recordsJson(char* out, size_t cap) {
  static const char* keys[5] = {"highest_co2", "highest_temp_f", "lowest_temp_f",
                                "most_visitors_day", "longest_uptime_d"};
  int n = snprintf(out, cap, "{");
  if (n < 0 || static_cast<size_t>(n) >= cap) return;
  for (int i = 0; i < 5; ++i) {
    n += snprintf(out + n, cap - n, "\"%s\":", keys[i]);
    if (!g.rec[i].set) {
      n += snprintf(out + n, cap - n, "null");
    } else if (i == 0 || i == 3) {
      n += snprintf(out + n, cap - n, "{\"value\":%d,\"at\":\"%s\"}",
                    static_cast<int>(g.rec[i].value), g.rec[i].at);
    } else {
      n += snprintf(out + n, cap - n, "{\"value\":%.2f,\"at\":\"%s\"}",
                    g.rec[i].value, g.rec[i].at);
    }
    if (i < 4) n += snprintf(out + n, cap - n, ",");
  }
  snprintf(out + n, cap - n, "}");
}

// --- dumsor ---------------------------------------------------------------

void dumsorBootCheck() {
  if (!config::v.dumsorTracking) return;
  if (g_dumsorChecked) return;
  struct tm t;
  if (!getLocalTime(&t, 0)) return;  // clock not authoritative yet; retry later
  const uint32_t now = static_cast<uint32_t>(mktime(&t));
  if (now == 0) return;
  g_dumsorChecked = true;

  const uint32_t last = g.lastSeen;
  g_dirty = true;

  if (last == 0 || last >= now) {
    g.lastSeen = now;
    return;
  }

  const uint32_t gap = now - last;
  if (gap >= kOutageMinSecs && gap <= kOutageMaxSecs) {
    // Persist the row BEFORE advancing lastSeen. v1.5 did the opposite, so a
    // crash between the two left lastSeen stale and every later boot appended
    // another copy of the same outage - which is exactly what we had to clean
    // out of the R2 snapshot after the last incident.
    if (fsx::writable()) {
      std::string existing;
      const bool missing = !fsx::readAll(kPowerEventsPath, existing, 1024);
      if (missing) {
        fsx::appendText(kPowerEventsPath,
                        "outage_start_unix,power_restored_unix,down_seconds\r\n");
      }
      char row[64];
      const size_t n = snprintf(row, sizeof(row), "%u,%u,%u\n", last, now, gap);
      fsx::append(kPowerEventsPath, row, n);
      trimPowerEvents();
    }

    ++g.outagesTotal;
    g.outageSecsTotal += gap;
    g.lastOutageSecs = gap;
    g.lastOutageAt = now;

    const time_t st = static_cast<time_t>(last);
    struct tm et, nowTm;
    const time_t nt = static_cast<time_t>(now);
    localtime_r(&st, &et);
    localtime_r(&nt, &nowTm);
    if (et.tm_year == nowTm.tm_year && et.tm_mon == nowTm.tm_mon) {
      ++g.outagesMonth;
      g.outageSecsMonth += gap;
    }
    Serial.printf("[dumsor] power was off ~%us\n", (unsigned)gap);
  }
  g.lastSeen = now;
}

void dumsorHeartbeat() {
  if (!config::v.dumsorTracking) return;
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  const uint32_t now = static_cast<uint32_t>(mktime(&t));
  if (now == 0 || now == g.lastSeen) return;
  g.lastSeen = now;
  g_dirty = true;
}

uint32_t outagesTotal() { return g.outagesTotal; }
uint32_t outagesMonth() { return g.outagesMonth; }
uint64_t outageSecondsTotal() { return g.outageSecsTotal; }
uint64_t outageSecondsMonth() { return g.outageSecsMonth; }
uint32_t lastOutageSecs() { return g.lastOutageSecs; }
int64_t  lastOutageAt() { return static_cast<int64_t>(g.lastOutageAt); }

// --- CSV log --------------------------------------------------------------

void logSample(float tempC, float tempF, float hum, float pressureHpa,
               float altitudeFt, uint16_t co2, uint16_t voc, float heatC,
               int rssi, uint32_t reqs) {
  // Never write an un-timestamped row: pre-sync rows would pollute every
  // chart with a bogus date.
  if (!clockOk()) return;

  char dir[24], file[48];
  struct tm t;
  getLocalTime(&t, 0);
  strftime(dir, sizeof(dir), "/logs/%Y", &t);
  strftime(file, sizeof(file), "/logs/%Y/%Y-%m-%d.csv", &t);

  if (!fsx::exists(file)) {
    fsx::mkdirs(dir);
    static const char kHeader[] =
        "timestamp,cpu_temp_c,cpu_temp_f,memory_percent,temperature_c,temperature_f,"
        "humidity_percent,pressure_hpa,altitude_ft,co2_ppm,voc_ppb,heat_index_c,"
        "heat_index_f,rssi,sd_free_mb,requests_interval,power_w\r\n";
    if (!fsx::appendText(file, kHeader)) return;
  }

  char ts[32];
  util::timestamp(ts, sizeof(ts));

  const float memPct =
      100.0f * (ESP.getHeapSize() - ESP.getFreeHeap()) / ESP.getHeapSize();
  const float cpuTemp = temperatureRead();
  const size_t freeBytes = fsx::totalBytes() - fsx::usedBytes();
  const float freeMb = static_cast<float>(freeBytes) / 1048576.0f;

  // power_w is intentionally left empty: the column stays so the historical
  // charts keep their column indices, but the Shelly integration is gone.
  char row[256];
  const int n = snprintf(row, sizeof(row),
      "%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%u,%.2f,%.2f,%d,%.2f,%u,\r\n",
      ts, cpuTemp, cpuTemp * 9.0f / 5.0f + 32.0f, memPct, tempC, tempF, hum,
      pressureHpa, altitudeFt, static_cast<unsigned>(co2),
      static_cast<unsigned>(voc), heatC, heatC * 9.0f / 5.0f + 32.0f, rssi,
      freeMb, static_cast<unsigned>(reqs));
  if (n > 0) fsx::append(file, row, static_cast<size_t>(n));
}

int pruneLogs(int days) {
  struct tm t;
  if (!getLocalTime(&t, 0)) return 0;
  const time_t cutoff = mktime(&t) - static_cast<time_t>(days) * 86400;
  struct tm ct;
  localtime_r(&cutoff, &ct);
  char cutoffLabel[12];
  strftime(cutoffLabel, sizeof(cutoffLabel), "%Y-%m-%d", &ct);

  // Collect first, delete after. Mutating a directory while walking it is the
  // pattern that contributed to the original metadata corruption.
  static char victims[48][40];
  int victimCount = 0;

  fsx::walk([&](const char* abs, const char* base, size_t) {
    if (victimCount >= 48) return true;
    const bool inLogs = strstr(abs, "/logs/") != nullptr;
    if (inLogs) {
      if (strlen(base) == 14 && strcmp(base + 10, ".csv") == 0 &&
          memcmp(base, cutoffLabel, 10) < 0) {
        snprintf(victims[victimCount++], sizeof(victims[0]), "%s", abs);
      }
      return true;
    }
    // Abandoned atomic-write fragments anywhere in the tree are always junk.
    const size_t n = strlen(base);
    if (n > 4 && (strcmp(base + n - 4, ".tmp") == 0 ||
                  strcmp(base + n - 4, ".bak") == 0)) {
      snprintf(victims[victimCount++], sizeof(victims[0]), "%s", abs);
    }
    return true;
  });

  int removed = 0;
  for (int i = 0; i < victimCount; ++i) {
    if (fsx::remove(victims[i])) ++removed;
  }
  if (removed) Serial.printf("[state] pruned %d old log file(s)\n", removed);
  return removed;
}

// --- interval request counter --------------------------------------------

void countRequest(uint32_t peakThisMinute) {
  ++g_intervalReqs;
  if (peakThisMinute > g_intervalPeak) g_intervalPeak = peakThisMinute;
}
uint32_t requestsThisInterval() { return g_intervalReqs; }
void resetIntervalRequests() {
  g_intervalReqs = 0;
  g_intervalPeak = 0;
}

}  // namespace state