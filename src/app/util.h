// Small shared helpers. Header-only so the compiler can inline them.
#pragma once

#include <Arduino.h>
#include <math.h>

namespace util {

// ISO-8601 with a numeric TZ offset and no colon, matching every timestamp
// already in the stored data: "2026-10-02T09:15:16+0000".
// Returns false when the clock has never been set.
inline bool timestamp(char* out, size_t cap) {
  struct tm t;
  if (!getLocalTime(&t, 0)) {
    snprintf(out, cap, "unknown");
    return false;
  }
  strftime(out, cap, "%Y-%m-%dT%H:%M:%S%z", &t);
  return true;
}

// Barometric altitude, derived from an already-bounded pressure so a glitched
// pressure reading cannot produce an absurd altitude.
inline float altitudeFt(float pressureHpa, float seaLevelHpa = 1013.25f) {
  if (isnan(pressureHpa) || pressureHpa < 300.0f || pressureHpa > 1100.0f) return 0.0f;
  const double meters = 44330.0 * (1.0 - pow(pressureHpa / seaLevelHpa, 0.1903));
  return static_cast<float>(meters * 3.28084);
}

// Rothfusz heat index in Celsius; falls back to ambient below 26.7C.
inline float heatIndexC(float tempC, float humidity) {
  if (isnan(tempC) || isnan(humidity) || tempC < 26.7f) return tempC;
  double t = tempC;
  double r = humidity;
  const double hi = -42.379 + 2.04901523 * t + 10.14333127 * r
                  - 0.22475541 * t * r - 0.00683783 * t * t
                  - 0.05481717 * r * r + 0.00122874 * t * t * r
                  + 0.00085282 * t * r * r - 0.00000199 * t * t * r * r;
  return static_cast<float>(hi);
}

// JSON string escaping. Bytes >= 0x80 pass through untouched so UTF-8 text
// stays readable rather than being escaped into noise.
inline void jsonEscape(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in); *p && o + 7 < cap; ++p) {
    const unsigned char c = *p;
    switch (c) {
      case '"':  out[o++] = '\\'; out[o++] = '"';  break;
      case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
      case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
      case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
      case '\t': out[o++] = '\\'; out[o++] = 't';  break;
      default:
        if (c < 0x20) {
          o += snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
          out[o++] = static_cast<char>(c);
        }
    }
  }
  out[o] = '\0';
}

// Case-insensitive substring test used by guestbook search.
inline bool containsCI(const char* haystack, const char* needle) {
  if (!needle || !*needle) return true;
  const size_t nl = strlen(needle);
  for (const char* p = haystack; *p; ++p) {
    size_t i = 0;
    while (i < nl && p[i]) {
      char a = p[i], b = needle[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) break;
      ++i;
    }
    if (i == nl) return true;
  }
  return false;
}

// True when the remote address is on a private/reserved range, i.e. a LAN
// visitor rather than real public traffic.
inline bool isPrivateIp(uint32_t ip) {
  const uint8_t o0 = static_cast<uint8_t>(ip);
  const uint8_t o1 = static_cast<uint8_t>(ip >> 8);
  if (o0 == 10) return true;
  if (o0 == 192 && o1 == 168) return true;
  if (o0 == 172 && o1 >= 16 && o1 <= 31) return true;
  if (o0 == 127) return true;
  if (o0 == 169 && o1 == 254) return true;
  return false;
}

// Normalises a country code to two uppercase A-Z, or "??" for anything else.
inline void normalizeCountry(const char* in, char* out) {
  out[0] = out[1] = '?';
  out[2] = '\0';
  if (!in || !in[0] || !in[1]) return;
  const char a = in[0], b = in[1];
  if (a < 'A' || a > 'Z' || b < 'A' || b > 'Z') return;
  if (a == 'X' && b == 'X') return;  // XX is the reserved "unknown" code
  out[0] = a;
  out[1] = b;
}

}  // namespace util