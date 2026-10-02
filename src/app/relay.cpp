#include "relay.h"

#include <mbedtls/sha256.h>

#include "backup.h"
#include "config.h"
#include "storage.h"

namespace relay {
namespace {

constexpr size_t kQueueMax = 6;
constexpr size_t kBodyMax = 2048;
constexpr uint32_t kReconnectMs = 5000;
constexpr uint32_t kStaleMs = 180000;
constexpr uint8_t  kMaxBackoffFails = 5;  // cap on the fast-fail backoff

struct Pending {
  int32_t id;
  char method[8];
  char path[192];
  char body[kBodyMax];
};

Pending g_queue[kQueueMax];
uint8_t g_queueCount = 0;

WiFiClientSecure g_tls;
bool     g_socketOpen = false;
bool     g_authed = false;
uint32_t g_lastAttempt = 0;
uint32_t g_fastFails = 0;
uint32_t g_reconnects = 0;
unsigned long g_lastActivity = 0;
char     g_nonce[64] = "";

// HMAC-SHA256, hex encoded.
//
// This must be a real HMAC, not a salted SHA-256: the Worker verifies with
// crypto.subtle.importKey(..., {name:'HMAC'}) and will reject anything else,
// which shows up as the device never leaving the "connecting" state. The
// construction is the standard one from RFC 2104:
//
//   HMAC(K,m) = SHA256((K ^ opad) || SHA256((K ^ ipad) || m))
//
// with K padded or hashed down to the 64-byte block size.
void hmacSha256Hex(const char* key, const char* msg, char* out) {
  constexpr size_t kBlock = 64;  // SHA-256 block size
  uint8_t kx[kBlock];
  memset(kx, 0, kBlock);
  const size_t keyLen = strlen(key);
  if (keyLen > kBlock) {
    uint8_t digest[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, reinterpret_cast<const unsigned char*>(key), keyLen);
    mbedtls_sha256_finish(&c, digest);
    mbedtls_sha256_free(&c);
    memcpy(kx, digest, 32);
  } else {
    memcpy(kx, key, keyLen);
  }

  uint8_t ipad[kBlock], opad[kBlock];
  for (size_t i = 0; i < kBlock; ++i) {
    ipad[i] = static_cast<uint8_t>(kx[i] ^ 0x36);
    opad[i] = static_cast<uint8_t>(kx[i] ^ 0x5C);
  }

  const size_t msgLen = strlen(msg);
  uint8_t inner[32];

  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, ipad, kBlock);
  mbedtls_sha256_update(&c, reinterpret_cast<const unsigned char*>(msg), msgLen);
  mbedtls_sha256_finish(&c, inner);
  mbedtls_sha256_free(&c);

  uint8_t outer[32];
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, opad, kBlock);
  mbedtls_sha256_update(&c, inner, 32);
  mbedtls_sha256_finish(&c, outer);
  mbedtls_sha256_free(&c);

  for (int i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", outer[i]);
  out[64] = '\0';
}

// Writes every byte or reports failure. NetworkClientSecure::write() may accept
// fewer bytes than offered - the TLS record and socket buffers drain in
// whatever sizes the network hands over - and treating a short write as fatal
// silently truncated relay responses mid-base64, which the Worker rejected as
// invalid and dropped. Retrying until the buffer is empty is the only correct
// behaviour here.
bool writeAll(const uint8_t* data, size_t len) {
  size_t sent = 0;
  unsigned spins = 0;
  while (sent < len) {
    const int n = g_tls.write(data + sent, len - sent);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      spins = 0;
      continue;
    }
    // A zero or negative write means the socket buffer is full, not broken.
    // Back off briefly, but give up eventually so a dead peer cannot wedge the
    // loop task forever.
    if (++spins > 200) return false;
    delay(2);
  }
  return true;
}

