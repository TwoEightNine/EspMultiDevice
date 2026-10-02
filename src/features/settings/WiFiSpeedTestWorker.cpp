#include "WiFiSpeedTestWorker.h"

#include <WiFi.h>
#include <esp32-hal-psram.h>
#include <stdlib.h>

#include "../../services/network/TlsMemoryScope.h"
#include "../../services/network/WiFiController.h"

namespace {
constexpr char Host[] = "speed.cloudflare.com";
constexpr uint16_t Port = 443;
constexpr uint32_t StageBytes[] = {256UL * 1024UL, 1024UL * 1024UL, 4UL * 1024UL * 1024UL};
constexpr uint32_t DirectionMaximumBytes = StageBytes[0] + StageBytes[1] + StageBytes[2];
constexpr uint8_t LatencyProbeCount = 5;

class StateLock {
public:
  explicit StateLock(SemaphoreHandle_t mutex)
      : _mutex(mutex), _locked(mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}
  ~StateLock() { if (_locked) xSemaphoreGive(_mutex); }
  bool locked() const { return _locked; }

private:
  SemaphoreHandle_t _mutex;
  bool _locked;
};

void copyStatus(char *target, size_t capacity, const char *value) {
  if (!target || capacity == 0) return;
  if (!value) value = "";
  strlcpy(target, value, capacity);
}
}  // namespace

WiFiSpeedTestWorker::WiFiSpeedTestWorker(WiFiController &wifi)
    : _wifi(wifi),
      _snapshotMutex(nullptr),
      _task(nullptr),
      _buffer(nullptr),
      _bufferBytes(0),
      _generation(1),
      _taskGeneration(0),
      _active(false),
      _snapshot{},
      _testStartedMs(0),
      _lastProgressPublishMs(0) {}

bool WiFiSpeedTestWorker::begin() {
  if (!_snapshotMutex) _snapshotMutex = xSemaphoreCreateMutex();
  if (!_snapshotMutex) return false;
  if (!_buffer) {
    if (psramFound()) _buffer = static_cast<uint8_t *>(ps_malloc(PreferredBufferBytes));
    if (_buffer) _bufferBytes = PreferredBufferBytes;
    else {
      _buffer = static_cast<uint8_t *>(malloc(FallbackBufferBytes));
      _bufferBytes = _buffer ? FallbackBufferBytes : 0;
    }
    if (_buffer) memset(_buffer, 0, _bufferBytes);
  }
  if (!_buffer) {
    publishError("Speed test memory unavailable");
    return false;
  }
  {
    StateLock lock(_snapshotMutex);
    if (!lock.locked()) return false;
    const uint32_t revision = _snapshot.revision + 1;
    _snapshot = {};
    _snapshot.revision = revision;
    copyStatus(_snapshot.status, sizeof(_snapshot.status), "Ready");
  }
  return true;
}

bool WiFiSpeedTestWorker::start() {
  if (_active || !_wifi.isConnected()) return false;
  if (!begin()) return false;
  _generation = _generation + 1;
  if (_generation == 0) _generation = 1;
  _taskGeneration = _generation;
  _active = true;
  _testStartedMs = millis();
  _lastProgressPublishMs = 0;
  publish(WiFiSpeedTestPhase::Latency, 0, "Preparing secure connection");
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  const BaseType_t workerCore = 0;
#else
  const BaseType_t workerCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
  if (xTaskCreatePinnedToCore(taskMain, "wifi-speed", 12288, this,
                              tskIDLE_PRIORITY + 1, &_task, workerCore) != pdPASS) {
    _active = false;
    _task = nullptr;
    publishError("Speed test worker unavailable");
    return false;
  }
  Serial.printf("[Wi-Fi speed test] worker started on core %d, buffer=%u bytes\n",
                static_cast<int>(workerCore), static_cast<unsigned>(_bufferBytes));
  return true;
}

void WiFiSpeedTestWorker::cancel() {
  if (!_active) return;
  _generation = _generation + 1;
  if (_generation == 0) _generation = 1;
  publish(WiFiSpeedTestPhase::Cancelled, 0, "Cancelling...");
}

