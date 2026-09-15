#include "SecureWebSocketClient.h"

#include <esp_system.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform.h>
#include <mbedtls/sha1.h>

#include <strings.h>
#include <string.h>

// Arduino-ESP32 3.3.8's esp_mem.h declares these ESP-IDF C symbols with C++
// linkage. Use distinct local names pinned to the unmangled runtime symbols.
extern void *assistantDefaultTlsCalloc(size_t count, size_t size) asm("esp_mbedtls_mem_calloc");
extern void assistantDefaultTlsFree(void *pointer) asm("esp_mbedtls_mem_free");

namespace {
constexpr uint32_t IoTimeoutMs = 5000;
constexpr uint32_t ConnectTimeoutMs = 15000;
constexpr size_t MaxHandshakeBytes = 4096;
constexpr char WebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// ESP-IDF exposes mbedTLS's allocator hook. The TLS contexts and certificate
// parser do not require DMA-capable RAM, so put their short-lived handshake
// allocations in the available OPI PSRAM instead of fragmenting internal heap.
void *assistantTlsCalloc(size_t count, size_t size) {
  if (psramFound()) {
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  return assistantDefaultTlsCalloc(count, size);
}

bool responseHeaderEquals(const char *response, const char *name, const char *expected) {
  const size_t nameLength = strlen(name);
  const char *line = response;
  while (line && *line) {
    const char *end = strstr(line, "\r\n");
    const size_t lineLength = end ? static_cast<size_t>(end - line) : strlen(line);
    if (lineLength > nameLength + 1 && strncasecmp(line, name, nameLength) == 0 && line[nameLength] == ':') {
      const char *value = line + nameLength + 1;
      while (*value == ' ' || *value == '\t') ++value;
      const size_t valueLength = lineLength - static_cast<size_t>(value - line);
      return strlen(expected) == valueLength && strncmp(value, expected, valueLength) == 0;
    }
    line = end ? end + 2 : nullptr;
  }
  return false;
}
} // namespace

SecureWebSocketClient::SecureWebSocketClient()
    : _eventHead(0), _eventCount(0), _active(false), _connected(false), _overflowPending(false),
      _extraHeaders{}, _rxPayload{}, _txChunk{}, _receiveState(ReceiveState::Header), _rxFirst(0),
      _rxLengthBytes{}, _rxLengthUsed(0), _rxLength(0), _rxUsed(0), _lastError(F("Not connected")) {}

SecureWebSocketClient::~SecureWebSocketClient() { close(); }

bool SecureWebSocketClient::connect(const Endpoint &endpoint, const Header *headers, size_t headerCount,
                                    const TrustStore &trust) {
  close();
  if (!endpoint.host || !endpoint.host[0] || endpoint.port == 0 || !endpoint.path || endpoint.path[0] != '/' ||
      !validEndpointValue(endpoint.host) || !validEndpointValue(endpoint.path) ||
      (endpoint.subprotocol && !validEndpointValue(endpoint.subprotocol))) {
    setError(F("Invalid WSS endpoint"));
    return false;
  }
  if (!trust.data || trust.length == 0) {
    setError(F("WSS trust store is required"));
    return false;
  }
  if (trust.kind == TrustKind::PemCertificate && trust.data[trust.length - 1] != '\0') {
    setError(F("PEM trust store must be null terminated"));
    return false;
  }
  if (!buildHeaders(headers, headerCount)) {
    memset(_extraHeaders, 0, sizeof(_extraHeaders));
    return false;
  }

  if (trust.kind == TrustKind::CertificateBundle) _tls.setCACertBundle(trust.data, trust.length);
  else _tls.setCACert(reinterpret_cast<const char *>(trust.data));
  _tls.setHandshakeTimeout(ConnectTimeoutMs / 1000U);
  _tls.setTimeout(1000);
  const bool psramTlsAllocator = psramFound() &&
    mbedtls_platform_set_calloc_free(assistantTlsCalloc, assistantDefaultTlsFree) == 0;
  const bool tlsConnected = _tls.connect(endpoint.host, endpoint.port,
                                         static_cast<int32_t>(ConnectTimeoutMs));
  if (psramTlsAllocator) {
    mbedtls_platform_set_calloc_free(assistantDefaultTlsCalloc, assistantDefaultTlsFree);
  }
  if (!tlsConnected) {
    memset(_extraHeaders, 0, sizeof(_extraHeaders));
    setError(F("TLS WSS connection failed"));
    _tls.stop();
    return false;
  }
  _active = true;
  if (!performHandshake(endpoint)) {
    memset(_extraHeaders, 0, sizeof(_extraHeaders));
    _tls.stop();
    _active = false;
    return false;
  }
  memset(_extraHeaders, 0, sizeof(_extraHeaders));
  _connected = true;
  resetReceiveState();
  _lastError = F("OK");
  enqueue(EventType::Connected, nullptr, 0);
  return true;
}

bool SecureWebSocketClient::poll(Event &event) {
  if (dequeue(event)) return true;
  if (_overflowPending) {
    _overflowPending = false;
    event.type = EventType::Overflow;
    event.length = 0;
    event.payload[0] = 0;
    return true;
  }
  if (_active && !pumpIncoming()) return dequeue(event);
  return dequeue(event);
}

bool SecureWebSocketClient::sendText(const char *text, size_t length) {
  if (!text || length == 0 || length > MaxPayloadBytes) {
    setError(F("Invalid WebSocket text payload"));
    return false;
  }
  return sendFrame(0x1, reinterpret_cast<const uint8_t *>(text), length);
}

bool SecureWebSocketClient::sendBinary(const uint8_t *data, size_t length) {
  if (!data || length == 0 || length > MaxPayloadBytes) {
    setError(F("Invalid WebSocket binary payload"));
    return false;
  }
  return sendFrame(0x2, data, length);
}

void SecureWebSocketClient::close() {
  if (_connected) sendFrame(0x8, nullptr, 0);
  _tls.stop();
  _active = false;
  _connected = false;
  _eventHead = 0;
  _eventCount = 0;
  _overflowPending = false;
  memset(_extraHeaders, 0, sizeof(_extraHeaders));
  memset(_events, 0, sizeof(_events));
  memset(_rxPayload, 0, sizeof(_rxPayload));
  memset(_txChunk, 0, sizeof(_txChunk));
  memset(_rxLengthBytes, 0, sizeof(_rxLengthBytes));
  resetReceiveState();
}

bool SecureWebSocketClient::isActive() const { return _active; }
bool SecureWebSocketClient::isConnected() const { return _connected; }
const String &SecureWebSocketClient::lastError() const { return _lastError; }

bool SecureWebSocketClient::buildHeaders(const Header *headers, size_t count) {
  if (count > MaxHeaders || (count && !headers)) {
    setError(F("Too many WebSocket headers"));
    return false;
  }
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!validHeaderName(headers[i].name) || !validHeaderValue(headers[i].value)) {
      setError(F("Invalid WebSocket header"));
      return false;
    }
    const size_t nameLength = strlen(headers[i].name);
    const size_t valueLength = strlen(headers[i].value);
    if (used + nameLength + valueLength + 4 > MaxHeaderBytes) {
      setError(F("WebSocket headers exceed limit"));
      return false;
    }
    memcpy(_extraHeaders + used, headers[i].name, nameLength); used += nameLength;
    _extraHeaders[used++] = ':'; _extraHeaders[used++] = ' ';
    memcpy(_extraHeaders + used, headers[i].value, valueLength); used += valueLength;
    _extraHeaders[used++] = '\r'; _extraHeaders[used++] = '\n';
  }
  _extraHeaders[used] = 0;
  return true;
}

