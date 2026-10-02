#include "guestbook.h"

#include "state.h"
#include "storage.h"
#include "util.h"

namespace guestbook {
namespace {

constexpr char kPath[] = "/guestbook.csv";
constexpr char kSchemaPath[] = "/guestbook.schema";
constexpr uint8_t kSchemaVersion = 3;
constexpr size_t kMaxRowLen = 400;
constexpr size_t kMaxPending = 1000;
constexpr uint32_t kRateWindowMs = 3600000UL;
constexpr uint8_t kMaxRateEntries = 50;

uint16_t g_approved = 0;
uint16_t g_pending = 0;
uint16_t g_all = 0;

// Per-IP submission throttle.
struct RateEntry {
  uint32_t ip;
  uint32_t lastMs;
};
RateEntry g_rate[kMaxRateEntries];
uint8_t g_rateCount = 0;

uint32_t ipToKey(const char* ip) {
  return (static_cast<uint32_t>(strtoul(ip, nullptr, 10)) |
          (static_cast<uint32_t>(strtoul(ip, nullptr, 10)) >> 8)) &
         0x00FFFFFF;
}

// Finds the sixth and seventh commas, i.e. the boundaries of id/reply_to/status.
// Returns false for any row that is not schema v3, which is the whole point:
// a malformed row must never be silently reshaped into a different message.
bool splitRow(const char* line, int c[7]) {
  c[0] = 0;
  for (int i = 1; i <= 6; ++i) {
    const char* p = strchr(line + c[i - 1], ',');
    if (!p) return false;
    c[i] = static_cast<int>(p - line);
  }
  return true;
}

void copyField(const char* line, int start, int end, char* out, size_t cap) {
  size_t len = static_cast<size_t>(end - start);
  if (len >= cap) len = cap - 1;
  memcpy(out, line + start, len);
  out[len] = '\0';
  // Rows are written with CRLF but the moderation rewrite uses LF, so strip
  // whichever terminator arrived.
  for (size_t i = len; i > 0; --i) {
    if (out[i - 1] == '\r' || out[i - 1] == ' ' || out[i - 1] == '\t') {
      out[i - 1] = '\0';
    } else {
      break;
    }
  }
}

// Reproduces the v1.5 sanitiser exactly. Dropping commas and backslashes is not
// cosmetic: it is what guarantees the 6-comma invariant readers rely on.
String sanitize(const char* in, size_t maxLen) {
  String out;
  out.reserve(maxLen);
  const unsigned char* p = reinterpret_cast<const unsigned char*>(in);
  for (size_t i = 0; i < maxLen && p[i]; ++i) {
    const unsigned char c = p[i];
    if (c < 0x20 || c == 0x7F) continue;          // control bytes
    if (c == '\\') continue;                       // escape char, never stored
    if (c == ',') { out += ' '; continue; }        // would break field counting
    if (c == '"') { out += '\''; continue; }       // neutralise quoting
    // Zero-width / bidi / BOM sequences: invisible in a browser but able to
    // spoof a name, so they are stripped rather than stored.
    if (c == 0xE2 && p[i + 1] >= 0x80 && p[i + 1] <= 0x84) {
      const unsigned char n = p[i + 1] & 0x1F;
      const unsigned char t = p[i + 2];
      if (t >= 0x80 && t <= 0x9F && (n == 0x00 || n == 0x02 || n == 0x03)) {
        i += 2;
        continue;
      }
    }
    if (c == 0xEF && p[i + 1] == 0xBB && p[i + 2] == 0xBF) {
      i += 2;
      continue;
    }
    out += static_cast<char>(c);
  }
  out.trim();
  return out;
}

void generateId(char* out) {
  // Crockford base32 minus i, l, o, u: unambiguous when read aloud or retyped.
  static const char kAlphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
  uint32_t seed = ESP.getEfuseMac() ^ (millis() * 2654435761UL) ^ (uint32_t)random();
  for (int i = 0; i < 8; ++i) {
    out[i] = kAlphabet[seed & 31];
    seed >>= 5;
    if (seed == 0) seed = ESP.getEfuseMac() ^ (uint32_t)random();
  }
  out[8] = '\0';
}

void writeSchemaStamp() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u\r\n", (unsigned)kSchemaVersion);
  fsx::writeText(kSchemaPath, buf);
}

// Scans the file once to recompute the counters and index the reply graph.
void recompute() {
  g_approved = 0;
  g_pending = 0;
  g_all = 0;

  std::string raw;
  if (!fsx::readAll(kPath, raw, 200 * 1024)) return;

  // Two passes: count first, then decide whether we exceed the queue cap.
  const char* p = raw.c_str();
  while (*p) {
    const char* eol = strchr(p, '\n');
    const size_t len = eol ? static_cast<size_t>(eol - p) : strlen(p);
    char line[kMaxRowLen];
    if (len && len < sizeof(line)) {
      memcpy(line, p, len);
      line[len] = '\0';
      int c[7];
      if (splitRow(line, c)) {
        const char status = line[c[6]];
        ++g_all;
        if (status == '1') ++g_approved;
        else if (status == '0') ++g_pending;
      }
    }
    if (!eol) break;
    p = eol + 1;
  }

  state::adjustGuestbook(static_cast<int32_t>(g_pending), static_cast<int32_t>(g_approved),
                         static_cast<int32_t>(g_all));
}

}  // namespace