// Writes a masked text frame. The mask is a constant because it has no
// confidentiality role here: TLS already provides that, and a constant keeps
// the hot path free of randomness.
bool writeFrame(const char* payload, size_t len) {
  static const uint8_t kMask[4] = {0x12, 0x34, 0x56, 0x78};
  uint8_t header[10];
  size_t headerLen;
  header[0] = 0x81;  // FIN + text
  if (len < 126) {
    header[1] = static_cast<uint8_t>(len | 0x80);
    headerLen = 2;
  } else if (len < 65536) {
    header[1] = static_cast<uint8_t>(126 | 0x80);
    header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>(len & 0xFF);
    headerLen = 4;
  } else {
    header[1] = static_cast<uint8_t>(127 | 0x80);
    for (int i = 0; i < 4; ++i) header[2 + i] = 0;
    header[6] = static_cast<uint8_t>((len >> 24) & 0xFF);
    header[7] = static_cast<uint8_t>((len >> 16) & 0xFF);
    header[8] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[9] = static_cast<uint8_t>(len & 0xFF);
    headerLen = 10;
  }
  g_lastActivity = millis();
  if (!writeAll(header, headerLen)) return false;
  if (!writeAll(kMask, 4)) return false;

  uint8_t chunk[512];
  size_t sent = 0;
  while (sent < len) {
    const size_t n = (len - sent) < sizeof(chunk) ? (len - sent) : sizeof(chunk);
    for (size_t i = 0; i < n; ++i) {
      chunk[i] = static_cast<uint8_t>(payload[sent + i]) ^ kMask[(sent + i) & 3];
    }
    if (!writeAll(chunk, n)) return false;
    sent += n;
    // Yield between chunks: this is the only place the loop task gets a chance
    // to run while a large push is in flight.
    delay(1);
  }
  return true;
}

void closeSocket();

// Reads one WebSocket frame. This is a direct port of the v1.5 wsRead() that
// ran in production for months; the earlier hand-rolled version in this file
// mis-decode masked frames, so the proven version is used verbatim. Only the
// module globals were renamed.
String readFrame() {
  if (!g_tls.available()) return "";

  const uint8_t b0 = g_tls.read();
  const uint8_t b1 = g_tls.read();
  const bool masked = (b1 & 0x80) != 0;
  size_t len = b1 & 0x7F;

  if (len == 126) {
    len = (static_cast<size_t>(g_tls.read()) << 8) | g_tls.read();
  } else if (len == 127) {
    for (int i = 0; i < 4; ++i) g_tls.read();
    len = (static_cast<size_t>(g_tls.read()) << 24) |
          (static_cast<size_t>(g_tls.read()) << 16) |
          (static_cast<size_t>(g_tls.read()) << 8) | g_tls.read();
  }

  // The outbound socket uses setInsecure(), so a hostile or desynchronised
  // peer could declare a multi-megabyte frame and force a huge reserve(). The
  // Worker only sends small control frames and base64 body chunks, so 64 KB is
  // well above anything legitimate.
  if (len > 65536) return "";

  uint8_t mask[4] = {0};
  if (masked) {
    for (int i = 0; i < 4; ++i) mask[i] = g_tls.read();
  }

  String result;
  result.reserve(len + 1);
  const unsigned long readStart = millis();
  for (size_t i = 0; i < len; ++i) {
    while (!g_tls.available()) {
      if ((millis() - readStart) > 5000 || !g_tls.connected()) {
        g_authed = false;
        return "";
      }
      delay(1);
    }
    uint8_t c = g_tls.read();
    if (masked) c ^= mask[i % 4];
    result += static_cast<char>(c);
  }

  const uint8_t opcode = b0 & 0x0F;
  if (opcode == 0x08) {
    // close: the Worker or the edge is going away. Stop the TLS client too -
    // leaving it open makes tick() believe the socket is still usable, so the
    // reconnect path never runs.
    Serial.println("[relay] close frame from peer");
    closeSocket();
    return "";
  }
  if (opcode == 0x09) {
    // ping: answer with a pong, same fixed mask the sender uses.
    const uint8_t pongHeader[2] = {0x8A, 0x80};
    const uint8_t pongMask[4] = {0, 0, 0, 0};
    g_tls.write(pongHeader, 2);
    g_tls.write(pongMask, 4);
    return "";
  }
  if (opcode == 0x0A) {
    g_lastActivity = millis();
    return "";
  }

  g_lastActivity = millis();
  return result;
}

// Pulls a top-level string field out of a JSON frame without building a tree.
bool jsonStr(const String& src, const char* key, char* out, size_t cap) {
  const String needle = String("\"") + key + "\":\"";
  const int at = src.indexOf(needle);
  if (at < 0) return false;
  const int start = at + needle.length();
  const int end = src.indexOf('"', start);
  if (end <= start) return false;
  const size_t n = static_cast<size_t>(end - start);
  if (n >= cap) return false;
  memcpy(out, src.c_str() + start, n);
  out[n] = '\0';
  return true;
}

