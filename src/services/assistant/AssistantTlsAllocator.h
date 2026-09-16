#pragma once

// Arduino-ESP32's default mbedTLS allocator targets internal RAM. Assistant
// handshakes are screen-scoped and can keep their certificate/parser working
// set in PSRAM, preserving internal RAM for the mandatory network task stack.
class AssistantTlsAllocatorScope {
public:
  AssistantTlsAllocatorScope();
  ~AssistantTlsAllocatorScope();

  AssistantTlsAllocatorScope(const AssistantTlsAllocatorScope &) = delete;
  AssistantTlsAllocatorScope &operator=(const AssistantTlsAllocatorScope &) = delete;

  bool active() const { return _active; }

private:
  bool _active;
};
