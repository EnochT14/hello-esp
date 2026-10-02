// Outbound relay to the Cloudflare Worker.
//
// The device dials out and keeps one WebSocket open; the Worker multiplexes
// public HTTP traffic over it. No inbound connection from the internet is ever
// accepted, which is the entire security model.
//
// The WebSocket framing is hand-rolled (Arduino has no client) and deliberately
// allocation-free on the hot path: frames are written straight into the
// TLS client's buffer from a caller-provided buffer.
#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

namespace relay {

void begin();
void end();

// True while the socket is open AND authenticated.
bool connected();

// Milliseconds since the last byte the Worker sent us.
unsigned long lastActivityMs();

// Attempts to (re)connect when due. Call from loop(); never blocks for long.
void tick();

// Non-blocking outbound JSON. Returns false when not connected.
bool sendJson(const char* json, size_t len);

// --- events pushed to the Worker -----------------------------------------

// Device -> Worker
void pushStats(const char* statsJson);
void pushConsole(const char* json);
void pushBackupStart(uint32_t seq, const char* generatedAt, const char* firmware,
                     const char* uptime, size_t totalBytes);
void pushBackupFileStart(uint32_t seq, const char* name, size_t size);
void pushBackupFileChunk(uint32_t seq, const char* b64);
void pushBackupFileEnd(uint32_t seq, const char* name);
void pushBackupFileSkipped(uint32_t seq, const char* name, size_t size, const char* reason);
void pushBackupEnd(uint32_t seq, size_t totalBytes);
void pushBackupNow();

// Worker -> device: true when a request is waiting to be served.
bool hasRequest();
// Pops the next request. Returns false when the queue is empty.
bool nextRequest(int32_t* id, char* method, size_t methodCap, char* path,
                 size_t pathCap, char* body, size_t bodyCap);
// Finishes a request: status, content type, optional cache-control.
void sendResponse(int32_t id, int status, const char* contentType, const char* cacheControl,
                  const uint8_t* body, size_t len);

// Streams a file from the filesystem as a relayed response.
void sendFile(int32_t id, const char* absPath, const char* contentType,
              const char* contentEncoding, const char* cacheControl);

uint32_t reconnectCount();

}  // namespace relay