#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class WiFiController;

enum class WiFiSpeedTestPhase : uint8_t {
  Idle,
  Latency,
  Download,
  Upload,
  Complete,
  Cancelled,
  Error
};

struct WiFiSpeedTestSnapshot {
  WiFiSpeedTestPhase phase = WiFiSpeedTestPhase::Idle;
  uint32_t revision = 0;
  uint8_t progress = 0;
  float latencyMs = -1.0f;
  float downloadMbps = -1.0f;
  float uploadMbps = -1.0f;
  char status[80] = {};
};

// Screen-owned, one-shot network worker. The worker owns the TLS client and
// transfer buffer while active and communicates with the UI only through a
// copied snapshot.
class WiFiSpeedTestWorker {
public:
  explicit WiFiSpeedTestWorker(WiFiController &wifi);

  bool begin();
  bool start();
  void cancel();
  bool end();
  bool active() const { return _active; }
  WiFiSpeedTestSnapshot snapshot() const;

private:
  struct HttpResponse {
    int statusCode = 0;
    int32_t contentLength = -1;
    bool chunked = false;
    bool connectionClose = false;
    uint32_t firstByteMs = 0;
  };

  static constexpr uint16_t PreferredBufferBytes = 8192;
  static constexpr uint16_t FallbackBufferBytes = 1024;
  static constexpr uint32_t ConnectTimeoutMs = 10000;
  static constexpr uint32_t NoProgressTimeoutMs = 5000;
  static constexpr uint32_t RequestTimeoutMs = 30000;
  static constexpr uint32_t WholeTestTimeoutMs = 75000;
  static constexpr uint32_t SlowStageMs = 3000;
  static constexpr uint32_t DirectionTargetMs = 6000;
  static constexpr uint16_t MaxHeaderLineBytes = 384;
  static constexpr uint8_t MaxHeaderLines = 32;

  WiFiController &_wifi;
  mutable SemaphoreHandle_t _snapshotMutex;
  TaskHandle_t _task;
  WiFiClientSecure _client;
  uint8_t *_buffer;
  size_t _bufferBytes;
  volatile uint32_t _generation;
  volatile uint32_t _taskGeneration;
  volatile bool _active;
  WiFiSpeedTestSnapshot _snapshot;
  uint32_t _testStartedMs;
  uint32_t _lastProgressPublishMs;

  static void taskMain(void *context);
  void run(uint32_t generation);
  bool current(uint32_t generation) const;
  bool timedOut() const;
  void publish(WiFiSpeedTestPhase phase, uint8_t progress, const char *status,
               float latencyMs = -1.0f, float downloadMbps = -1.0f,
               float uploadMbps = -1.0f, bool throttle = false);
  void publishError(const char *message);

  bool ensureConnected(uint32_t generation, const char *&error);
  bool writeAll(uint32_t generation, const uint8_t *data, size_t bytes,
                uint32_t requestStartedMs, const char *&error);
  bool readLine(uint32_t generation, String &line, uint32_t requestStartedMs,
                uint32_t *firstByteMs, const char *&error);
  bool readResponseHeaders(uint32_t generation, HttpResponse &response,
                           uint32_t requestStartedMs, const char *&error);
  bool readResponseBody(uint32_t generation, const HttpResponse &response,
                        uint32_t expectedBytes, uint32_t requestStartedMs,
                        uint32_t &payloadStartedMs, uint32_t &payloadFinishedMs,
                        uint32_t &payloadBytes, WiFiSpeedTestPhase phase,
                        uint32_t completedBefore, uint32_t directionMaximum,
                        const char *&error);
  bool readFixedBytes(uint32_t generation, uint32_t bytes, uint32_t requestStartedMs,
                      uint32_t &payloadStartedMs, uint32_t &payloadFinishedMs,
                      uint32_t &payloadBytes, WiFiSpeedTestPhase phase,
                      uint32_t completedBefore, uint32_t directionMaximum,
                      const char *&error);
  bool readChunkedBytes(uint32_t generation, uint32_t expectedBytes,
                        uint32_t requestStartedMs, uint32_t &payloadStartedMs,
                        uint32_t &payloadFinishedMs, uint32_t &payloadBytes,
                        WiFiSpeedTestPhase phase, uint32_t completedBefore,
                        uint32_t directionMaximum, const char *&error);
  bool performGet(uint32_t generation, uint32_t bytes, uint32_t &firstByteMs,
                  uint32_t &payloadDurationMs, uint32_t &payloadBytes,
                  WiFiSpeedTestPhase phase, uint32_t completedBefore,
                  uint32_t directionMaximum, const char *&error);
  bool performUpload(uint32_t generation, uint32_t bytes,
                     uint32_t &payloadDurationMs, uint32_t &payloadBytes,
                     uint32_t completedBefore, uint32_t directionMaximum,
                     const char *&error);
  bool sendRequestHeader(uint32_t generation, const String &header,
                         uint32_t requestStartedMs, const char *&error);
  bool consumeCrlf(uint32_t generation, uint32_t requestStartedMs,
                   const char *&error);
  void updateTransferProgress(uint32_t generation, WiFiSpeedTestPhase phase,
                              uint32_t completed, uint32_t directionMaximum,
                              const char *status);

  static bool statusSucceeded(int statusCode);
  static bool equalsIgnoreCasePrefix(const String &line, const char *prefix);
};
