#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../../services/network/HttpClient.h"
#include "JsonPick.h"

// Runs an SDK 6 app's HTTP requests on their own task, one at a time in
// submission order, so a connect or a slow server never blocks the app's
// callbacks or the UI. The app worker submits validated requests and takes
// finished results; it never touches the HTTP client.
class AppNetWorker {
public:
  static constexpr uint8_t MaxRequests = 4;
  static constexpr uint8_t MaxHosts = 8;
  static constexpr size_t MaxBodyBytes = 32768;
  static constexpr uint32_t MaxTextBytes = 96 * 1024;
  static constexpr uint32_t DefaultTextBytes = 32 * 1024;
  static constexpr uint32_t MaxJsonStreamBytes = 2 * 1024 * 1024;
  static constexpr uint32_t NonJsonBodyBytes = 4096;

  enum class State : uint8_t { Free, Queued, Connecting, Sending, Receiving, Done };

  struct Spec {
    HttpClient::Method method = HttpClient::Method::Get;
    String url;
    HttpClient::Header headers[HttpClient::MaxRequestHeaders];
    uint8_t headerCount = 0;
    uint8_t *body = nullptr;  // PSRAM; owned by the request
    size_t bodyLength = 0;
    bool followRedirects = true;
    uint32_t timeoutMs = 20000;
    uint32_t maxBytes = DefaultTextBytes;
    bool json = false;
    JsonPick::Options jsonOptions{};
    char keepText[JsonPick::MaxKeepKeys][JsonPick::MaxKeyBytes + 1] = {};
  };

  struct Result {
    uint32_t id = 0;
    int status = 0;
    String finalUrl;
    HttpClient::Header headers[HttpClient::MaxResponseHeaders];
    uint8_t headerCount = 0;
    char *body = nullptr;  // PSRAM; text mode or a non-JSON reply
    size_t bodyLength = 0;
    JsonPick *json = nullptr;  // JSON mode with a JSON reply
    bool truncated = false;
    String error;
  };

  AppNetWorker() = default;
  ~AppNetWorker();
  AppNetWorker(const AppNetWorker &) = delete;
  AppNetWorker &operator=(const AppNetWorker &) = delete;

  void configure(const String hosts[], uint8_t hostCount, const String &userAgent);
  bool hostAllowed(const String &host) const;
  // Takes ownership of `spec` (its body); returns the id, or 0 with `error`.
  uint32_t submit(Spec *spec, String &error);
  bool status(uint32_t id, State &state, uint32_t &received, int32_t &total);
  // `id` 0 cancels every request.
  void cancel(uint32_t id);
  bool active();
  // Moves the oldest finished result out; the caller frees it with release().
  bool take(Result *&result);
  static void release(Result *result);
  // Cancels everything and waits for the task to exit. False while a
  // transfer is still unwinding; call again.
  bool stop();

  static void *allocate(size_t bytes);
  static void free(void *pointer);

private:
  struct Slot {
    uint32_t id = 0;
    State state = State::Free;
    volatile bool cancel = false;
    uint32_t received = 0;
    int32_t total = -1;
    Spec *spec = nullptr;
    Result *result = nullptr;
  };

  class Transfer;

  String _hosts[MaxHosts];
  uint8_t _hostCount = 0;
  String _userAgent;
  Slot _slots[MaxRequests];
  uint32_t _nextId = 1;
  SemaphoreHandle_t _mutex = nullptr;
  TaskHandle_t _task = nullptr;
  volatile bool _stopping = false;
  volatile bool _running = false;
  HttpClient *_client = nullptr;

  bool ensureTask();
  static void taskMain(void *context);
  void run();
  int nextQueued();
  void execute(Slot &slot);
};
