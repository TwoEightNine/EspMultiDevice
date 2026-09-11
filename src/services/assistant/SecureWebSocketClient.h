#pragma once

#include <Arduino.h>
#include <NetworkClientSecure.h>

// Deliberately small RFC 6455 client for assistant traffic. It owns a TLS
// session but never allows TLS without an explicit caller-supplied trust store.
class SecureWebSocketClient {
public:
  static constexpr size_t MaxHeaders = 8;
  static constexpr size_t MaxHeaderBytes = 1024;
  static constexpr size_t MaxPayloadBytes = 2048;
  static constexpr size_t EventQueueCapacity = 4;

  enum class TrustKind : uint8_t { PemCertificate, CertificateBundle };

  struct Endpoint { const char *host; uint16_t port; const char *path; const char *subprotocol; };
  struct Header { const char *name; const char *value; };
  struct TrustStore {
    // The pointed-to trust bytes must remain valid until close(). PEM input
    // includes its terminating null in length; bundle input uses its byte size.
    TrustKind kind; const uint8_t *data; size_t length;
  };
  enum class EventType : uint8_t { Connected, Disconnected, Text, Binary, Ping, Pong, Error, Overflow };
  struct Event { EventType type; size_t length; uint8_t payload[MaxPayloadBytes + 1]; };

  SecureWebSocketClient();
  ~SecureWebSocketClient();
  SecureWebSocketClient(const SecureWebSocketClient &) = delete;
  SecureWebSocketClient &operator=(const SecureWebSocketClient &) = delete;

  bool connect(const Endpoint &endpoint, const Header *headers, size_t headerCount,
               const TrustStore &trust);
  bool poll(Event &event);
  bool sendText(const char *text, size_t length);
  bool sendBinary(const uint8_t *data, size_t length);
  void close();
  bool isActive() const;
  bool isConnected() const;
  const String &lastError() const;

private:
  enum class ReceiveState : uint8_t { Header, ExtendedLength, Payload };

  NetworkClientSecure _tls;
  Event _events[EventQueueCapacity];
  size_t _eventHead, _eventCount;
  bool _active, _connected, _overflowPending;
  char _extraHeaders[MaxHeaderBytes + 1];
  uint8_t _rxPayload[MaxPayloadBytes + 1];
  uint8_t _txChunk[256];
  ReceiveState _receiveState;
  uint8_t _rxFirst, _rxLengthBytes[2];
  size_t _rxLengthUsed, _rxLength, _rxUsed;
  String _lastError;

  bool buildHeaders(const Header *headers, size_t count);
  bool performHandshake(const Endpoint &endpoint);
  bool buildWebSocketKey(char *key, size_t keySize, char *accept, size_t acceptSize);
  bool writeAll(const uint8_t *data, size_t length, uint32_t timeoutMs);
  bool sendFrame(uint8_t opcode, const uint8_t *data, size_t length);
  bool pumpIncoming();
  bool consumeByte(uint8_t byte);
  void completeFrame();
  void resetReceiveState();
  void failConnection(const __FlashStringHelper *message);
  bool dequeue(Event &event);
  void enqueue(EventType type, const uint8_t *payload, size_t length);
  void setError(const __FlashStringHelper *message);
  static bool validHeaderName(const char *name);
  static bool validHeaderValue(const char *value);
  static bool validEndpointValue(const char *value);
};