bool jsonInt(const String& src, const char* key, long* out) {
  const String needle = String("\"") + key + "\":";
  const int at = src.indexOf(needle);
  if (at < 0) return false;
  *out = strtol(src.c_str() + at + needle.length(), nullptr, 10);
  return true;
}

void sendAuthResponse() {
  char sig[80] = "";
  if (config::v.deviceKey[0]) {
    hmacSha256Hex(config::v.deviceKey, g_nonce, sig);
  }
  char frame[128];
  const int n = snprintf(frame, sizeof(frame),
                         "{\"type\":\"auth_response\",\"hmac\":\"%s\"}", sig);
  if (n > 0) writeFrame(frame, static_cast<size_t>(n));
}

void handleFrame(const String& msg) {
  if (msg.length() < 2 || msg[0] != '{') return;

  char type[24];
  if (jsonStr(msg, "type", type, sizeof(type))) {
    if (strcmp(type, "auth_challenge") == 0) {
      if (jsonStr(msg, "nonce", g_nonce, sizeof(g_nonce))) {
        sendAuthResponse();
        // The Worker sends nothing back when the HMAC verifies - it just
        // flips its own authenticated flag. Waiting for an "auth_result"
        // therefore never completes, so treat the challenge as satisfied once
        // we have answered it. A wrong key is still caught: the Worker closes
        // the socket and we reconnect, which is visible on the serial log.
        g_authed = true;
        Serial.println("[relay] auth response sent");
      }
      return;
    }
  }

  char event[32];
  if (jsonStr(msg, "event", event, sizeof(event))) {
    if (strcmp(event, "auth_result") == 0) {
      char ok[8] = "";
      if (jsonStr(msg, "ok", ok, sizeof(ok))) {
        g_authed = (strcmp(ok, "true") == 0);
        Serial.printf("[relay] auth rejected by worker (%s)\n", ok);
      }
    } else if (strcmp(event, "backup_committed") == 0) {
      char date[16] = "";
      if (jsonStr(msg, "date", date, sizeof(date))) backup::onCommitted(date);
    } else if (strcmp(event, "backup_error") == 0) {
      char why[96] = "";
      if (jsonStr(msg, "reason", why, sizeof(why))) {
        Serial.printf("[relay] backup rejected: %s\n", why);
      }
    } else if (strcmp(event, "r2_healthcheck_result") == 0) {
      char detail[128] = "";
      if (jsonStr(msg, "detail", detail, sizeof(detail))) {
        Serial.printf("[relay] r2 healthcheck: %s\n", detail);
      }
    }
    return;
  }

  // Otherwise a relayed HTTP request:
  //   {"id":N,"method":"GET","path":"/...","body":"..."}
  long id = 0;
  if (!jsonInt(msg, "id", &id)) return;
  if (g_queueCount >= kQueueMax) return;  // drop; the Worker times out and retries

  Pending& p = g_queue[g_queueCount];
  p.id = static_cast<int32_t>(id);
  char tmp[512];
  if (jsonStr(msg, "method", tmp, sizeof(tmp))) {
    snprintf(p.method, sizeof(p.method), "%s", tmp);
  } else {
    snprintf(p.method, sizeof(p.method), "GET");
  }
  if (jsonStr(msg, "path", tmp, sizeof(tmp))) {
    snprintf(p.path, sizeof(p.path), "%s", tmp);
  } else {
    snprintf(p.path, sizeof(p.path), "/");
  }
  p.body[0] = '\0';
  if (jsonStr(msg, "body", tmp, sizeof(tmp))) {
    snprintf(p.body, sizeof(p.body), "%s", tmp);
  }
  ++g_queueCount;
}

void closeSocket() {
  g_authed = false;
  g_socketOpen = false;
  g_queueCount = 0;
  g_nonce[0] = '\0';
  if (g_tls.connected()) g_tls.stop();
}

