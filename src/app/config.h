// Device configuration, read from /config.txt on LittleFS.
//
// The file format is unchanged from v1.5 on purpose: the live config.txt only
// exists on the device (it is deliberately excluded from R2 backups because it
// holds secrets), so changing the key names would lock the owner out of their
// own WiFi and Worker credentials. Keys added or dropped by the rewrite are
// listed at the bottom; removed ones are simply ignored on read.
#pragma once

#include <Arduino.h>

namespace config {

// Field maxima, matching the on-disk limits the v1.5 parser enforced.
struct Value {
  char wifiSsid[64]     = "";
  char wifiPass[64]     = "";
  char adminUser[32]    = "admin";
  char adminPass[64]    = "";
  char workerUrl[128]   = "";   // hostname only, no scheme
  char workerKey[128]   = "";
  char deviceKey[128]   = "";   // HMAC secret; blank disables HMAC
  char timezone[48]     = "UTC0";
  bool  workerExclusive = false;
  bool  dumsorTracking  = true;
};

extern Value v;

// Reads /config.txt. Returns false when the file is absent or unparseable,
// in which case the caller must not start the network stack.
bool load();

// Applies the POSIX TZ string to the C runtime.
void applyTimezone();

}  // namespace config