bool SecureWebSocketClient::performHandshake(const Endpoint &endpoint) {
  char key[25] = {};
  char expectedAccept[29] = {};
  char request[MaxHandshakeBytes] = {};
  char response[MaxHandshakeBytes + 1] = {};
  if (!buildWebSocketKey(key, sizeof(key), expectedAccept, sizeof(expectedAccept))) {
    setError(F("WebSocket key generation failed"));
    return false;
  }
  const char *subprotocol = endpoint.subprotocol ? endpoint.subprotocol : "";
  int written = snprintf(request, sizeof(request),
                         "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s%s%s\r\n",
                         endpoint.path, endpoint.host, endpoint.port, key,
                         subprotocol[0] ? "Sec-WebSocket-Protocol: " : "",
                         subprotocol[0] ? subprotocol : "", subprotocol[0] ? "\r\n" : "");
  const size_t extras = strlen(_extraHeaders);
  if (written < 0 || static_cast<size_t>(written) < 2 || static_cast<size_t>(written) + extras + 1 > sizeof(request)) {
    memset(request, 0, sizeof(request));
    setError(F("WebSocket handshake exceeds limit"));
    return false;
  }
  size_t requestLength = static_cast<size_t>(written) - 2;
  memmove(request + requestLength + extras, request + requestLength, 3);
  memcpy(request + requestLength, _extraHeaders, extras);
  requestLength += extras + 2;
  if (!writeAll(reinterpret_cast<const uint8_t *>(request), requestLength, IoTimeoutMs)) {
    memset(request, 0, sizeof(request));
    setError(F("WebSocket upgrade write failed"));
    return false;
  }
  memset(request, 0, sizeof(request));
  size_t used = 0;
  const uint32_t started = millis();
  while (millis() - started < IoTimeoutMs && used < MaxHandshakeBytes) {
    const int available = _tls.available();
    if (available <= 0) { delay(1); continue; }
    // Do not consume a WebSocket frame that the server coalesced directly
    // behind the HTTP response (the fixture sends `header-ok` that way).
    const int received = _tls.read(reinterpret_cast<uint8_t *>(response + used), 1);
    if (received <= 0) break;
    used += static_cast<size_t>(received);
    response[used] = 0;
    if (strstr(response, "\r\n\r\n")) break;
  }
  if (!strstr(response, "\r\n\r\n")) {
    setError(F("WebSocket upgrade response timeout"));
    return false;
  }
  if (strncmp(response, "HTTP/1.1 101 ", 13) != 0 || !responseHeaderEquals(response, "Upgrade", "websocket") ||
      !responseHeaderEquals(response, "Sec-WebSocket-Accept", expectedAccept)) {
    setError(F("WebSocket upgrade validation failed"));
    return false;
  }
  if (subprotocol[0] && !responseHeaderEquals(response, "Sec-WebSocket-Protocol", subprotocol)) {
    setError(F("WebSocket subprotocol rejected"));
    return false;
  }
  return true;
}

