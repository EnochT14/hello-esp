// HTTP surface.
//
// The JSON shapes here are a contract with data/*.html and with the Worker, so
// they are reproduced exactly as the previous firmware emitted them - including
// field order and which fields are conditional. The pages are already written
// and tested; rewriting them alongside the backend would be risk without
// benefit.
#pragma once

#include <Arduino.h>

namespace web {

// Builds the dashboard payload. Also used verbatim as the Worker push.
size_t buildStatsJson(char* out, size_t cap);

void begin();

// Serves one request forwarded by the Worker over the relay socket.
void handleRelayedRequest(int32_t id, const char* method, const char* path,
                          const char* body);

}  // namespace web