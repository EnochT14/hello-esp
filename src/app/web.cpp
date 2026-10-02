#include "web.h"

#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

#include <esp_system.h>

#include "board.h"
#include "backup.h"
#include "config.h"
#include "guestbook.h"
#include "relay.h"
#include "sensors.h"
#include "state.h"
#include "storage.h"
#include "util.h"

namespace web {
namespace {

AsyncWebServer g_server(80);

constexpr size_t kAuthFailMax = 8;
constexpr uint32_t kAuthLockoutMs = 600000UL;  // 10 min
constexpr uint32_t kGuestbookRateWindow = 3600000UL;
constexpr size_t kPageSize = 20;

struct AuthFail {
  uint32_t ip;
  uint8_t count;
  uint32_t lastMs;
};
AuthFail g_authFail[kAuthFailMax];
uint8_t g_authFailCount = 0;

uint32_t ipOf(AsyncWebServerRequest* r) {
  return r->client()->remoteIP();
}

bool constantTimeEquals(const char* a, const char* b) {
  size_t la = strlen(a), lb = strlen(b);
  size_t n = la > lb ? la : lb;
  unsigned char diff = static_cast<unsigned char>(la ^ lb);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char ca = i < la ? a[i] : 0;
    const unsigned char cb = i < lb ? b[i] : 0;
    diff |= static_cast<unsigned char>(ca ^ cb);
  }
  return diff == 0;
}

// Minimal Base64 decoder for the Basic-auth header. Hand-rolled so the build
// does not depend on which Base64 overloads this core happens to ship.
String b64Decode(const String& in) {
  static int8_t table[256];
  static bool ready = false;
  if (!ready) {
    for (int i = 0; i < 256; ++i) table[i] = -1;
    const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) {
      table[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
    }
    ready = true;
  }
  String out;
  uint32_t acc = 0;
  int bits = 0;
  for (unsigned i = 0; i < in.length(); ++i) {
    const unsigned char c = in[i];
    if (c == '=' || c == '\r' || c == '\n') continue;
    const int8_t v = table[c];
    if (v < 0) continue;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return out;
}

bool adminAuth(AsyncWebServerRequest* r) {
  const uint32_t ip = ipOf(r);
  const uint32_t now = millis();
  for (uint8_t i = 0; i < g_authFailCount; ++i) {
    if (g_authFail[i].ip != ip) continue;
    if (now - g_authFail[i].lastMs > kAuthLockoutMs) {
      g_authFail[i].count = 0;
      continue;
    }
    if (g_authFail[i].count >= 5) return false;
  }

  if (!r->hasHeader("Authorization")) return false;
  const String h = r->header("Authorization");
  if (!h.startsWith("Basic ")) return false;
  const String raw = b64Decode(h.substring(6));
  const int colon = raw.indexOf(':');
  if (colon < 0) return false;

  const String user = raw.substring(0, colon);
  const String pass = raw.substring(colon + 1);
  const bool ok = constantTimeEquals(user.c_str(), config::v.adminUser) &&
                  constantTimeEquals(pass.c_str(), config::v.adminPass);
  if (ok) return true;

  for (uint8_t i = 0; i < g_authFailCount; ++i) {
    if (g_authFail[i].ip == ip) {
      g_authFail[i].count++;
      g_authFail[i].lastMs = now;
      return false;
    }
  }
  if (g_authFailCount < kAuthFailMax) {
    g_authFail[g_authFailCount].ip = ip;
    g_authFail[g_authFailCount].count = 1;
    g_authFail[g_authFailCount].lastMs = now;
    ++g_authFailCount;
  }
  return false;
}

// --- response helpers -----------------------------------------------------
void sendText(AsyncWebServerRequest* r, int code, const char* body) {
  r->send(code, "text/plain", body);
}

// Static assets ship pre-gzipped; serving the .gz directly keeps the LWIP pbuf
// pool clear, which is what stops relayed pushes from failing under load.
void sendAsset(AsyncWebServerRequest* r, const char* base, const char* cacheControl) {
  char gz[160];
  snprintf(gz, sizeof(gz), "%s.gz", base);
  const bool useGz = r->header("Accept-Encoding").indexOf("gzip") >= 0 && fsx::exists(gz);
  const char* path = useGz ? gz : base;

  File f = fsx::vol().open(path, FILE_READ);
  if (!f) {
    sendText(r, 404, "not found");
    return;
  }
  AsyncWebServerResponse* resp =
      r->beginResponse(static_cast<Stream&>(f), "text/html", f.size());
  if (useGz) resp->addHeader("Content-Encoding", "gzip");
  if (cacheControl) resp->addHeader("Cache-Control", cacheControl);
  r->send(resp);
}

void sendJson(AsyncWebServerRequest* r, const char* body, const char* cacheControl) {
  AsyncWebServerResponse* resp =
      r->beginResponse(200, "application/json",
                       reinterpret_cast<const uint8_t*>(body), strlen(body));
  if (cacheControl) resp->addHeader("Cache-Control", cacheControl);
  r->send(resp);
}

// --- read-only storage guard ----------------------------------------------
// Admin mutations must fail loudly when the filesystem is latched read-only,
// rather than appearing to succeed.
bool guardWritable(AsyncWebServerRequest* r) {
  if (fsx::writable()) return true;
  r->send(503, "text/plain",
          "Storage is read-only: filesystem metadata needs repairing. "
          "Run `pio run -t uploadfs` on the device.");
  return false;
}

// --- visitor counting -----------------------------------------------------
void countVisitIfPublic(AsyncWebServerRequest* r) {
  const bool viaWorker = r->hasHeader("CF-Connecting-IP");
  const bool fromLan = util::isPrivateIp(ipOf(r));
  if (!viaWorker && fromLan) return;  // owner testing, not a visitor

  char cc[4] = "??";
  if (viaWorker) util::normalizeCountry(r->header("CF-IPCountry").c_str(), cc);
  state::countVisit(cc);
}

// --- guestbook row iteration ---------------------------------------------
struct Row {
  char time[32], country[4], name[34], message[208], id[10], replyTo[10];
  char status;
};

bool parseRow(const char* line, Row* out) {
  int c[7];
  c[0] = 0;
  for (int i = 1; i <= 6; ++i) {
    const char* p = strchr(line + c[i - 1], ',');
    if (!p) return false;
    c[i] = static_cast<int>(p - line);
  }
  auto grab = [&](int a, int b, char* dst, size_t cap) {
    size_t n = static_cast<size_t>(b - a);
    if (n >= cap) n = cap - 1;
    memcpy(dst, line + a, n);
    dst[n] = '\0';
    for (size_t i = n; i > 0; --i) {
      const char ch = dst[i - 1];
      if (ch == '\r' || ch == ' ' || ch == '\t') dst[i - 1] = '\0';
      else break;
    }
  };
  grab(c[0], c[1], out->time, sizeof(out->time));
  grab(c[1], c[2], out->country, sizeof(out->country));
  grab(c[2], c[3], out->name, sizeof(out->name));
  grab(c[3], c[4], out->message, sizeof(out->message));
  grab(c[4], c[5], out->id, sizeof(out->id));
  grab(c[5], c[6], out->replyTo, sizeof(out->replyTo));
  char st[2] = {};
  grab(c[6], c[6] + 1, st, sizeof(st));
  out->status = st[0];
  return true;
}

// Walks guestbook.csv newest-first. `cb` returns false to stop.
template <typename F>
size_t walkEntries(bool approvedOnly, F cb) {
  std::string raw;
  if (!fsx::readAll("/guestbook.csv", raw, 200 * 1024)) return 0;
  // Index the line starts so we can iterate backwards without copying.
  static size_t starts[1024];
  size_t count = 0;
  size_t pos = 0;
  while (pos < raw.size() && count < 1024) {
    starts[count++] = pos;
    const size_t nl = raw.find('\n', pos);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  for (size_t i = count; i-- > 0;) {
    size_t end = raw.find('\n', starts[i]);
    if (end == std::string::npos) end = raw.size();
    if (end <= starts[i]) continue;
    char line[400];
    const size_t len = end - starts[i];
    if (len >= sizeof(line)) continue;
    memcpy(line, raw.data() + starts[i], len);
    line[len] = '\0';
    Row row;
    if (!parseRow(line, &row)) continue;
    if (approvedOnly && row.status != '1') continue;
    if (!cb(&row)) return i + 1;
  }
  return count;
}

void emitEntry(const Row& r, char* out, size_t cap, size_t* o, bool withReplyTo) {
  char e[32] = "", m[256] = "", n[48] = "", t[40] = "";
  util::jsonEscape(r.message, m, sizeof(m));
  util::jsonEscape(r.name, n, sizeof(n));
  util::jsonEscape(r.time, t, sizeof(t));
  *o += snprintf(out + *o, cap - *o,
                 "{\"time\":\"%s\",\"country\":\"%s\",\"name\":\"%s\",\"message\":\"%s\",\"id\":\"%s\"",
                 t, r.country, n, m, r.id);
  if (withReplyTo) *o += snprintf(out + *o, cap - *o, ",\"reply_to\":\"%s\"", r.replyTo);
}

// --- /stats ---------------------------------------------------------------
size_t buildStatsJsonRaw(char* out, size_t cap) {
  sensors::Reading s{};
  sensors::read(&s);

  const uint32_t usedBytes = fsx::usedBytes();
  const uint32_t totalBytes = fsx::totalBytes();
  const float freeMb = static_cast<float>(totalBytes - usedBytes) / 1048576.0f;
  const float usedMb = static_cast<float>(usedBytes) / 1048576.0f;
  const float memUsed = ESP.getHeapSize() - ESP.getFreeHeap();

  char ts[32];
  util::timestamp(ts, sizeof(ts));

  const unsigned long up = millis() / 1000UL;
  const float cpuTemp = temperatureRead();

  char today[12] = "";
  int hourNow = -1;
  struct tm t = {};
  if (getLocalTime(&t, 0)) {
    strftime(today, sizeof(today), "%Y-%m-%d", &t);
    hourNow = t.tm_hour;
  }

  int n = snprintf(out, cap,
      "{\"response_ms\":0,\"timestamp\":\"%s\","
      "\"uptime\":\"%lu days, %lu hours, %lu minutes, %lu seconds\","
      "\"rssi\":%d,"
      "\"cpu_temp\":{\"celsius\":%.2f,\"fahrenheit\":%.2f},"
      "\"memory\":{\"used_bytes\":%lu,\"used_kb\":%.2f,\"used_percent\":%.2f},"
      "\"sd_used_mb\":%.2f,\"sd_free_mb\":%.2f,"
      "\"visitors\":%lu,\"daily_visitors\":%lu,"
      "\"temperature\":{\"celsius\":%.2f,\"fahrenheit\":%.2f},"
      "\"heat_index\":{\"celsius\":%.2f,\"fahrenheit\":%.2f},"
      "\"altitude_ft\":%.2f,\"humidity_percent\":%.2f,\"pressure_hpa\":%.2f,"
      "\"co2_ppm\":%u,\"voc_ppb\":%u,\"countries\":%lu,"
      "\"guestbook_approved\":%lu,"
      "\"sensors\":{\"bme_ok\":%s,\"ccs_ok\":%s,\"oled_ok\":%s},"
      "\"today_local\":\"%s\",\"today_local_hour\":%d",
      ts,
      up / 86400UL, (up / 3600UL) % 24, (up / 60UL) % 60, up % 60,
      WiFi.RSSI(),
      cpuTemp, cpuTemp * 9.0f / 5.0f + 32.0f,
      static_cast<unsigned long>(memUsed), memUsed / 1024.0f,
      100.0f * memUsed / ESP.getHeapSize(),
      usedMb, freeMb,
      static_cast<unsigned long>(state::visitors()),
      static_cast<unsigned long>(state::dailyVisitors()),
      s.tempC, s.tempF,
      s.heatIndex, s.heatIndex * 9.0f / 5.0f + 32.0f,
      s.altitude, s.humidity, s.pressure,
      static_cast<unsigned>(s.co2), static_cast<unsigned>(s.voc),
      static_cast<unsigned long>(state::countriesTracked()),
      static_cast<unsigned long>(state::guestbookApproved()),
      s.bmeOk ? "true" : "false",
      s.ccsOk ? "true" : "false",
      sensors::displayOk() ? "true" : "false",
      today, hourNow);

  if (n < 0 || static_cast<size_t>(n) >= cap) return 0;

  // The Shelly power block is gone; the dumsor block is therefore always the
  // tail. Previously the two were mutually exclusive, which meant a configured
  // plug silently removed outage data from the dashboard.
  n += snprintf(out + n, cap - n,
      ",\"dumsor\":{\"outage_secs\":%lu,\"outage_at\":%lld,"
      "\"outages_total\":%lu,\"outages_month\":%lu,"
      "\"seconds_total\":%llu,\"seconds_month\":%llu}}",
      static_cast<unsigned long>(state::lastOutageSecs()),
      static_cast<long long>(state::lastOutageAt()),
      static_cast<unsigned long>(state::outagesTotal()),
      static_cast<unsigned long>(state::outagesMonth()),
      static_cast<unsigned long long>(state::outageSecondsTotal()),
      static_cast<unsigned long long>(state::outageSecondsMonth()));
  return static_cast<size_t>(n);
}

// Relayed requests arrive without HTTP headers of their own; the Worker
// already filtered them, so every one of them is public traffic.
void countVisitIfPublicRelay() { state::countVisit("??"); }

}  // namespace

// --- buildStatsJson (public) ----------------------------------------------

size_t buildStatsJson(char* out, size_t cap) {
  char raw[1400];
  const size_t n = buildStatsJsonRaw(raw, sizeof(raw));
  if (n == 0 || n >= cap) return 0;
  memcpy(out, raw, n + 1);
  return n;
}

// --- JSON builders shared by the LAN routes and the Worker relay ---------
// Both paths must emit byte-identical bodies, so each endpoint's body is
// built by exactly one function here.

static size_t buildCountriesJson(char* out, size_t cap) {
  size_t o = 0;
  o += snprintf(out + o, cap - o, "{");
  std::string raw;
  if (fsx::readAll("/countries.csv", raw, 4096)) {
    const char* p = raw.c_str();
    bool first = true;
    while (*p) {
      const char* eol = strchr(p, '\n');
      const size_t len = eol ? static_cast<size_t>(eol - p) : strlen(p);
      char line[32];
      if (len > 4 && len < sizeof(line)) {
        memcpy(line, p, len);
        line[len] = '\0';
        char* cr = strchr(line, '\r');
        if (cr) *cr = '\0';
        if (line[2] == ',') {
          o += snprintf(out + o, cap - o, "%s\"%c%c\":%s",
                        first ? "" : ",", line[0], line[1], line + 3);
          first = false;
        }
      }
      if (!eol) break;
      p = eol + 1;
    }
  }
  o += snprintf(out + o, cap - o, "}");
  return o;
}

// Lists the archived period labels. The year directory is flattened away,
// which is the shape the history page expects.
static size_t buildHistoryIndexJson(char* out, size_t cap) {
  struct tm t = {};
  const int thisYear = getLocalTime(&t, 0) ? (t.tm_year + 1900) : 2026;
  size_t o = 0;
  o += snprintf(out + o, cap - o, "{\"weekly\":[");
  auto emit = [&](const char* kind, bool* first) {
    for (int y = thisYear - 4; y <= thisYear; ++y) {
      char dir[64];
      snprintf(dir, sizeof(dir), "/stats/%s/%d", kind, y);
      if (!fsx::exists(dir)) continue;
      const int max = (strcmp(kind, "weekly") == 0) ? 53 : 12;
      for (int n = 1; n <= max; ++n) {
        char label[16], file[96];
        if (strcmp(kind, "weekly") == 0) {
          snprintf(label, sizeof(label), "%d-W%02d", y, n);
        } else {
          snprintf(label, sizeof(label), "%d-%02d", y, n);
        }
        snprintf(file, sizeof(file), "%s/%s.json", dir, label);
        if (!fsx::exists(file)) continue;
        o += snprintf(out + o, cap - o, "%s\"%s\"", *first ? "" : ",", label);
        *first = false;
      }
    }
  };
  bool first = true;
  emit("weekly", &first);
  o += snprintf(out + o, cap - o, "],\"monthly\":[");
  first = true;
  emit("monthly", &first);
  o += snprintf(out + o, cap - o, "],\"yearly\":[");
  first = true;
  for (int y = thisYear - 4; y <= thisYear; ++y) {
    char file[64], label[8];
    snprintf(file, sizeof(file), "/stats/yearly/%d.json", y);
    if (!fsx::exists(file)) continue;
    snprintf(label, sizeof(label), "%d", y);
    o += snprintf(out + o, cap - o, "%s\"%s\"", first ? "" : ",", label);
    first = false;
  }
  o += snprintf(out + o, cap - o, "],\"current\":{\"week\":\"%s\",\"month\":\"%s\",\"year\":\"%s\"}}",
                state::weekLabel(), state::monthLabel(), state::yearLabel());
  return o;
}

static size_t buildStatsCurrentJson(char* out, size_t cap) {
  // The three periods must not live inside `out`: snprintf would be reading the
  // %s arguments from the same bytes it is writing. Serialising into stack
  // buffers first keeps source and destination disjoint.
  char w[640], m[640], y[640];
  state::week().toJson(w, sizeof(w), state::weekLabel());
  state::month().toJson(m, sizeof(m), state::monthLabel());
  state::year().toJson(y, sizeof(y), state::yearLabel());
  return snprintf(out, cap, "{\"week\":%s,\"month\":%s,\"year\":%s}", w, m, y);
}

// Newest-first page of approved guestbook entries.
static size_t buildGuestbookEntriesJson(char* out, size_t cap, int pageNo,
                                       const String& needle, int* matchingOut) {
  int totalApproved = 0;
  int matching = 0;
  char countrySeen[64][3] = {};
  int countryCount = 0;

  walkEntries(true, [&](const Row* row) {
    if (needle.length() && !util::containsCI(row->name, needle.c_str()) &&
        !util::containsCI(row->message, needle.c_str())) {
      ++matching;
      return true;
    }
    ++totalApproved;
    if (row->country[0] != '?') {
      bool seen = false;
      for (int i = 0; i < countryCount; ++i) {
        if (strncmp(row->country, countrySeen[i], 2) == 0) { seen = true; break; }
      }
      if (!seen && countryCount < 64) {
        memcpy(countrySeen[countryCount], row->country, 3);
        ++countryCount;
      }
    }
    return true;
  });
  if (matchingOut) *matchingOut = matching;

  size_t o = 0;
  o += snprintf(out + o, cap - o, "{\"entries\":[");
  int emitted = 0;
  const int offset = (pageNo - 1) * static_cast<int>(kPageSize);
  walkEntries(true, [&](const Row* row) {
    if (needle.length() && !util::containsCI(row->name, needle.c_str()) &&
        !util::containsCI(row->message, needle.c_str())) {
      return true;
    }
    const int rank = totalApproved - 1 - emitted;
    if (rank < 0 || rank < offset) return true;
    if (emitted >= static_cast<int>(kPageSize)) return false;

    int replyCount = 0;
    walkEntries(true, [&](const Row* other) {
      if (strcmp(other->replyTo, row->id) == 0) ++replyCount;
      return true;
    });

    if (emitted++) o += snprintf(out + o, cap - o, ",");
    emitEntry(*row, out, cap, &o, false);
    o += snprintf(out + o, cap - o, ",\"reply_count\":%d", replyCount);
    if (replyCount >= 1 && replyCount <= 2) {
      o += snprintf(out + o, cap - o, ",\"preview_replies\":[");
      int pre = 0;
      walkEntries(true, [&](const Row* other) {
        if (pre >= 2) return false;
        if (strcmp(other->replyTo, row->id) != 0) return true;
        if (pre++) o += snprintf(out + o, cap - o, ",");
        emitEntry(*other, out, cap, &o, true);
        return true;
      });
      o += snprintf(out + o, cap - o, "]");
    }
    o += snprintf(out + o, cap - o, "}");
    return true;
  });

  const bool hasMore = (offset + emitted) < totalApproved;
  o += snprintf(out + o, cap - o, "],\"hasMore\":%s,\"total\":%d,\"countries\":%d",
                hasMore ? "true" : "false", totalApproved, countryCount);
  if (needle.length()) {
    o += snprintf(out + o, cap - o, ",\"matching\":%d}", matching);
  } else {
    o += snprintf(out + o, cap - o, "}");
  }
  return o;
}

static void xmlEscape(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (const char* p = in; *p && o + 7 < cap; ++p) {
    switch (*p) {
      case '&': o += snprintf(out + o, cap - o, "&amp;"); break;
      case '<': o += snprintf(out + o, cap - o, "&lt;"); break;
      case '>': o += snprintf(out + o, cap - o, "&gt;"); break;
      case '"': o += snprintf(out + o, cap - o, "&quot;"); break;
      default: out[o++] = *p;
    }
  }
  out[o] = '\0';
}

static size_t buildGuestbookRss(char* out, size_t cap) {
  size_t o = 0;
  o += snprintf(out + o, cap - o,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rss version=\"2.0\">\n<channel>\n"
      "<title>HelloESP guestbook</title>\n<link>/guestbook</link>\n<description>Recent messages</description>\n");
  int n = 0;
  walkEntries(true, [&](const Row* row) {
    if (row->replyTo[0]) return true;      // top-level only
    if (n >= 20) return false;
    ++n;
    char title[64], body[256];
    if (row->country[0] != '?') {
      snprintf(title, sizeof(title), "%s (%s)", row->name, row->country);
    } else {
      snprintf(title, sizeof(title), "%s", row->name);
    }
    xmlEscape(title, body, sizeof(body));
    char msg[256], safe[512];
    xmlEscape(row->message, msg, sizeof(msg));
    o += snprintf(out + o, cap - o,
        "<item><title>%s</title><link>/guestbook#%s</link>"
        "<guid>gb-%s</guid><pubDate>%s</pubDate><description>%s</description></item>\n",
        body, row->id, row->id, row->time, msg);
    (void)safe;
    return true;
  });
  o += snprintf(out + o, cap - o, "</channel>\n</rss>\n");
  return o;
}

// --- Worker relay entry point ---------------------------------------------
// The Worker forwards public HTTP over the device's outbound socket.
//
// Every forwarded request MUST get a response. A request left unanswered makes
// the Worker retry it while the original connection is still open, and enough
// of those pile up to exhaust lwIP's MEMP_SYS_TIMEOUT pool - which aborts the
// chip inside sys_timeout(). So every branch below terminates in a response,
// and the fallback answers 404 rather than falling silent.
static char g_relayBuf[3800];

void handleRelayedRequest(int32_t id, const char* method, const char* path,
                          const char* body) {
  (void)body;

  auto sendText = [&](int status, const char* text, const char* ct, const char* cc) {
    relay::sendResponse(id, status, ct, cc,
                        reinterpret_cast<const uint8_t*>(text), strlen(text));
  };
  auto sendJsonBuf = [&](const char* buf, const char* cc) {
    relay::sendResponse(id, 200, "application/json", cc,
                        reinterpret_cast<const uint8_t*>(g_relayBuf), strlen(g_relayBuf));
  };
  auto sendAsset = [&](const char* file, const char* cc) {
    char gz[160];
    snprintf(gz, sizeof(gz), "%s.gz", file);
    if (fsx::exists(gz)) relay::sendFile(id, gz, "text/html", "gzip", cc);
    else if (fsx::exists(file)) relay::sendFile(id, file, "text/html", nullptr, cc);
    else relay::sendFile(id, "/404.html", "text/html", nullptr, "no-store");
  };

  // Relayed traffic has already been filtered by the Worker, so it is public
  // traffic by definition and every hit counts.
  auto isPage = [&](const char* route, const char* file) {
    if (strcmp(path, route) != 0) return false;
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
      countVisitIfPublicRelay();
    }
    sendAsset(file, "public, max-age=60");
    return true;
  };

  if (isPage("/", "/index.html")) return;
  if (isPage("/index.html", "/index.html")) return;
  if (isPage("/about", "/about.html")) return;
  if (isPage("/about.html", "/about.html")) return;
  if (isPage("/console", "/console.html")) return;
  if (isPage("/console.html", "/console.html")) return;
  if (isPage("/guestbook", "/guestbook.html")) return;
  if (isPage("/guestbook.html", "/guestbook.html")) return;
  if (isPage("/history", "/history.html")) return;
  if (isPage("/history.html", "/history.html")) return;
  if (isPage("/adsb", "/adsb.html")) return;
  if (isPage("/adsb.html", "/adsb.html")) return;

  if (strcmp(path, "/ping") == 0) {
    sendText(200, "pong", "text/plain", "no-store");
    return;
  }

  if (strcmp(path, "/stats") == 0 || strcmp(path, "/stats.json") == 0) {
    
    const size_t n = buildStatsJsonRaw(g_relayBuf, sizeof(g_relayBuf));
    if (n) sendJsonBuf(g_relayBuf, "no-store");
    else sendText(500, "stats unavailable", "text/plain", "no-store");
    return;
  }

  if (strcmp(path, "/stats/current") == 0) {
    
    buildStatsCurrentJson(g_relayBuf, sizeof(g_relayBuf));
    sendJsonBuf(g_relayBuf, "public, max-age=300");
    return;
  }

  if (strcmp(path, "/records.json") == 0) {
    
    state::recordsJson(g_relayBuf, sizeof(g_relayBuf));
    sendJsonBuf(g_relayBuf, "public, max-age=300");
    return;
  }

  if (strcmp(path, "/countries") == 0) {
    
    buildCountriesJson(g_relayBuf, sizeof(g_relayBuf));
    sendJsonBuf(g_relayBuf, "public, max-age=60");
    return;
  }

  if (strcmp(path, "/history.json") == 0) {
    
    buildHistoryIndexJson(g_relayBuf, sizeof(g_relayBuf));
    sendJsonBuf(g_relayBuf, "public, max-age=300");
    return;
  }

  if (strcmp(path, "/console.json") == 0) {
    // The console log is now Worker-side; the device has no per-request log.
    sendJsonBuf("{\"entries\":[]}", "no-store");
    return;
  }

  if (strcmp(path, "/adsb.json") == 0) {
    // Tracking moved to the Worker; the page self-hides on an empty fleet.
    sendJsonBuf("{\"now\":0,\"aircraft\":[]}", "no-store");
    return;
  }

  if (strcmp(path, "/guestbook/entries") == 0) {
    
    int page = 1;
    String needle;
    // The relay delivers the query string inside `path`.
    const char* qp = strchr(path, '?');
    if (qp) {
      if (strncmp(qp, "?page=", 6) == 0) page = atoi(qp + 6);
      const char* q = strstr(qp, "q=");
      if (q) {
        needle = q + 2;
        const int amp = needle.indexOf('&');
        if (amp >= 0) needle = needle.substring(0, amp);
      }
    }
    if (page < 1) page = 1;
    if (needle.length() > 64) needle = needle.substring(0, 64);
    buildGuestbookEntriesJson(g_relayBuf, sizeof(g_relayBuf), page, needle, nullptr);
    sendJsonBuf(g_relayBuf, needle.length() ? nullptr : "public, max-age=30");
    return;
  }

  if (strcmp(path, "/guestbook.rss") == 0) {
    
    buildGuestbookRss(g_relayBuf, sizeof(g_relayBuf));
    relay::sendResponse(id, 200, "application/rss+xml", "public, max-age=600",
                        reinterpret_cast<const uint8_t*>(g_relayBuf), strlen(g_relayBuf));
    return;
  }

  if (strncmp(path, "/stats/weekly/", 14) == 0 ||
      strncmp(path, "/stats/monthly/", 15) == 0) {
    const bool weekly = path[14] == 'w';
    const char* label = path + (weekly ? 14 : 15);
    const size_t labelLen = strlen(label);
    if (strchr(label, '/') != nullptr || (weekly ? labelLen != 8 : labelLen != 7) ||
        label[0] == '.') {
      sendText(400, "bad label", "text/plain", "no-store");
      return;
    }
    char file[96];
    snprintf(file, sizeof(file), weekly ? "/stats/weekly/%.4s/%s.json"
                                        : "/stats/monthly/%.4s/%s.json",
             label, label);
    if (!fsx::exists(file)) {
      sendText(404, "not found", "text/plain", "no-store");
      return;
    }
    
    std::string body2;
    if (!fsx::readAll(file, body2, sizeof(g_relayBuf) - 1)) {
      sendText(404, "not found", "text/plain", "no-store");
      return;
    }
    sendJsonBuf(body2.c_str(), "public, max-age=31536000, immutable");
    return;
  }

  if (strcmp(path, "/robots.txt") == 0) {
    sendText(200, "User-agent: *\nDisallow: /admin\n", "text/plain", "public, max-age=86400");
    return;
  }

  // Anything else: answer, never hang.
  if (fsx::exists("/404.html.gz")) {
    relay::sendFile(id, "/404.html.gz", "text/html", "gzip", "no-store");
    return;
  }
  sendText(404, "not found", "text/plain", "no-store");
}

static char g_lanBuf[3800];

// --- routes ---------------------------------------------------------------


void begin() {
  // --- static pages ---
  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    countVisitIfPublic(r);
    sendAsset(r, "/index.html", "public, max-age=60");
  });
  g_server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/index.html", "public, max-age=60");
  });
  g_server.on("/about", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/about.html", "public, max-age=300");
  });
  g_server.on("/about.html", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/about.html", "public, max-age=300");
  });
  g_server.on("/console", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/console.html", "public, max-age=300");
  });
  g_server.on("/console.html", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/console.html", "public, max-age=300");
  });
  g_server.on("/guestbook", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/guestbook.html", "public, max-age=300");
  });
  g_server.on("/guestbook.html", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/guestbook.html", "public, max-age=300");
  });
  g_server.on("/history", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/history.html", "public, max-age=300");
  });
  g_server.on("/history.html", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/history.html", "public, max-age=300");
  });
  g_server.on("/adsb", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/adsb.html", "public, max-age=300");
  });
  g_server.on("/adsb.html", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/adsb.html", "public, max-age=300");
  });
  g_server.on("/admin", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/admin.html", "no-store");
  });
  g_server.on("/admin.html", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendAsset(r, "/admin.html", "no-store");
  });

  // --- health ---
  g_server.on("/ping", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/plain", "pong");
  });

  // --- dashboard payload ---
  g_server.on("/stats", HTTP_GET, [](AsyncWebServerRequest* r) {
    
    if (buildStatsJsonRaw(g_lanBuf, sizeof(g_lanBuf))) sendJson(r, g_lanBuf, "no-store");
    else sendText(r, 500, "stats unavailable");
  });

  // The Shelly/ADS-B features are gone. Both endpoints stay registered and
  // report an empty, valid shape so the pages self-hide instead of erroring.
  g_server.on("/adsb.json", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendJson(r, "{\"now\":0,\"aircraft\":[]}", "no-store");
  });

  g_server.on("/records.json", HTTP_GET, [](AsyncWebServerRequest* r) {
    
    state::recordsJson(g_lanBuf, sizeof(g_lanBuf));
    sendJson(r, g_lanBuf, "public, max-age=300");
  });

  g_server.on("/countries", HTTP_GET, [](AsyncWebServerRequest* r) {
    
    size_t o = 0;
    o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "{");
    std::string raw;
    if (fsx::readAll("/countries.csv", raw, 4096)) {
      const char* p = raw.c_str();
      bool first = true;
      while (*p) {
        const char* eol = strchr(p, '\n');
        const size_t len = eol ? static_cast<size_t>(eol - p) : strlen(p);
        char line[32];
        if (len > 4 && len < sizeof(line)) {
          memcpy(line, p, len);
          line[len] = '\0';
          char* cr = strchr(line, '\r');
          if (cr) *cr = '\0';
          if (line[2] == ',') {
            o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "%s\"%c%c\":%s",
                          first ? "" : ",", line[0], line[1], line + 3);
            first = false;
          }
        }
        if (!eol) break;
        p = eol + 1;
      }
    }
    snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "}");
    sendJson(r, g_lanBuf, "public, max-age=60");
  });

  g_server.on("/console.json", HTTP_GET, [](AsyncWebServerRequest* r) {
    sendJson(r, "{\"entries\":[]}", "no-store");
  });

  // --- history ---
  g_server.on("/history.json", HTTP_GET, [](AsyncWebServerRequest* r) {
    
    size_t o = 0;
    o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "{\"weekly\":[");
    // Archives live at /stats/{weekly,monthly}/<year>/<label>.json; the API
    // flattens the year directory away.
    auto emitList = [&](const char* kind, bool* first) {
      char yearDir[48];
      snprintf(yearDir, sizeof(yearDir), "/stats/%s", kind);
      if (!fsx::exists(yearDir)) return;
      char scan[64];
      snprintf(scan, sizeof(scan), "/stats/%s/2026", kind);
      // Years are discovered by probing a small range around the current one.
      struct tm t = {};
      int thisYear = getLocalTime(&t, 0) ? (t.tm_year + 1900) : 2026;
      for (int y = thisYear - 3; y <= thisYear; ++y) {
        char dir[64], file[96];
        snprintf(dir, sizeof(dir), "/stats/%s/%d", kind, y);
        if (!fsx::exists(dir)) continue;
        for (int w = 1; w <= 53; ++w) {
          char label[16];
          if (strcmp(kind, "weekly") == 0) snprintf(label, sizeof(label), "%d-W%02d", y, w);
          else snprintf(label, sizeof(label), "%d-%02d", y, w);
          snprintf(file, sizeof(file), "%s/%s.json", dir, label);
          if (!fsx::exists(file)) continue;
          o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "%s\"%s\"", *first ? "" : ",", label);
          *first = false;
        }
      }
      (void)scan;
    };
    bool first = true;
    emitList("weekly", &first);
    o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "],\"monthly\":[");
    first = true;
    emitList("monthly", &first);
    o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "],\"yearly\":[");
    first = true;
    for (int y = 2020; y <= 2100; ++y) {
      char file[64], label[8];
      snprintf(file, sizeof(file), "/stats/yearly/%d.json", y);
      if (!fsx::exists(file)) continue;
      snprintf(label, sizeof(label), "%d", y);
      o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "%s\"%s\"", first ? "" : ",", label);
      first = false;
    }
    snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o,
             "],\"current\":{\"week\":\"%s\",\"month\":\"%s\",\"year\":\"%s\"}}",
             state::weekLabel(), state::monthLabel(), state::yearLabel());
    sendJson(r, g_lanBuf, "public, max-age=300");
  });

  g_server.on("/stats/current", HTTP_GET, [](AsyncWebServerRequest* r) {
    
    char w[640], m[640], y[640];
    state::week().toJson(w, sizeof(w), state::weekLabel());
    state::month().toJson(m, sizeof(m), state::monthLabel());
    state::year().toJson(y, sizeof(y), state::yearLabel());
    snprintf(g_lanBuf, sizeof(g_lanBuf), "{\"week\":%s,\"month\":%s,\"year\":%s}", w, m, y);
    sendJson(r, g_lanBuf, "public, max-age=300");
  });

  // Archive passthrough with an immutable cache header: these never change
  // once written.
  g_server.on("/stats/weekly/*", HTTP_GET, [](AsyncWebServerRequest* r) {
    const String label = r->url().substring(strlen("/stats/weekly/"));
    if (label.length() != 8 || label.indexOf("..") >= 0 || label.indexOf('/') >= 0) {
      sendText(r, 400, "bad label");
      return;
    }
    char file[64];
    snprintf(file, sizeof(file), "/stats/weekly/%.4s/%s.json", label.c_str(), label.c_str());
    if (!fsx::exists(file)) {
      sendText(r, 404, "not found");
      return;
    }
    std::string body;
    if (!fsx::readAll(file, body, 2048)) {
      sendText(r, 404, "not found");
      return;
    }
    sendJson(r, body.c_str(), "public, max-age=31536000, immutable");
  });

  g_server.on("/stats/monthly/*", HTTP_GET, [](AsyncWebServerRequest* r) {
    const String label = r->url().substring(strlen("/stats/monthly/"));
    if (label.length() != 7 || label.indexOf("..") >= 0 || label.indexOf('/') >= 0) {
      sendText(r, 400, "bad label");
      return;
    }
    char file[64];
    snprintf(file, sizeof(file), "/stats/monthly/%.4s/%s.json", label.c_str(), label.c_str());
    if (!fsx::exists(file)) {
      sendText(r, 404, "not found");
      return;
    }
    std::string body;
    if (!fsx::readAll(file, body, 2048)) {
      sendText(r, 404, "not found");
      return;
    }
    sendJson(r, body.c_str(), "public, max-age=31536000, immutable");
  });

  // --- guestbook ---
  g_server.on("/guestbook/entries", HTTP_GET, [](AsyncWebServerRequest* r) {
    
    const int page = r->hasParam("page") ? atoi(r->getParam("page")->value().c_str()) : 1;
    const int pageNo = (page < 1) ? 1 : page;
    String q;
    if (r->hasParam("q")) q = r->getParam("q")->value();
    if (q.length() > 64) q = q.substring(0, 64);

    // Two passes over the file instead of caching rows in RAM: guestbook.csv can
    // be larger than the heap, and a row array large enough to hold it would
    // cost more DRAM than the whole application.
    //   pass 1: counts + country tally
    //   pass 2: emit just this page
    int totalApproved = 0;
    int matching = 0;
    char countrySeen[64][3] = {};
    int countryCount = 0;
    const String needle = q;

    walkEntries(true, [&](const Row* row) {
      if (needle.length() && !util::containsCI(row->name, needle.c_str()) &&
          !util::containsCI(row->message, needle.c_str())) {
        ++matching;
        return true;
      }
      ++totalApproved;
      if (row->country[0] != '?') {
        bool seen = false;
        for (int i = 0; i < countryCount; ++i) {
          if (strncmp(row->country, countrySeen[i], 2) == 0) { seen = true; break; }
        }
        if (!seen && countryCount < 64) {
          memcpy(countrySeen[countryCount], row->country, 3);
          ++countryCount;
        }
      }
      return true;
    });

    size_t o = 0;
    o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "{\"entries\":[");
    int emitted = 0;
    walkEntries(true, [&](const Row* row) {
      if (needle.length() && !util::containsCI(row->name, needle.c_str()) &&
          !util::containsCI(row->message, needle.c_str())) {
        return true;
      }
      // Newest first: skip down to this page without buffering.
      const int rank = totalApproved - 1 - emitted;
      if (rank < 0) return true;
      const int offset = (pageNo - 1) * static_cast<int>(kPageSize);
      if (rank < offset) return true;
      if (emitted >= static_cast<int>(kPageSize)) return false;

      // Count direct replies so the UI can decide between an inline preview and
      // a "show N replies" link.
      int replyCount = 0;
      walkEntries(true, [&](const Row* other) {
        if (strcmp(other->replyTo, row->id) == 0) ++replyCount;
        return true;
      });

      if (emitted++) o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, ",");
      emitEntry(*row, g_lanBuf, sizeof(g_lanBuf), &o, false);
      o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, ",\"reply_count\":%d", replyCount);
      if (replyCount >= 1 && replyCount <= 2) {
        o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, ",\"preview_replies\":[");
        int pre = 0;
        walkEntries(true, [&](const Row* other) {
          if (pre >= 2) return false;
          if (strcmp(other->replyTo, row->id) != 0) return true;
          if (pre++) o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, ",");
          emitEntry(*other, g_lanBuf, sizeof(g_lanBuf), &o, true);
          return true;
        });
        o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "]");
      }
      o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "}");
      return true;
    });

    const int offset = (pageNo - 1) * static_cast<int>(kPageSize);
    const bool hasMore = (offset + emitted) < totalApproved;
    o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o,
                  "],\"hasMore\":%s,\"total\":%d,\"countries\":%d",
                  hasMore ? "true" : "false", totalApproved, countryCount);
    if (q.length()) {
      o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, ",\"matching\":%d}", matching);
    } else {
      o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "}");
    }
    sendJson(r, g_lanBuf, q.length() ? nullptr : "public, max-age=30");
  });

  g_server.on("/guestbook/submit", HTTP_POST, [](AsyncWebServerRequest* r) {
    const String name = r->hasParam("name") ? r->getParam("name")->value() : "";
    const String msg = r->hasParam("message") ? r->getParam("message")->value() : "";
    const String replyTo = r->hasParam("reply_to") ? r->getParam("reply_to")->value() : "";
    char ip[16];
    snprintf(ip, sizeof(ip), "%u", static_cast<unsigned>(ipOf(r)));

    if (guestbook::rateLimited(ip)) {
      r->send(429, "text/plain", "Please wait before posting again");
      return;
    }
    const char* err = nullptr;
    char cc[4] = "??";
    util::normalizeCountry(r->header("CF-IPCountry").c_str(), cc);
    if (!guestbook::submit(name.c_str(), msg.c_str(),
                           replyTo.length() ? replyTo.c_str() : nullptr, cc, &err)) {
      r->send(err && strstr(err, "full") ? 429 : 400, "text/plain", err ? err : "Rejected");
      return;
    }
    guestbook::noteSubmitAttempt(ip);
    r->send(200, "text/plain", "Thanks! Your message will appear after review.");
  });

  // --- admin ---
  g_server.on("/admin/info", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!adminAuth(r)) {
      r->send(401, "text/plain", "Unauthorized");
      return;
    }
    
    const char* reasons[] = {"Unknown", "Power on", "External", "Software", "Panic",
                             "Watchdog (int)", "Watchdog (task)", "Watchdog (other)",
                             "Deep sleep", "Brownout", "SDIO"};
    const int reason = static_cast<int>(esp_reset_reason());
    const char* reasonStr = (reason >= 0 && reason <= 10) ? reasons[reason] : "Unknown";
    const unsigned long up = millis() / 1000UL;
    const size_t usedBytes = fsx::usedBytes();
    const size_t totalBytes = fsx::totalBytes();

    snprintf(g_lanBuf, sizeof(g_lanBuf),
        "{\"firmware\":\"%s\",\"chip_model\":\"%s\",\"chip_revision\":%d,"
        "\"cpu_freq_mhz\":%d,\"flash_size_mb\":%.1f,\"sdk_version\":\"%s\","
        "\"time_t_bytes\":%d,\"mac_address\":\"%s\",\"local_ip\":\"%s\","
        "\"gateway\":\"%s\",\"dns\":\"%s\",\"subnet\":\"%s\","
        "\"free_heap\":%lu,\"min_free_heap\":%lu,\"heap_size\":%lu,"
        "\"last_reset\":\"%s\",\"fs_writes_suspended\":%s,\"fs_crash_count\":%u,"
        "\"fs_health\":\"%s\","
        "\"uptime\":\"%lu days, %lu hours, %lu minutes, %lu seconds\","
        "\"rssi\":%d,\"worker_configured\":%s,\"worker_connected\":%s,"
        "\"worker_reconnects\":%lu,\"storage_used_bytes\":%lu,\"storage_total_bytes\":%lu,"
        "\"bme280_retired\":%s,\"ccs811_retired\":%s}",
        FIRMWARE_VERSION_STR, ESP.getChipModel(), ESP.getChipRevision(),
        ESP.getCpuFreqMHz(), ESP.getFlashChipSize() / (1024.0f * 1024.0f),
        ESP.getSdkVersion(), static_cast<int>(sizeof(time_t)),
        WiFi.macAddress().c_str(), WiFi.localIP().toString().c_str(),
        WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(),
        WiFi.subnetMask().toString().c_str(),
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(ESP.getMinFreeHeap()),
        static_cast<unsigned long>(ESP.getHeapSize()),
        reasonStr, fsx::writesSuspended() ? "true" : "false", fsx::bootFailures(),
        fsx::health(),
        up / 86400UL, (up / 3600UL) % 24, (up / 60UL) % 60, up % 60,
        WiFi.RSSI(),
        config::v.workerUrl[0] ? "true" : "false",
        relay::connected() ? "true" : "false",
        static_cast<unsigned long>(relay::reconnectCount()),
        static_cast<unsigned long>(usedBytes), static_cast<unsigned long>(totalBytes),
        sensors::bmeRetired() ? "true" : "false",
        sensors::ccsRetired() ? "true" : "false");
    sendJson(r, g_lanBuf, nullptr);
  });

  g_server.on("/admin/export", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!adminAuth(r)) {
      r->send(401, "text/plain", "Unauthorized");
      return;
    }
    if (!guardWritable(r)) return;
    if (!fsx::mkdirs("/fw")) {
      sendText(r, 500, "cannot create /fw");
      return;
    }
    const char* which = r->hasParam("f") ? r->getParam("f")->value().c_str() : "state.bin";
    char name[32];
    snprintf(name, sizeof(name), "%s", which);
    for (char* p = name; *p; ++p) {
      const char c = *p;
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-')) {
        sendText(r, 400, "bad name");
        return;
      }
    }
    const size_t before = fsx::usedBytes();
    const size_t total = fsx::totalBytes();
    if (total - before < 64 * 1024) {
      sendText(r, 500, "not enough free space to stage an export");
      return;
    }
    std::string body;
    if (!fsx::readAll("/state.bin", body, 2048)) {
      sendText(r, 500, "no state to export");
      return;
    }
    char out[96];
    snprintf(out, sizeof(out), "/fw/%s", name);
    if (!fsx::writeFile(out, body.data(), body.size())) {
      sendText(r, 500, "export failed");
      return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "staged /fw/%s (%u bytes)", name, (unsigned)body.size());
    sendText(r, 200, msg);
  });

  g_server.on("/admin/reset", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!adminAuth(r)) {
      r->send(401, "text/plain", "Unauthorized");
      return;
    }
    if (!guardWritable(r)) return;
    const char* target = r->hasParam("target") ? r->getParam("target")->value().c_str() : "";
    if (strcmp(target, "visitors") != 0) {
      sendText(r, 400, "unknown target");
      return;
    }
    state::resetVisitors();
    sendText(r, 200, "visitors reset");
  });

  g_server.on("/guestbook/pending", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!adminAuth(r)) {
      r->send(401, "text/plain", "Unauthorized");
      return;
    }
    
    size_t o = 0;
    o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, "{\"entries\":[");
    int shown = 0;
    int idx = 0;
    walkEntries(false, [&](const Row* row) {
      const int myIdx = idx++;
      if (row->status != '0') return true;
      if (shown >= static_cast<int>(kPageSize)) return false;
      if (shown++) o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o, ",");
      char m[256] = "", n[48] = "", t[40] = "";
      util::jsonEscape(row->message, m, sizeof(m));
      util::jsonEscape(row->name, n, sizeof(n));
      util::jsonEscape(row->time, t, sizeof(t));
      o += snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o,
                    "{\"idx\":%d,\"time\":\"%s\",\"country\":\"%s\",\"name\":\"%s\","
                    "\"message\":\"%s\",\"id\":\"%s\",\"reply_to\":\"%s\",\"approved\":0}",
                    myIdx, t, row->country, n, m, row->id, row->replyTo);
      return true;
    });
    snprintf(g_lanBuf + o, sizeof(g_lanBuf) - o,
             "],\"hasMore\":false,\"counts\":{\"new\":%u,\"approved\":%u,\"denied\":0,\"all\":%u}}",
             static_cast<unsigned>(guestbook::pendingCount()),
             static_cast<unsigned>(guestbook::approvedCount()),
             static_cast<unsigned>(guestbook::allCount()));
    sendJson(r, g_lanBuf, nullptr);
  });

  g_server.on("/guestbook/moderate-batch", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!adminAuth(r)) {
      r->send(401, "text/plain", "Unauthorized");
      return;
    }
    if (!guardWritable(r)) return;
    if (!r->hasParam("ops")) {
      sendText(r, 400, "missing ops");
      return;
    }
    const String ops = r->getParam("ops")->value();
    if (ops.length() > 4000) {
      sendText(r, 400, "ops too long");
      return;
    }
    uint16_t applied = 0;
    guestbook::moderateBatch(ops.c_str(), &applied);
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"applied\":%u,\"deletes\":false}", (unsigned)applied);
    sendJson(r, msg, nullptr);
  });

  g_server.on("/admin/backup-now", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!adminAuth(r)) {
      r->send(401, "text/plain", "Unauthorized");
      return;
    }
    backup::request();
    sendText(r, 200, "backup queued");
  });

  g_server.on("/admin/selftest", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!adminAuth(r)) {
      r->send(401, "text/plain", "Unauthorized");
      return;
    }
    sensors::Reading s{};
    sensors::read(&s);
    const bool fsOk = fsx::writable();
    char msg[320];
    snprintf(msg, sizeof(msg),
             "{\"ok\":%s,\"bme280\":%s,\"ccs811\":%s,\"storage\":%s,"
             "\"heap\":%lu,\"uptime_s\":%lu}",
             (fsOk && s.bmeOk) ? "true" : "false",
             s.bmeOk ? "ok" : "fail", s.ccsOk ? "ok" : "fail",
             fsx::health(),
             static_cast<unsigned long>(ESP.getFreeHeap()),
             static_cast<unsigned long>(millis() / 1000));
    sendJson(r, msg, nullptr);
  });

  g_server.on("/admin/unretire", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!adminAuth(r)) {
      r->send(401, "text/plain", "Unauthorized");
      return;
    }
    sensors::clearRetired();
    sendText(r, 200, "sensor retirement cleared");
  });

  g_server.onNotFound([](AsyncWebServerRequest* r) {
    // Genuinely unknown paths only. This must not hijack /stats/*: doing so
    // shadowed /stats/current and served the dashboard blob in its place.
    sendAsset(r, "/404.html", "no-store");
  });

  g_server.begin();
}

}  // namespace web