bool SecureWebSocketClient::buildWebSocketKey(char *key, size_t keySize, char *accept, size_t acceptSize) {
  uint8_t nonce[16];
  for (size_t i = 0; i < sizeof(nonce); i += sizeof(uint32_t)) {
    const uint32_t randomValue = esp_random();
    memcpy(nonce + i, &randomValue, min(sizeof(randomValue), sizeof(nonce) - i));
  }
  size_t encoded = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(key), keySize, &encoded, nonce, sizeof(nonce)) != 0 ||
      encoded + 1 > keySize) return false;
  key[encoded] = 0;
  char source[64] = {};
  const int sourceLength = snprintf(source, sizeof(source), "%s%s", key, WebSocketGuid);
  uint8_t digest[20];
  if (sourceLength < 0 || static_cast<size_t>(sourceLength) >= sizeof(source) ||
      mbedtls_sha1(reinterpret_cast<const unsigned char *>(source), sourceLength, digest) != 0 ||
      mbedtls_base64_encode(reinterpret_cast<unsigned char *>(accept), acceptSize, &encoded, digest, sizeof(digest)) != 0 ||
      encoded + 1 > acceptSize) return false;
  accept[encoded] = 0;
  return true;
}

bool SecureWebSocketClient::writeAll(const uint8_t *data, size_t length, uint32_t timeoutMs) {
  uint32_t lastProgress = millis();
  while (length && millis() - lastProgress < timeoutMs) {
    const size_t written = _tls.write(data, length);
    if (!written) { delay(1); continue; }
    data += written;
    length -= written;
    lastProgress = millis();
  }
  return length == 0;
}

bool SecureWebSocketClient::sendFrame(uint8_t opcode, const uint8_t *data, size_t length) {
  if (!_connected || !_active || length > MaxPayloadBytes || (length && !data)) {
    setError(F("WebSocket is not connected"));
    return false;
  }
  uint8_t header[8];
  size_t headerLength = 0;
  header[headerLength++] = static_cast<uint8_t>(0x80U | opcode);
  if (length < 126) header[headerLength++] = static_cast<uint8_t>(0x80U | length);
  else { header[headerLength++] = 0xfe; header[headerLength++] = static_cast<uint8_t>(length >> 8); header[headerLength++] = static_cast<uint8_t>(length); }
  const uint32_t randomMask = esp_random();
  uint8_t mask[4];
  memcpy(mask, &randomMask, sizeof(mask));
  memcpy(header + headerLength, mask, sizeof(mask));
  headerLength += sizeof(mask);
  if (!writeAll(header, headerLength, IoTimeoutMs)) {
    failConnection(F("WebSocket frame header write failed"));
    return false;
  }
  for (size_t offset = 0; offset < length;) {
    const size_t chunk = min(sizeof(_txChunk), length - offset);
    for (size_t i = 0; i < chunk; ++i) _txChunk[i] = data[offset + i] ^ mask[(offset + i) % 4];
    if (!writeAll(_txChunk, chunk, IoTimeoutMs)) {
      failConnection(F("WebSocket frame payload write failed"));
      return false;
    }
    offset += chunk;
  }
  return true;
}