bool WiFiSpeedTestWorker::end() {
  cancel();
  if (_active) return false;
  _client.stop();
  _task = nullptr;
  if (_buffer) free(_buffer);
  _buffer = nullptr;
  _bufferBytes = 0;
  if (_snapshotMutex) {
    vSemaphoreDelete(_snapshotMutex);
    _snapshotMutex = nullptr;
  }
  return true;
}

WiFiSpeedTestSnapshot WiFiSpeedTestWorker::snapshot() const {
  if (!_snapshotMutex) return _snapshot;
  StateLock lock(_snapshotMutex);
  return lock.locked() ? _snapshot : WiFiSpeedTestSnapshot{};
}

void WiFiSpeedTestWorker::taskMain(void *context) {
  WiFiSpeedTestWorker *worker = static_cast<WiFiSpeedTestWorker *>(context);
  const uint32_t generation = worker->_taskGeneration;
  worker->run(generation);
  worker->_client.stop();
  if (!worker->current(generation)) {
    worker->publish(WiFiSpeedTestPhase::Cancelled, 0, "Test cancelled");
  }
  worker->_task = nullptr;
  worker->_active = false;
  Serial.println(F("[Wi-Fi speed test] worker stopped"));
  vTaskDelete(nullptr);
}

void WiFiSpeedTestWorker::run(uint32_t generation) {
  const char *error = nullptr;
  uint32_t firstByteMs = 0;
  uint32_t durationMs = 0;
  uint32_t transferred = 0;

  // Warm DNS, TCP and TLS without counting their setup time as latency.
  if (!performGet(generation, 0, firstByteMs, durationMs, transferred,
                  WiFiSpeedTestPhase::Latency, 0, 1, error)) {
    if (current(generation)) publishError(error ? error : "Connection failed");
    return;
  }

  uint32_t latency[LatencyProbeCount] = {};
  for (uint8_t index = 0; index < LatencyProbeCount; ++index) {
    if (!current(generation)) return;
    if (!performGet(generation, 0, firstByteMs, durationMs, transferred,
                    WiFiSpeedTestPhase::Latency, index, LatencyProbeCount, error)) {
      if (current(generation)) publishError(error ? error : "Latency test failed");
      return;
    }
    latency[index] = durationMs;
    publish(WiFiSpeedTestPhase::Latency,
            static_cast<uint8_t>(((index + 1) * 100U) / LatencyProbeCount),
            "Measuring latency");
  }
  for (uint8_t i = 1; i < LatencyProbeCount; ++i) {
    const uint32_t value = latency[i];
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && latency[j] > value) {
      latency[j + 1] = latency[j];
      --j;
    }
    latency[j + 1] = value;
  }
  const float latencyMs = latency[LatencyProbeCount / 2];

  uint64_t downloadBytes = 0;
  uint64_t downloadTimeMs = 0;
  uint32_t completed = 0;
  for (uint8_t index = 0; index < 3; ++index) {
    if (!current(generation)) return;
    char status[40];
    snprintf(status, sizeof(status), "Download %u of 3", index + 1);
    publish(WiFiSpeedTestPhase::Download,
            static_cast<uint8_t>((completed * 100ULL) / DirectionMaximumBytes),
            status, latencyMs);
    if (!performGet(generation, StageBytes[index], firstByteMs, durationMs, transferred,
                    WiFiSpeedTestPhase::Download, completed, DirectionMaximumBytes, error)) {
      if (current(generation)) publishError(error ? error : "Download test failed");
      return;
    }
    downloadBytes += transferred;
    downloadTimeMs += durationMs;
    completed += transferred;
    if (durationMs >= SlowStageMs || downloadTimeMs >= DirectionTargetMs) break;
  }
  if (downloadBytes == 0 || downloadTimeMs == 0) {
    publishError("Download result unavailable");
    return;
  }
  const float downloadMbps = static_cast<float>(downloadBytes * 8.0 / downloadTimeMs / 1000.0);

  // Download bytes are untrusted remote data. Replace them before this same
  // bounded buffer is used as the synthetic upload payload.
  memset(_buffer, 0, _bufferBytes);
  uint64_t uploadBytes = 0;
  uint64_t uploadTimeMs = 0;
  completed = 0;
  for (uint8_t index = 0; index < 3; ++index) {
    if (!current(generation)) return;
    char status[40];
    snprintf(status, sizeof(status), "Upload %u of 3", index + 1);
    publish(WiFiSpeedTestPhase::Upload,
            static_cast<uint8_t>((completed * 100ULL) / DirectionMaximumBytes),
            status, latencyMs, downloadMbps);
    if (!performUpload(generation, StageBytes[index], durationMs, transferred,
                       completed, DirectionMaximumBytes, error)) {
      if (current(generation)) publishError(error ? error : "Upload test failed");
      return;
    }
    uploadBytes += transferred;
    uploadTimeMs += durationMs;
    completed += transferred;
    if (durationMs >= SlowStageMs || uploadTimeMs >= DirectionTargetMs) break;
  }
  if (uploadBytes == 0 || uploadTimeMs == 0) {
    publishError("Upload result unavailable");
    return;
  }
  const float uploadMbps = static_cast<float>(uploadBytes * 8.0 / uploadTimeMs / 1000.0);
  publish(WiFiSpeedTestPhase::Complete, 100, "Test complete",
          latencyMs, downloadMbps, uploadMbps);
}

