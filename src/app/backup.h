// Off-site backup: streams the filesystem to the Worker, which writes it to R2.
//
// Read-only by construction - it walks the filesystem and never writes - so a
// backup still runs when the device has latched itself read-only, which is
// exactly when a backup matters most.
#pragma once

#include <Arduino.h>

namespace backup {

void begin();

// Called by the relay layer when the Worker confirms the R2 write landed.
void onCommitted(const char* date);

// True while a transfer is in progress.
bool running();

// Last known outcome, for /admin/info.
bool lastSucceeded();
const char* lastError();

// Requests a run at the next tick if one is not already going.
void request();

// Runs the daily 04:00 backup when the date has rolled over and no backup has
// happened yet today. Call from loop().
void tick();
void tick(bool forceNow);

}  // namespace backup