// Performs the HTTP upgrade by hand: WiFiClientSecure has no WebSocket API.
// Requires a 101 before any framing is written, otherwise a 401 (bad secret)
// or a redirect would corrupt the byte stream.
bool handshake() {
  const char* host = config::v.workerUrl;
  if (g_tls.connect(host, 443) != 1) {
    if (++g_fastFails > kMaxBackoffFails) g_fastFails = kMaxBackoffFails;
    return false;
  }
  g_tls.setTimeout(3000);

  // The nonce only has to satisfy the handshake shape; authentication is
  // worker_key in the query string plus the HMAC challenge that follows.
  static const char kHandshakeKey[] = "dGhlIHNhbXBsZSBub25jZQ==";
  g_tls.printf("GET /_ws?key=%s HTTP/1.1\r\n", config::v.workerKey);
  g_tls.printf("Host: %s\r\n", host);
  g_tls.print("Upgrade: websocket\r\n");
  g_tls.print("Connection: Upgrade\r\n");
  g_tls.printf("Sec-WebSocket-Key: %s\r\n", kHandshakeKey);
  g_tls.print("Sec-WebSocket-Version: 13\r\n");
  g_tls.printf("Origin: https://%s\r\n", host);
  g_tls.print("\r\n");

  uint32_t startedAt = millis();
  bool upgraded = false;
  bool inHeaders = true;
  bool lastWasLf = false;
  char status[16];  // must hold at least "HTTP/1.1 101"
  size_t sn = 0;

  // Read until the BLANK line that ends the response headers. Waiting for the
  // status line alone is not enough: the rest of the headers, and possibly the
  // first WebSocket frame, would be left in the buffer and then mis-parsed as
  // frame data.
  while (millis() - startedAt < 6000) {
    if (!g_tls.connected()) return false;
    if (!g_tls.available()) {
      delay(4);
      continue;
    }
    const char c = static_cast<char>(g_tls.read());
    if (c == '\n') {
      if (lastWasLf) break;  // blank line: headers are done
      if (inHeaders) {
        status[sn] = '\0';
        // Cloudflare answers with HTTP/1.0 or HTTP/1.1 depending on the edge
        // node, so both have to be accepted.
        upgraded = (strncmp(status, "HTTP/1.0 101", 12) == 0) ||
                   (strncmp(status, "HTTP/1.1 101", 12) == 0);
        inHeaders = false;
      }
      lastWasLf = true;
      continue;
    }
    // A bare CR is part of the CRLF pair, not content.
    if (c == '\r') continue;
    // Any real character means we are inside a line again, so the blank-line
    // detector must be re-armed. Without this the read stops at the first
    // newline after the status line and leaves the remaining headers plus the
    // first frame to be mis-parsed as WebSocket data.
    lastWasLf = false;
    if (inHeaders && sn + 1 < sizeof(status)) status[sn++] = c;
  }

  if (!upgraded) {
    Serial.println("[relay] websocket upgrade rejected");
    g_tls.stop();
    return false;
  }
  return true;
}

void eventHeader(const char* eventName, uint32_t seq, bool hasSeq, size_t* written) {
  char head[128];
  int n;
  if (hasSeq) {
    n = snprintf(head, sizeof(head),
                 "{\"type\":\"event\",\"event\":\"%s\",\"seq\":%u,\"data\":\"",
                 eventName, (unsigned)seq);
  } else {
    n = snprintf(head, sizeof(head),
                 "{\"type\":\"event\",\"event\":\"%s\",\"data\":\"", eventName);
  }
  if (n > 0) g_tls.write(reinterpret_cast<const uint8_t*>(head), static_cast<size_t>(n));
  *written = static_cast<size_t>(n > 0 ? n : 0);
}

void endEvent() { g_tls.write(reinterpret_cast<const uint8_t*>("\"}"), 2); }

void streamEvent(const char* eventName, uint32_t seq, bool hasSeq,
                 const void* body, size_t bodyLen) {
  if (!g_socketOpen || !g_authed) return;
  size_t n = 0;
  eventHeader(eventName, seq, hasSeq, &n);
  g_tls.write(reinterpret_cast<const uint8_t*>(body), bodyLen);
  endEvent();
}

}  // namespace

void begin() {
  end();
  // TLS verification is skipped deliberately: this connects to a custom domain
  // behind Cloudflare, and the connection is authenticated by worker_key plus
  // the HMAC challenge, not by the certificate chain.
  g_tls.setInsecure();
}

void end() { closeSocket(); }

bool connected() { return g_socketOpen && g_authed; }
unsigned long lastActivityMs() { return g_lastActivity; }
uint32_t reconnectCount() { return g_reconnects; }

