#include "adsb.h"

#include <HTTPClient.h>
#include <WiFiClient.h>

#include "board.h"
#include "relay.h"
#include "util.h"

#include <ctype.h>

namespace adsb {
namespace {

// The receiver defaults to dump1090/tar1090 on the LAN. Configurable so a moved
// receiver does not need a reflash.
constexpr char kDefaultUrl[] = "http://192.168.100.2:8080/data/aircraft.json";
constexpr uint32_t kPollMs = 6000;

// Cap what we hold and forward. The receiver reports every aircraft it hears,
// including ones with no position; the Worker filters, but there is no point
// shipping more than this.
constexpr int kMaxAircraft = 40;

// One payload slot, reused. Doubles as the /adsb.json answer.
// The prefix at the head is reserved for the event wrapper so the Worker push
// can be assembled in place, which avoids a second full-size buffer - DRAM on
// this part is tight enough that the extra copy overflowed dram0_0_seg.
constexpr size_t kPayload = 1400;

// Heap, allocated once in begin(). dram0_0_seg is effectively full on this
// build - even 1.4KB of static here overflows it - and the heap has ~133KB.
char* g_json = nullptr;
size_t g_jsonLen = 0;
bool g_live = false;
uint32_t g_lastPollMs = 0;
bool g_wifiClientStarted = false;

// Bounded reader for the receiver response. The full document runs ~1.4KB with a
// handful of aircraft but can be far larger when traffic is heavy, so the buffer
// is capped and the parse is skipped when it does not fit.
// The receiver emits ~1.4KB for a typical sky. Cap at 3.5KB: enough for a busy
// sky. Allocated on the heap, because dram0_0_seg is nearly exhausted and a
// static here overflowed it - the heap has ~133KB free.
constexpr size_t kMaxDoc = 3584;

WiFiClient& client() {
  static WiFiClient c;
  return c;
}

const char* receiverUrl() { return kDefaultUrl; }

// --- minimal extraction ---------------------------------------------------
//
// No JSON library: a full DOM for one object per aircraft would cost more than
// the rest of the app. This scans for the keys the Worker and the map actually
// read and copies out just those.

// Finds `"key":` within [begin, end). The trailing colon is part of the match
// so a key occurring inside a string value - a callsign reading `"lat"` - can
// never be mistaken for the real key.
const char* findKey(const char* begin, const char* end, const char* key, size_t keyLen) {
  const char* p = begin;
  while (p < end) {
    const char* q = static_cast<const char*>(memmem(p, static_cast<size_t>(end - p), key, keyLen));
    if (!q) return nullptr;
    const char* after = q + keyLen;
    while (after < end && isspace(static_cast<unsigned char>(*after))) ++after;
    if (after < end && *after == ':') return q;
    p = q + 1;
  }
  return nullptr;
}

// Copies a JSON string value into dst, unescaping the few escapes dump1090 emits.
// Returns false when the key is absent or the value is not a string.
bool readString(const char* obj, const char* objEnd, const char* key, char* dst, size_t dstCap) {
  const size_t kl = strlen(key);
  const char* at = findKey(obj, objEnd, key, kl);
  if (!at) return false;
  const char* colon = strchr(at, ':');
  if (!colon || colon >= objEnd) return false;
  const char* p = colon + 1;
  while (p < objEnd && isspace(static_cast<unsigned char>(*p))) ++p;
  if (p >= objEnd || *p != '"') return false;
  ++p;
  size_t o = 0;
  while (p < objEnd && *p != '"') {
    char c = *p++;
    if (c == '\\' && p < objEnd) {
      const char* e = p++;
      switch (*e) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        default:  c = *e; break;
      }
    }
    if (o + 1 < dstCap) dst[o++] = c;
  }
  dst[o] = '\0';
  return o > 0;
}

// Copies a numeric value. Returns false when absent, null, or not a number -
// dump1090 uses null for unknown altitude and ground is a string.
bool readNumber(const char* obj, const char* objEnd, const char* key, double* out) {
  const size_t kl = strlen(key);
  const char* at = findKey(obj, objEnd, key, kl);
  if (!at) return false;
  const char* colon = strchr(at, ':');
  if (!colon || colon >= objEnd) return false;
  const char* p = colon + 1;
  while (p < objEnd && isspace(static_cast<unsigned char>(*p))) ++p;
  if (p >= objEnd || *p == 'n') return false;   // null
  if (*p == '"') return false;                  // e.g. "ground"
  char* endp = nullptr;
  const double v = strtod(p, &endp);
  if (endp == p) return false;
  *out = v;
  return true;
}

// Emits "key":value for a number, or nothing when absent.
// Appends one character, staying inside the buffer.
// Appends one character. Returns cap without writing when there is no room,
// so the terminator always stays inside the buffer.
size_t appendChar(char* out, size_t cap, size_t o, char c) {
  if (o + 1 >= cap) return o;
  out[o] = c;
  out[o + 1] = '\0';
  return o + 1;
}

// Appends with a bounded advance. snprintf reports the length it WOULD have
// written, so a plain `o += snprintf(...)` overshoots once a value approaches
// the buffer limit and every later write lands outside the array.
size_t put(char* out, size_t cap, size_t o, const char* fmt, ...) {
  if (o >= cap) return cap;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(out + o, cap - o, fmt, ap);
  va_end(ap);
  if (n < 0) return o;
  const size_t next = o + static_cast<size_t>(n);
  return next > cap ? cap : next;
}

void putNum(char* out, size_t cap, size_t* o, const char* key, double v) {
  if (v == static_cast<double>(static_cast<long>(v))) {
    *o = put(out, cap, *o, "\"%s\":%ld", key, static_cast<long>(v));
  } else {
    *o = put(out, cap, *o, "\"%s\":%.4f", key, v);
  }
}

void putStr(char* out, size_t cap, size_t* o, const char* key, const char* v) {
  *o = put(out, cap, *o, "\"%s\":\"%s\"", key, v);
}

}  // namespace