bool WiFiSpeedTestWorker::current(uint32_t generation) const {
  return generation != 0 && generation == _generation;
}

bool WiFiSpeedTestWorker::timedOut() const {
  return millis() - _testStartedMs >= WholeTestTimeoutMs;
}

void WiFiSpeedTestWorker::publish(WiFiSpeedTestPhase phase, uint8_t progress,
                                  const char *status, float latencyMs,
                                  float downloadMbps, float uploadMbps,
                                  bool throttle) {
  const uint32_t nowMs = millis();
  if (throttle && nowMs - _lastProgressPublishMs < 250) return;
  _lastProgressPublishMs = nowMs;
  if (!_snapshotMutex) return;
  StateLock lock(_snapshotMutex);
  if (!lock.locked()) return;
  _snapshot.phase = phase;
  _snapshot.progress = progress > 100 ? 100 : progress;
  if (latencyMs >= 0.0f) _snapshot.latencyMs = latencyMs;
  if (downloadMbps >= 0.0f) _snapshot.downloadMbps = downloadMbps;
  if (uploadMbps >= 0.0f) _snapshot.uploadMbps = uploadMbps;
  copyStatus(_snapshot.status, sizeof(_snapshot.status), status);
  ++_snapshot.revision;
}

void WiFiSpeedTestWorker::publishError(const char *message) {
  publish(WiFiSpeedTestPhase::Error, 0, message ? message : "Speed test failed");
}

bool WiFiSpeedTestWorker::ensureConnected(uint32_t generation, const char *&error) {
  if (!current(generation)) { error = "Cancelled"; return false; }
  if (!_wifi.isConnected()) { error = "Wi-Fi connection lost"; return false; }
  if (timedOut()) { error = "Speed test timed out"; return false; }
  if (_client.connected()) return true;
  _client.stop();
  _client.setInsecure();
  _client.setHandshakeTimeout(ConnectTimeoutMs / 1000);
  _client.setTimeout(NoProgressTimeoutMs);
  TlsMemoryScope tlsMemory(true);
  if (!_client.connect(Host, Port, static_cast<int32_t>(ConnectTimeoutMs))) {
    error = "Cannot connect to test server";
    return false;
  }
  return true;
}

