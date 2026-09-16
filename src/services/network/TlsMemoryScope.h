#pragma once

// Arduino-ESP32's default mbedTLS allocator targets scarce internal RAM.
// A scoped handshake can use PSRAM for its certificate, RSA, and parser
// working set while retaining the ESP allocator as a safe fallback.
//
// The mbedTLS allocator hook is process-global. Nested or overlapping scopes
// are reference-counted so the default allocator is restored only after the
// final scoped handshake finishes.
class TlsMemoryScope {
public:
  explicit TlsMemoryScope(bool enabled = true);
  ~TlsMemoryScope();

  TlsMemoryScope(const TlsMemoryScope &) = delete;
  TlsMemoryScope &operator=(const TlsMemoryScope &) = delete;

  bool active() const { return _active; }

private:
  bool _active;
};