void begin() {
  // v1.5 aborted the boot when the schema marker was missing or too old. We
  // keep the guard for rows that are not v3 (there is no safe way to interpret
  // them) but infer v3 from a well-formed first row instead of demanding a
  // marker file that may never have been uploaded.
  char ver[8] = "";
  fsx::readTrimmed(kSchemaPath, ver, sizeof(ver));
  const int version = atoi(ver);

  std::string raw;
  const bool haveCsv = fsx::readAll(kPath, raw, 200 * 1024);

  if (version == kSchemaVersion) {
    recompute();
    return;
  }

  if (!haveCsv) {
    writeSchemaStamp();
    recompute();
    return;
  }

  // No usable marker: probe the first data row.
  const char* first = raw.c_str();
  if (*first == '\0' || *first == '\r' || *first == '\n') {
    writeSchemaStamp();
    recompute();
    return;
  }
  int c[7];
  char line[kMaxRowLen];
  const char* eol = strchr(first, '\n');
  const size_t len = eol ? static_cast<size_t>(eol - first) : strlen(first);
  if (len && len < sizeof(line)) {
    memcpy(line, first, len);
    line[len] = '\0';
    if (splitRow(line, c)) {
      writeSchemaStamp();
      recompute();
      return;
    }
  }
  Serial.println("[guestbook] unrecognised schema; queue disabled");
}

uint16_t approvedCount() { return g_approved; }
uint16_t allCount() { return g_all; }
uint16_t pendingCount() { return g_pending; }
uint16_t pendingLimitReached() { return g_pending >= kMaxPending ? 1 : 0; }

bool rateLimited(const char* ip) {
  if (!ip) return false;
  const uint32_t key = ipToKey(ip);
  const uint32_t now = millis();
  for (uint8_t i = 0; i < g_rateCount; ++i) {
    if (g_rate[i].ip == key) {
      if (now - g_rate[i].lastMs < kRateWindowMs) return true;
      g_rate[i].lastMs = now;
      return false;
    }
  }
  return false;
}

void noteSubmitAttempt(const char* ip) {
  if (!ip) return;
  const uint32_t key = ipToKey(ip);
  for (uint8_t i = 0; i < g_rateCount; ++i) {
    if (g_rate[i].ip == key) {
      g_rate[i].lastMs = millis();
      return;
    }
  }
  if (g_rateCount < kMaxRateEntries) {
    g_rate[g_rateCount].ip = key;
    g_rate[g_rateCount].lastMs = millis();
    ++g_rateCount;
  }
}

bool validReplyParent(const char* replyToId) {
  if (!replyToId || replyToId[0] == '\0') return true;
  const size_t idLen = strlen(replyToId);
  if (idLen != 8) return false;

  std::string raw;
  if (!fsx::readAll(kPath, raw, 200 * 1024)) return false;

  const char* p = raw.c_str();
  while (*p) {
    const char* eol = strchr(p, '\n');
    const size_t len = eol ? static_cast<size_t>(eol - p) : strlen(p);
    char line[kMaxRowLen];
    if (len && len < sizeof(line)) {
      memcpy(line, p, len);
      line[len] = '\0';
      int c[7];
      if (splitRow(line, c)) {
        char id[10] = "", reply[10] = "", status[2] = "";
        copyField(line, c[4], c[5], id, sizeof(id));
        copyField(line, c[5], c[6], reply, sizeof(reply));
        copyField(line, c[6], c[6] + 1, status, sizeof(status));
        if (strcmp(id, replyToId) == 0) {
          if (status[0] != '1') return false;         // must be public
          return strlen(reply) == 0;                   // parent must be top-level
        }
      }
    }
    if (!eol) break;
    p = eol + 1;
  }
  return false;
}