bool WiFiSpeedTestWorker::writeAll(uint32_t generation, const uint8_t *data,
                                   size_t bytes, uint32_t requestStartedMs,
                                   const char *&error) {
  size_t written = 0;
  uint32_t lastProgressMs = millis();
  while (written < bytes) {
    if (!current(generation)) { error = "Cancelled"; return false; }
    if (!_wifi.isConnected()) { error = "Wi-Fi connection lost"; return false; }
    if (timedOut() || millis() - requestStartedMs >= RequestTimeoutMs) {
      error = "Request timed out";
      return false;
    }
    const size_t count = _client.write(data + written, bytes - written);
    if (count > 0) {
      written += count;
      lastProgressMs = millis();
    } else if (!_client.connected()) {
      error = "Connection closed early";
      return false;
    } else if (millis() - lastProgressMs >= NoProgressTimeoutMs) {
      error = "Network stalled";
      return false;
    } else {
      vTaskDelay(1);
    }
  }
  return true;
}

bool WiFiSpeedTestWorker::readLine(uint32_t generation, String &line,
                                   uint32_t requestStartedMs, uint32_t *firstByteMs,
                                   const char *&error) {
  line = "";
  line.reserve(96);
  uint32_t lastProgressMs = millis();
  while (true) {
    if (!current(generation)) { error = "Cancelled"; return false; }
    if (!_wifi.isConnected()) { error = "Wi-Fi connection lost"; return false; }
    if (timedOut() || millis() - requestStartedMs >= RequestTimeoutMs) {
      error = "Request timed out";
      return false;
    }
    if (_client.available() > 0) {
      const int value = _client.read();
      if (value < 0) continue;
      if (firstByteMs && *firstByteMs == 0) *firstByteMs = millis();
      lastProgressMs = millis();
      if (value == '\n') return true;
      if (value != '\r') {
        if (line.length() >= MaxHeaderLineBytes) {
          error = "Test server header is too long";
          return false;
        }
        line += static_cast<char>(value);
      }
      continue;
    }
    if (!_client.connected()) { error = "Connection closed early"; return false; }
    if (millis() - lastProgressMs >= NoProgressTimeoutMs) {
      error = "Network stalled";
      return false;
    }
    vTaskDelay(1);
  }
}

bool WiFiSpeedTestWorker::readResponseHeaders(uint32_t generation,
                                              HttpResponse &response,
                                              uint32_t requestStartedMs,
                                              const char *&error) {
  response = {};
  String line;
  if (!readLine(generation, line, requestStartedMs, &response.firstByteMs, error)) return false;
  const int firstSpace = line.indexOf(' ');
  if (!line.startsWith("HTTP/") || firstSpace < 0) {
    error = "Invalid test server response";
    return false;
  }
  response.statusCode = line.substring(firstSpace + 1).toInt();
  for (uint8_t count = 0; count < MaxHeaderLines; ++count) {
    if (!readLine(generation, line, requestStartedMs, nullptr, error)) return false;
    if (line.length() == 0) return true;
    if (equalsIgnoreCasePrefix(line, "Content-Length:")) {
      line = line.substring(15);
      line.trim();
      response.contentLength = line.toInt();
    } else if (equalsIgnoreCasePrefix(line, "Transfer-Encoding:")) {
      line.toLowerCase();
      response.chunked = line.indexOf("chunked") >= 0;
    } else if (equalsIgnoreCasePrefix(line, "Connection:")) {
      line.toLowerCase();
      response.connectionClose = line.indexOf("close") >= 0;
    }
  }
  error = "Too many test server headers";
  return false;
}

bool WiFiSpeedTestWorker::readResponseBody(
    uint32_t generation, const HttpResponse &response, uint32_t expectedBytes,
    uint32_t requestStartedMs, uint32_t &payloadStartedMs,
    uint32_t &payloadFinishedMs, uint32_t &payloadBytes,
    WiFiSpeedTestPhase phase, uint32_t completedBefore,
    uint32_t directionMaximum, const char *&error) {
  payloadStartedMs = 0;
  payloadFinishedMs = 0;
  payloadBytes = 0;
  if (response.chunked) {
    return readChunkedBytes(generation, expectedBytes, requestStartedMs,
                            payloadStartedMs, payloadFinishedMs, payloadBytes,
                            phase, completedBefore, directionMaximum, error);
  }
  if (response.contentLength < 0 || static_cast<uint32_t>(response.contentLength) != expectedBytes) {
    error = "Unexpected test payload size";
    return false;
  }
  return readFixedBytes(generation, expectedBytes, requestStartedMs,
                        payloadStartedMs, payloadFinishedMs, payloadBytes,
                        phase, completedBefore, directionMaximum, error);
}