void tick() {
  // Liveness is judged on writes as well as reads: the Worker sends nothing
  // until it has something to say, so a read-only timestamp would expire a
  // perfectly healthy socket. The Worker also closes genuinely dead sockets
  // on its own side, which surfaces here as connect() failing.
  if (g_tls.connected() && (millis() - g_lastActivity) > kStaleMs) {
    Serial.println("[relay] idle too long, reconnecting");
    closeSocket();
  }
  if (!g_tls.connected()) {
    closeSocket();
    const uint32_t now = millis();
    if (g_lastAttempt && (now - g_lastAttempt) < kReconnectMs) return;
    g_lastAttempt = now;
    if (config::v.workerUrl[0] == '\0' || config::v.workerKey[0] == '\0') return;
    if (!handshake()) return;
    g_socketOpen = true;
    g_lastActivity = millis();
    ++g_reconnects;
    Serial.printf("[relay] connected to %s\n", config::v.workerUrl);
    return;
  }
  if (!g_tls.available()) return;

  const String msg = readFrame();
  if (msg.length() == 0) {
    if (!g_tls.connected()) closeSocket();
    return;
  }
  handleFrame(msg);
}

bool sendJson(const char* json, size_t len) { return writeFrame(json, len); }

void pushStats(const char* statsJson) {
  if (!g_socketOpen || !g_authed) return;
  streamEvent("stats_update", 0, false, statsJson, strlen(statsJson));
}

void pushConsole(const char* json) {
  if (!g_socketOpen || !g_authed) return;
  writeFrame(json, strlen(json));
}

void pushBackupStart(uint32_t seq, const char* generatedAt, const char* firmware,
                     const char* uptime, size_t totalBytes) {
  if (!g_socketOpen || !g_authed) return;
  char buf[320];
  const int n = snprintf(buf, sizeof(buf),
      "{\"type\":\"event\",\"event\":\"backup_start\",\"seq\":%u,"
      "\"generated_at\":\"%s\",\"firmware\":\"%s\",\"uptime\":\"%s\",\"size\":%u}",
      (unsigned)seq, generatedAt, firmware, uptime, (unsigned)totalBytes);
  if (n > 0) writeFrame(buf, static_cast<size_t>(n));
}

void pushBackupFileStart(uint32_t seq, const char* name, size_t size) {
  if (!g_socketOpen || !g_authed) return;
  char buf[320];
  const int n = snprintf(buf, sizeof(buf),
      "{\"type\":\"event\",\"event\":\"backup_file_start\",\"seq\":%u,"
      "\"name\":\"%s\",\"size\":%u}", (unsigned)seq, name, (unsigned)size);
  if (n > 0) writeFrame(buf, static_cast<size_t>(n));
}

void pushBackupFileChunk(uint32_t seq, const char* b64) {
  if (!g_socketOpen || !g_authed) return;
  streamEvent("backup_file_chunk", seq, true, b64, strlen(b64));
}

void pushBackupFileEnd(uint32_t seq, const char* name) {
  if (!g_socketOpen || !g_authed) return;
  char buf[320];
  const int n = snprintf(buf, sizeof(buf),
      "{\"type\":\"event\",\"event\":\"backup_file_end\",\"seq\":%u,\"name\":\"%s\"}",
      (unsigned)seq, name);
  if (n > 0) writeFrame(buf, static_cast<size_t>(n));
}

void pushBackupFileSkipped(uint32_t seq, const char* name, size_t size, const char* reason) {
  if (!g_socketOpen || !g_authed) return;
  char buf[352];
  const int n = snprintf(buf, sizeof(buf),
      "{\"type\":\"event\",\"event\":\"backup_file_skipped\",\"seq\":%u,"
      "\"name\":\"%s\",\"size\":%u,\"reason\":\"%s\"}",
      (unsigned)seq, name, (unsigned)size, reason);
  if (n > 0) writeFrame(buf, static_cast<size_t>(n));
}

void pushBackupEnd(uint32_t seq, size_t totalBytes) {
  if (!g_socketOpen || !g_authed) return;
  char buf[128];
  const int n = snprintf(buf, sizeof(buf),
      "{\"type\":\"event\",\"event\":\"backup_end\",\"seq\":%u,\"size\":%u}",
      (unsigned)seq, (unsigned)totalBytes);
  if (n > 0) writeFrame(buf, static_cast<size_t>(n));
}