bool submit(const char* name, const char* message, const char* replyToId,
            const char* country, const char** err) {
  if (!name || !message) {
    if (err) *err = "Name and message required";
    return false;
  }
  if (strlen(name) < 1 || strlen(name) > 32 || strlen(message) < 1 ||
      strlen(message) > 200) {
    if (err) *err = "Name (1-32 chars) and message (1-200 chars) required";
    return false;
  }
  if (!fsx::writable()) {
    if (err) *err = "Storage is read-only; the guestbook is temporarily closed";
    return false;
  }
  if (pendingLimitReached()) {
    if (err) *err = "Moderation queue is full, please try again later";
    return false;
  }
  if (replyToId && replyToId[0] && !validReplyParent(replyToId)) {
    if (err) *err = "Invalid reply target";
    return false;
  }

  const String cleanName = sanitize(name, 32);
  const String cleanMsg = sanitize(message, 200);
  if (cleanName.length() == 0 || cleanMsg.length() == 0) {
    if (err) *err = "Name and message required";
    return false;
  }

  char ts[32];
  if (!util::timestamp(ts, sizeof(ts)) || strcmp(ts, "unknown") == 0) {
    if (err) *err = "Clock not synced yet, try again in a moment";
    return false;
  }

  char cc[4];
  util::normalizeCountry(country, cc);

  char id[10];
  generateId(id);

  char reply[10] = "";
  if (replyToId && strlen(replyToId) == 8) {
    for (int i = 0; i < 8; ++i) {
      reply[i] = static_cast<char>((replyToId[i] >= 'A' && replyToId[i] <= 'Z')
                                       ? replyToId[i] - 'A' + 'a'
                                       : replyToId[i]);
    }
    reply[8] = '\0';
  }

  // The sanitiser guarantees neither field contains a comma, so the row has
  // exactly 6 of them.
  char row[320];
  const int n = snprintf(row, sizeof(row), "%s,%s,%s,%s,%s,%s,0\r\n", ts, cc,
                         cleanName.c_str(), cleanMsg.c_str(), id, reply);
  if (n <= 0 || !fsx::appendText(kPath, row)) {
    if (err) *err = "Failed to save";
    return false;
  }

  ++g_all;
  ++g_pending;
  state::adjustGuestbook(1, 0, 1);
  return true;
}

int moderateBatch(const char* ops, uint16_t* appliedOut) {
  uint16_t applied = 0;
  if (!ops || !fsx::writable()) {
    if (appliedOut) *appliedOut = 0;
    return 0;
  }

  std::string raw;
  if (!fsx::readAll(kPath, raw, 200 * 1024)) {
    if (appliedOut) *appliedOut = 0;
    return 0;
  }

  // Collect idx -> new status. idx is the 0-based data-row index, matching what
  // the admin UI shows, and excludes the header.
  char wanted[100][8];
  char value[100];
  int opCount = 0;

  const char* o = ops;
  while (*o && opCount < 100) {
    const char* comma = strchr(o, ',');
    const size_t segLen = comma ? static_cast<size_t>(comma - o) : strlen(o);
    if (segLen >= 3 && segLen < 8) {
      char seg[10];
      memcpy(seg, o, segLen);
      seg[segLen] = '\0';
      char* colon = strchr(seg, ':');
      if (colon) {
        *colon = '\0';
        const char st = colon[1];
        if (st >= '0' && st <= '3') {
          snprintf(wanted[opCount], 8, "%s", seg);
          value[opCount] = st;
          ++opCount;
        }
      }
    }
    if (!comma) break;
    o = comma + 1;
  }
  if (opCount == 0) {
    if (appliedOut) *appliedOut = 0;
    return 0;
  }

  // Single rewrite of the whole file, atomic via the storage layer.
  std::string out;
  out.reserve(raw.size() + 64);

  int rowIndex = -1;
  int32_t dPending = 0, dApproved = 0, dAll = 0;
  const char* p = raw.c_str();
  while (*p) {
    const char* eol = strchr(p, '\n');
    const size_t len = eol ? static_cast<size_t>(eol - p) : strlen(p);
    char line[kMaxRowLen];

    if (len && len < sizeof(line)) {
      memcpy(line, p, len);
      line[len] = '\0';
      int c[7];
      if (splitRow(line, c)) {
        char status[2] = "";
        copyField(line, c[6], c[6] + 1, status, sizeof(status));
        if (rowIndex >= 0) {
          const char oldStatus = status[0];
          char newStatus = oldStatus;
          for (int i = 0; i < opCount; ++i) {
            if (atoi(wanted[i]) == rowIndex) {
              newStatus = value[i];
              break;
            }
          }
          if (newStatus != oldStatus) {
            if (oldStatus == '0') --dPending;
            if (oldStatus == '1') --dApproved;
            if (newStatus == '0') ++dPending;
            if (newStatus == '1') ++dApproved;
            if (newStatus == '3') --dAll;
            ++applied;
            line[c[6]] = newStatus;
          }
          out.append(line);
          out.append("\r\n");
        }
        ++rowIndex;
      }
    }
    if (!eol) break;
    p = eol + 1;
  }

  if (applied && !fsx::writeText(kPath, out.c_str())) {
    if (appliedOut) *appliedOut = 0;
    return 0;
  }
  state::adjustGuestbook(dPending, dApproved, dAll);
  if (appliedOut) *appliedOut = applied;
  return applied;
}

}  // namespace guestbook