bool WiFiSpeedTestWorker::readFixedBytes(
    uint32_t generation, uint32_t bytes, uint32_t requestStartedMs,
    uint32_t &payloadStartedMs, uint32_t &payloadFinishedMs,
    uint32_t &payloadBytes, WiFiSpeedTestPhase phase,
    uint32_t completedBefore, uint32_t directionMaximum,
    const char *&error) {
  if (bytes == 0) {
    payloadStartedMs = payloadFinishedMs = millis();
    return true;
  }
  uint32_t lastProgressMs = millis();
  while (payloadBytes < bytes) {
    if (!current(generation)) { error = "Cancelled"; return false; }
    if (!_wifi.isConnected()) { error = "Wi-Fi connection lost"; return false; }
    if (timedOut() || millis() - requestStartedMs >= RequestTimeoutMs) {
      error = "Request timed out";
      return false;
    }
    const int available = _client.available();
    if (available > 0) {
      const size_t remaining = bytes - payloadBytes;
      const size_t wanted = min(static_cast<size_t>(available), min(_bufferBytes, remaining));
      const int count = _client.read(_buffer, wanted);
      if (count > 0) {
        if (payloadStartedMs == 0) payloadStartedMs = millis();
        payloadBytes += static_cast<uint32_t>(count);
        payloadFinishedMs = millis();
        lastProgressMs = payloadFinishedMs;
        updateTransferProgress(generation, phase, completedBefore + payloadBytes,
                               directionMaximum,
                               phase == WiFiSpeedTestPhase::Download ? "Downloading" : "Uploading");
      }
      continue;
    }
    if (!_client.connected()) { error = "Connection closed early"; return false; }
    if (millis() - lastProgressMs >= NoProgressTimeoutMs) {
      error = "Network stalled";
      return false;
    }
    vTaskDelay(1);
  }
  return true;
}

bool WiFiSpeedTestWorker::readChunkedBytes(
    uint32_t generation, uint32_t expectedBytes, uint32_t requestStartedMs,
    uint32_t &payloadStartedMs, uint32_t &payloadFinishedMs,
    uint32_t &payloadBytes, WiFiSpeedTestPhase phase,
    uint32_t completedBefore, uint32_t directionMaximum,
    const char *&error) {
  while (true) {
    String line;
    if (!readLine(generation, line, requestStartedMs, nullptr, error)) return false;
    const int extension = line.indexOf(';');
    if (extension >= 0) line = line.substring(0, extension);
    line.trim();
    char *end = nullptr;
    const unsigned long chunk = strtoul(line.c_str(), &end, 16);
    if (!end || end == line.c_str() || *end != '\0') {
      error = "Invalid chunked test response";
      return false;
    }
    if (chunk == 0) {
      do {
        if (!readLine(generation, line, requestStartedMs, nullptr, error)) return false;
      } while (line.length() > 0);
      if (payloadBytes != expectedBytes) {
        error = "Unexpected test payload size";
        return false;
      }
      if (expectedBytes == 0) payloadStartedMs = payloadFinishedMs = millis();
      return true;
    }
    if (chunk > expectedBytes - payloadBytes) {
      error = "Test payload is too large";
      return false;
    }
    const uint32_t before = payloadBytes;
    uint32_t chunkStarted = payloadStartedMs;
    uint32_t chunkFinished = payloadFinishedMs;
    uint32_t chunkRead = 0;
    if (!readFixedBytes(generation, static_cast<uint32_t>(chunk), requestStartedMs,
                        chunkStarted, chunkFinished, chunkRead, phase,
                        completedBefore + before, directionMaximum, error)) return false;
    if (payloadStartedMs == 0) payloadStartedMs = chunkStarted;
    payloadFinishedMs = chunkFinished;
    payloadBytes += chunkRead;
    if (!consumeCrlf(generation, requestStartedMs, error)) return false;
  }
}