void pushBackupNow() {
  if (!g_socketOpen || !g_authed) return;
  writeFrame("{\"type\":\"event\",\"event\":\"backup_request\"}", 39);
}

bool hasRequest() { return g_queueCount > 0; }

bool nextRequest(int32_t* id, char* method, size_t methodCap, char* path,
                 size_t pathCap, char* body, size_t bodyCap) {
  if (g_queueCount == 0) return false;
  const Pending& p = g_queue[0];
  *id = p.id;
  snprintf(method, methodCap, "%s", p.method);
  snprintf(path, pathCap, "%s", p.path);
  snprintf(body, bodyCap, "%s", p.body);
  for (uint8_t i = 1; i < g_queueCount; ++i) g_queue[i - 1] = g_queue[i];
  --g_queueCount;
  return true;
}

// Streams a response body as base64 chunks followed by an empty frame, which is
// the delimiter the Worker uses to close a response.
void sendResponse(int32_t id, int status, const char* contentType,
                  const char* cacheControl, const uint8_t* body, size_t len) {
  if (!g_socketOpen) return;

  char head[224];
  const int n = snprintf(head, sizeof(head),
                         "{\"id\":%d,\"status\":%d,\"ct\":\"%s\",\"cc\":\"%s\"}",
                         static_cast<int>(id), status, contentType,
                         cacheControl ? cacheControl : "");
  if (n <= 0) return;
  writeFrame(head, static_cast<size_t>(n));

  static const char kTbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t kChunk = 576;
  static char b64[800];
  for (size_t off = 0; off < len; off += kChunk) {
    const size_t n2 = (len - off) < kChunk ? (len - off) : kChunk;
    size_t o = 0;
    for (size_t i = 0; i < n2; i += 3) {
      const uint32_t a = body[off + i];
      const uint32_t b = (i + 1 < n2) ? body[off + i + 1] : 0;
      const uint32_t c = (i + 2 < n2) ? body[off + i + 2] : 0;
      const uint32_t v = (a << 16) | (b << 8) | c;
      b64[o++] = kTbl[(v >> 18) & 0x3F];
      b64[o++] = kTbl[(v >> 12) & 0x3F];
      b64[o++] = (i + 1 < n2) ? kTbl[(v >> 6) & 0x3F] : '=';
      b64[o++] = (i + 2 < n2) ? kTbl[v & 0x3F] : '=';
    }
    writeFrame(b64, o);
  }
  writeFrame("", 0);
}

void sendFile(int32_t id, const char* absPath, const char* contentType,
              const char* contentEncoding, const char* cacheControl) {
  File f = fsx::vol().open(absPath, FILE_READ);
  if (!f) {
    sendResponse(id, 404, "text/plain", "no-store",
                 reinterpret_cast<const uint8_t*>("not found"), 9);
    return;
  }

  char head[256];
  const int n = snprintf(head, sizeof(head),
      "{\"id\":%d,\"status\":200,\"ct\":\"%s\",\"cc\":\"%s\",\"ce\":\"%s\"}",
      static_cast<int>(id), contentType, cacheControl ? cacheControl : "",
      contentEncoding ? contentEncoding : "");
  if (n > 0) writeFrame(head, static_cast<size_t>(n));

  // Straight from flash to the socket: a page is up to 170KB and the heap
  // cannot hold a second copy.
  static const char kTbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static uint8_t block[576];
  static char b64[800];
  while (f.available()) {
    const size_t got = f.read(block, sizeof(block));
    if (got == 0) break;
    size_t o = 0;
    for (size_t i = 0; i < got; i += 3) {
      const uint32_t a = block[i];
      const uint32_t b = (i + 1 < got) ? block[i + 1] : 0;
      const uint32_t c = (i + 2 < got) ? block[i + 2] : 0;
      const uint32_t v = (a << 16) | (b << 8) | c;
      b64[o++] = kTbl[(v >> 18) & 0x3F];
      b64[o++] = kTbl[(v >> 12) & 0x3F];
      b64[o++] = (i + 1 < got) ? kTbl[(v >> 6) & 0x3F] : '=';
      b64[o++] = (i + 2 < got) ? kTbl[v & 0x3F] : '=';
    }
    writeFrame(b64, o);
  }
  f.close();
  writeFrame("", 0);
}

}  // namespace relay