bool SecureWebSocketClient::pumpIncoming() {
  while (_active) {
    const int available = _tls.available();
    if (available < 0) { failConnection(F("WebSocket TLS read failed")); return false; }
    if (!available) return true;
    uint8_t byte = 0;
    if (_tls.read(&byte, 1) != 1) { failConnection(F("WebSocket read failed")); return false; }
    if (!consumeByte(byte)) return false;
  }
  return false;
}

bool SecureWebSocketClient::consumeByte(uint8_t byte) {
  if (_receiveState == ReceiveState::Header) {
    if (_rxLengthUsed++ == 0) { _rxFirst = byte; return true; }
    const uint8_t marker = byte & 0x7fU;
    if (!(_rxFirst & 0x80U) || (_rxFirst & 0x70U) || (byte & 0x80U) || marker == 127) {
      failConnection(F("Unsupported WebSocket frame")); return false;
    }
    _rxLengthUsed = 0;
    if (marker == 126) { _receiveState = ReceiveState::ExtendedLength; return true; }
    _rxLength = marker; _rxUsed = 0; _receiveState = ReceiveState::Payload;
    if (!_rxLength) completeFrame();
    return true;
  }
  if (_receiveState == ReceiveState::ExtendedLength) {
    _rxLengthBytes[_rxLengthUsed++] = byte;
    if (_rxLengthUsed == sizeof(_rxLengthBytes)) {
      _rxLength = (static_cast<size_t>(_rxLengthBytes[0]) << 8) | _rxLengthBytes[1];
      _rxLengthUsed = 0;
      if (_rxLength > MaxPayloadBytes) { failConnection(F("WebSocket frame exceeds limit")); return false; }
      _rxUsed = 0; _receiveState = ReceiveState::Payload;
      if (!_rxLength) completeFrame();
    }
    return true;
  }
  _rxPayload[_rxUsed++] = byte;
  if (_rxUsed == _rxLength) completeFrame();
  return true;
}

void SecureWebSocketClient::completeFrame() {
  const uint8_t opcode = _rxFirst & 0x0fU;
  _rxPayload[_rxLength] = 0;
  if (opcode == 0x1) enqueue(EventType::Text, _rxPayload, _rxLength);
  else if (opcode == 0x2) enqueue(EventType::Binary, _rxPayload, _rxLength);
  else if (opcode == 0x9) { enqueue(EventType::Ping, _rxPayload, _rxLength); if (!sendFrame(0xA, _rxPayload, _rxLength)) return; }
  else if (opcode == 0xA) enqueue(EventType::Pong, _rxPayload, _rxLength);
  else if (opcode == 0x8) {
    if (_connected) sendFrame(0x8, _rxPayload, _rxLength);
    _tls.stop(); _active = false; _connected = false;
    enqueue(EventType::Disconnected, _rxPayload, _rxLength);
  } else { failConnection(F("Unsupported WebSocket opcode")); return; }
  resetReceiveState();
}

void SecureWebSocketClient::resetReceiveState() {
  _receiveState = ReceiveState::Header; _rxFirst = 0; _rxLengthUsed = 0; _rxLength = 0; _rxUsed = 0;
}

void SecureWebSocketClient::failConnection(const __FlashStringHelper *message) {
  setError(message);
  const bool wasActive = _active || _connected;
  _tls.stop(); _active = false; _connected = false; resetReceiveState();
  enqueue(EventType::Error, nullptr, 0);
  if (wasActive) enqueue(EventType::Disconnected, nullptr, 0);
}

bool SecureWebSocketClient::dequeue(Event &event) {
  if (!_eventCount) return false;
  event = _events[_eventHead]; _eventHead = (_eventHead + 1) % EventQueueCapacity; --_eventCount;
  return true;
}

void SecureWebSocketClient::enqueue(EventType type, const uint8_t *payload, size_t length) {
  if (length > MaxPayloadBytes || _eventCount == EventQueueCapacity) { _overflowPending = true; return; }
  Event &event = _events[(_eventHead + _eventCount) % EventQueueCapacity];
  event.type = type; event.length = length;
  if (payload && length) memcpy(event.payload, payload, length);
  event.payload[length] = 0;
  ++_eventCount;
}

void SecureWebSocketClient::setError(const __FlashStringHelper *message) { _lastError = message; }

bool SecureWebSocketClient::validHeaderName(const char *name) {
  if (!name || !name[0]) return false;
  for (const char *p = name; *p; ++p) {
    const char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}
bool SecureWebSocketClient::validHeaderValue(const char *value) { return validEndpointValue(value); }
bool SecureWebSocketClient::validEndpointValue(const char *value) {
  if (!value) return false;
  for (const char *p = value; *p; ++p) if (static_cast<unsigned char>(*p) < 0x20U || *p == 0x7f) return false;
  return true;
}