bool WiFiSpeedTestWorker::performGet(
    uint32_t generation, uint32_t bytes, uint32_t &firstByteMs,
    uint32_t &payloadDurationMs, uint32_t &payloadBytes,
    WiFiSpeedTestPhase phase, uint32_t completedBefore,
    uint32_t directionMaximum, const char *&error) {
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (!ensureConnected(generation, error)) return false;
    const uint32_t requestStartedMs = millis();
    String header = F("GET /__down?bytes=");
    header += bytes;
    header += F(" HTTP/1.1\r\nHost: speed.cloudflare.com\r\nUser-Agent: ESP-Media/1\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n");
    if (!sendRequestHeader(generation, header, requestStartedMs, error)) {
      _client.stop();
      if (attempt == 0) continue;
      return false;
    }
    HttpResponse response;
    if (!readResponseHeaders(generation, response, requestStartedMs, error)) {
      _client.stop();
      if (attempt == 0) continue;
      return false;
    }
    firstByteMs = response.firstByteMs;
    if (!statusSucceeded(response.statusCode)) {
      error = "Test server rejected request";
      _client.stop();
      return false;
    }
    uint32_t payloadStartedMs = 0;
    uint32_t payloadFinishedMs = 0;
    payloadBytes = 0;
    if (!readResponseBody(generation, response, bytes, requestStartedMs,
                          payloadStartedMs, payloadFinishedMs, payloadBytes,
                          phase, completedBefore, directionMaximum, error)) {
      _client.stop();
      // Retrying is safe only before any payload was transferred, preserving
      // the screen's strict traffic ceiling.
      if (attempt == 0 && payloadBytes == 0) continue;
      return false;
    }
    payloadDurationMs = bytes == 0
      ? max(1UL, response.firstByteMs - requestStartedMs)
      : max(1UL, payloadFinishedMs - payloadStartedMs);
    if (response.connectionClose) _client.stop();
    return true;
  }
  return false;
}

bool WiFiSpeedTestWorker::performUpload(
    uint32_t generation, uint32_t bytes, uint32_t &payloadDurationMs,
    uint32_t &payloadBytes, uint32_t completedBefore,
    uint32_t directionMaximum, const char *&error) {
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (!ensureConnected(generation, error)) return false;
    const uint32_t requestStartedMs = millis();
    String header = F("POST /__up?bytes=");
    header += bytes;
    header += F(" HTTP/1.1\r\nHost: speed.cloudflare.com\r\nUser-Agent: ESP-Media/1\r\nContent-Type: application/octet-stream\r\nContent-Length: ");
    header += bytes;
    header += F("\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n");
    if (!sendRequestHeader(generation, header, requestStartedMs, error)) {
      _client.stop();
      if (attempt == 0) continue;
      return false;
    }

    payloadBytes = 0;
    const uint32_t payloadStartedMs = millis();
    uint32_t lastProgressMs = payloadStartedMs;
    while (payloadBytes < bytes) {
      if (!current(generation)) { error = "Cancelled"; _client.stop(); return false; }
      if (!_wifi.isConnected()) { error = "Wi-Fi connection lost"; _client.stop(); return false; }
      if (timedOut() || millis() - requestStartedMs >= RequestTimeoutMs) {
        error = "Request timed out";
        _client.stop();
        return false;
      }
      const size_t wanted = min(_bufferBytes, static_cast<size_t>(bytes - payloadBytes));
      const size_t count = _client.write(_buffer, wanted);
      if (count > 0) {
        payloadBytes += static_cast<uint32_t>(count);
        lastProgressMs = millis();
        updateTransferProgress(generation, WiFiSpeedTestPhase::Upload,
                               completedBefore + payloadBytes, directionMaximum,
                               "Uploading");
      } else if (!_client.connected()) {
        error = "Connection closed early";
        _client.stop();
        // Retry only if the request carried no payload.
        if (attempt == 0 && payloadBytes == 0) break;
        return false;
      } else if (millis() - lastProgressMs >= NoProgressTimeoutMs) {
        error = "Network stalled";
        _client.stop();
        return false;
      } else {
        vTaskDelay(1);
      }
    }
    if (payloadBytes == 0 && attempt == 0) continue;
    HttpResponse response;
    if (!readResponseHeaders(generation, response, requestStartedMs, error)) {
      _client.stop();
      return false;
    }
    payloadDurationMs = max(1UL, response.firstByteMs - payloadStartedMs);
    if (!statusSucceeded(response.statusCode)) {
      error = "Test server rejected upload";
      _client.stop();
      return false;
    }
    uint32_t ignoredStart = 0;
    uint32_t ignoredFinish = 0;
    uint32_t ignoredBytes = 0;
    const uint32_t responseBytes = response.contentLength < 0
      ? 0 : static_cast<uint32_t>(response.contentLength);
    if (response.chunked || responseBytes > 0) {
      if (!readResponseBody(generation, response, responseBytes, requestStartedMs,
                            ignoredStart, ignoredFinish, ignoredBytes,
                            WiFiSpeedTestPhase::Upload, completedBefore,
                            directionMaximum, error)) {
        _client.stop();
        return false;
      }
    }
    if (response.connectionClose) _client.stop();
    return true;
  }
  return false;
}