void begin() {
  if (!g_json) g_json = static_cast<char*>(malloc(kPayload + 1));
  if (!g_json) return;
  snprintf(g_json, kPayload, "{\"now\":0,\"aircraft\":[]}");
  g_jsonLen = strlen(g_json);
  g_live = false;
  g_lastPollMs = 0;
}

bool live() { return g_live; }

const char* lastJson() { return g_json ? g_json : ""; }

void tick() {
  if (!g_json) return;
  const unsigned long nowMs = millis();
  // Poll on first call, then on the interval. g_lastPollMs starts at 0 and
  // millis() is small at boot, so allow one immediate attempt.
  if (g_lastPollMs && (nowMs - g_lastPollMs) < kPollMs) return;
  if (WiFi.status() != WL_CONNECTED) return;
  g_lastPollMs = nowMs;

  HTTPClient http;
  g_wifiClientStarted = http.begin(client(), receiverUrl());
  if (!g_wifiClientStarted) return;

  http.setTimeout(4000);
  http.setConnectTimeout(2000);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    // Do not clear a good payload on a transient failure; the Worker keeps the
    // last fleet until it goes stale on its own.
    return;
  }

  char* doc = static_cast<char*>(malloc(kMaxDoc));
  if (!doc) { http.end(); return; }
  const int n = http.getSize();
  if (n < 0 || static_cast<size_t>(n) > kMaxDoc - 1) {
    http.end();
    free(doc);
    return;
  }
  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  while (got < static_cast<size_t>(n) && stream->available()) {
    const int r = stream->readBytes(doc + got, n - got);
    if (r <= 0) break;
    got += static_cast<size_t>(r);
  }
  http.end();
  if (got < 16) { free(doc); return; }
  doc[got] = '\0';

  // Walk the aircraft array. Each element is scanned independently, so one
  // malformed record cannot corrupt the rest.
  const char* p = strstr(doc, "\"aircraft\"");
  if (!p) { free(doc); return; }
  const char* bracket = strchr(p, '[');
  if (!bracket) { free(doc); return; }
  ++bracket;

  // Build the fleet into g_json from offset 0. The event wrapper for the
  // Worker push is assembled in its own scratch buffer so nothing has to be
  // shifted afterwards - an earlier in-place version slid the payload up inside
  // this buffer, left the string unterminated, and let later writes bleed past
  // it, which showed up as a stray brace and fragments of another file.
  char* const out = g_json;
  const size_t cap = kPayload;
  size_t o = 0;
  o = put(out, cap, 0, "{\"now\":%ld,\"aircraft\":[",
          static_cast<long>(millis() / 1000));
  int count = 0;

  const char* cur = bracket;
  while (*cur && *cur != ']' && count < kMaxAircraft) {
    while (isspace(static_cast<unsigned char>(*cur)) || *cur == ',') ++cur;
    if (*cur != '{') break;
    // Find this object's end, respecting strings so a brace inside a callsign
    // cannot end it early.
    const char* objEnd = cur + 1;
    bool inStr = false;
    for (; *objEnd; ++objEnd) {
      if (*objEnd == '"' && objEnd[-1] != '\\') inStr = !inStr;
      else if (*objEnd == '}' && !inStr) break;
    }
    if (*objEnd != '}') break;

    char hex[16] = "";
    double lat, lon, seen, gs, trk, rssi;
    char flight[16] = "";
    char cat[8] = "";
    char sq[8] = "";

    const bool haveHex = readString(cur, objEnd + 1, "\"hex\"", hex, sizeof(hex));
    const bool haveLat = readNumber(cur, objEnd + 1, "\"lat\"", &lat);
    const bool haveLon = readNumber(cur, objEnd + 1, "\"lon\"", &lon);

    if (haveHex && strlen(hex) >= 6 && haveLat && haveLon) {
      // Reserve room for the rest of the record before committing to it.
      if (o + 240 >= cap) break;

      for (int i = 0; i < 6; ++i) hex[i] = static_cast<char>(toupper(hex[i]));
      hex[6] = '\0';

      if (count > 0) o = appendChar(out, cap, o, ',');
      o = appendChar(out, cap, o, '{');
      ++count;

      // Separator between fields is explicit so the first one is not preceded
      // by a stray comma.
      bool first = true;
      auto sep = [&]() { if (!first) o = appendChar(out, cap, o, ','); first = false; };

      sep(); putStr(out, cap, &o, "hex", hex);
      sep(); putNum(out, cap, &o, "lat", lat);
      sep(); putNum(out, cap, &o, "lon", lon);
      if (readNumber(cur, objEnd + 1, "\"seen\"", &seen)) {
        sep(); putNum(out, cap, &o, "seen", seen);
      }
      double alt2;
      if (readNumber(cur, objEnd + 1, "\"alt_baro\"", &alt2) ||
          readNumber(cur, objEnd + 1, "\"alt_geom\"", &alt2)) {
        sep(); putNum(out, cap, &o, "alt", alt2);
      }
      if (readNumber(cur, objEnd + 1, "\"gs\"", &gs)) {
        sep(); putNum(out, cap, &o, "gs", gs);
      }
      if (readNumber(cur, objEnd + 1, "\"track\"", &trk)) {
        sep(); putNum(out, cap, &o, "trk", trk);
      }
      if (readString(cur, objEnd + 1, "\"flight\"", flight, sizeof(flight))) {
        sep(); putStr(out, cap, &o, "f", flight);
      }
      if (readString(cur, objEnd + 1, "\"category\"", cat, sizeof(cat))) {
        sep(); putStr(out, cap, &o, "cat", cat);
      }
      if (readString(cur, objEnd + 1, "\"squawk\"", sq, sizeof(sq))) {
        sep(); putStr(out, cap, &o, "sq", sq);
      }
      if (readNumber(cur, objEnd + 1, "\"rssi\"", &rssi)) {
        sep(); putNum(out, cap, &o, "rssi", rssi);
      }
      o = appendChar(out, cap, o, '}');
    }

    cur = objEnd + 1;
  }

  o = put(out, cap, o, "],\"count\":%d}", count);
  if (o >= cap) { free(doc); return; }
  out[o] = '\0';                 // terminate exactly at the final offset
  g_jsonLen = o;
  g_live = count > 0;

  // Push to the Worker, which does the real processing and broadcasts to
  // viewers. If the socket is down the payload is still cached for /adsb.json.
  {
    static const char kPre[] =
        "{\"type\":\"event\",\"event\":\"adsb_update\",\"data\":";
    const size_t pre = sizeof(kPre) - 1;
    char* scratch = static_cast<char*>(malloc(pre + g_jsonLen + 2));
    if (scratch && relay::connected()) {
      memcpy(scratch, kPre, pre);
      memcpy(scratch + pre, g_json, g_jsonLen);
      scratch[pre + g_jsonLen] = '}';
      relay::pushRaw(scratch, pre + g_jsonLen + 1);
    }
    free(scratch);
  }

  free(doc);
  // Sample the summary so the log shows whether the feed is live without
  // flooding it every poll.
  static uint32_t lastLog = 0;
  if (millis() - lastLog > 30000) {
    lastLog = millis();
    Serial.printf("[adsb] fleet %d aircraft, %u bytes\n", count, g_jsonLen);
  }
}

}  // namespace adsb