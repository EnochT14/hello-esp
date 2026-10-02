#include "config.h"

#include <stdlib.h>
#include <time.h>

#include "storage.h"

namespace config {

Value v;

namespace {

// Copies at most cap-1 chars and always NUL-terminates.
void setStr(char* dst, size_t cap, const String& src) {
  const size_t n = (src.length() < cap - 1) ? src.length() : cap - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

// Strips control characters, which is the defence against header injection
// when these values end up in HTTP responses or the Worker handshake.
String scrub(const String& in) {
  String out;
  out.reserve(in.length());
  for (unsigned i = 0; i < in.length(); ++i) {
    const char c = in[i];
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) continue;
    out += c;
  }
  return out;
}

void handle(const String& key, const String& raw) {
  const String val = scrub(raw);
  if (key == "wifi_ssid")           setStr(v.wifiSsid, sizeof(v.wifiSsid), val);
  else if (key == "wifi_pass")      setStr(v.wifiPass, sizeof(v.wifiPass), val);
  else if (key == "admin_user")     setStr(v.adminUser, sizeof(v.adminUser), val);
  else if (key == "admin_pass")     setStr(v.adminPass, sizeof(v.adminPass), val);
  else if (key == "worker_url")     setStr(v.workerUrl, sizeof(v.workerUrl), val);
  else if (key == "worker_key")     setStr(v.workerKey, sizeof(v.workerKey), val);
  else if (key == "device_key")     setStr(v.deviceKey, sizeof(v.deviceKey), val);
  else if (key == "timezone")       setStr(v.timezone, sizeof(v.timezone), val);
  else if (key == "worker_exclusive") v.workerExclusive = (val == "true");
  else if (key == "dumsor_tracking")  v.dumsorTracking  = (val == "true");
  // shelly_*, cost_per_kwh, co2_per_kwh and adsb_* were removed with their
  // features. They are intentionally ignored rather than rejected so an
  // existing config.txt keeps loading unchanged.
}

}  // namespace

bool load() {
  std::string raw;
  if (!fsx::readAll("/config.txt", raw, 8192)) return false;

  v = Value();  // defaults

  const char* p = raw.c_str();
  while (*p) {
    const char* eol = strchr(p, '\n');
    String line;
    if (eol) {
      line = String(p, eol - p);
      p = eol + 1;
    } else {
      line = String(p);
      p += strlen(p);
    }
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;

    const int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();
    if (key.length() == 0) continue;
    handle(key, val);
  }
  return true;
}

void applyTimezone() {
  setenv("TZ", v.timezone, 1);
  tzset();
}

}  // namespace config