bool WiFiSpeedTestWorker::sendRequestHeader(uint32_t generation,
                                            const String &header,
                                            uint32_t requestStartedMs,
                                            const char *&error) {
  return writeAll(generation, reinterpret_cast<const uint8_t *>(header.c_str()),
                  header.length(), requestStartedMs, error);
}

bool WiFiSpeedTestWorker::consumeCrlf(uint32_t generation,
                                     uint32_t requestStartedMs,
                                     const char *&error) {
  uint8_t bytes[2] = {};
  uint8_t count = 0;
  uint32_t lastProgressMs = millis();
  while (count < 2) {
    if (!current(generation)) { error = "Cancelled"; return false; }
    if (timedOut() || millis() - requestStartedMs >= RequestTimeoutMs) {
      error = "Request timed out";
      return false;
    }
    if (_client.available() > 0) {
      const int value = _client.read();
      if (value >= 0) {
        bytes[count++] = static_cast<uint8_t>(value);
        lastProgressMs = millis();
      }
    } else if (!_client.connected()) {
      error = "Connection closed early";
      return false;
    } else if (millis() - lastProgressMs >= NoProgressTimeoutMs) {
      error = "Network stalled";
      return false;
    } else {
      vTaskDelay(1);
    }
  }
  if (bytes[0] != '\r' || bytes[1] != '\n') {
    error = "Invalid chunked test response";
    return false;
  }
  return true;
}

void WiFiSpeedTestWorker::updateTransferProgress(
    uint32_t generation, WiFiSpeedTestPhase phase, uint32_t completed,
    uint32_t directionMaximum, const char *status) {
  if (!current(generation) || directionMaximum == 0) return;
  uint32_t progressValue = static_cast<uint32_t>((completed * 100ULL) / directionMaximum);
  if (progressValue > 100) progressValue = 100;
  const uint8_t progress = static_cast<uint8_t>(progressValue);
  const WiFiSpeedTestSnapshot currentSnapshot = snapshot();
  publish(phase, progress, status, currentSnapshot.latencyMs,
          currentSnapshot.downloadMbps, currentSnapshot.uploadMbps, true);
}

bool WiFiSpeedTestWorker::statusSucceeded(int statusCode) {
  return statusCode >= 200 && statusCode < 300;
}

bool WiFiSpeedTestWorker::equalsIgnoreCasePrefix(const String &line,
                                                 const char *prefix) {
  const size_t length = strlen(prefix);
  return line.length() >= length && line.substring(0, length).equalsIgnoreCase(prefix);
}
