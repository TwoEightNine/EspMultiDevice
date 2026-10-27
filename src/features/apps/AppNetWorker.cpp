#include "AppNetWorker.h"

#include <esp_heap_caps.h>
#include <new>

namespace {
constexpr uint32_t WorkerStackBytes = 16384;

bool jsonContentType(const String &type) {
  return type.indexOf("json") >= 0;
}
}

// Policy and body sink for one request. Progress is published to the slot
// under the worker mutex; the body goes to a PSRAM buffer or the JSON filter.
class AppNetWorker::Transfer : public HttpClient::Policy, public HttpClient::Sink {
public:
  Transfer(AppNetWorker &owner, Slot &slot, Result &result)
    : _owner(owner), _slot(slot), _result(result), _spec(*slot.spec) {}
  ~Transfer() { if (_buffer) AppNetWorker::free(_buffer); }

  bool cancelled() const override { return _slot.cancel || _owner._stopping; }

  bool hostAllowed(const HttpClient::Url &url) const override {
    return _owner.hostAllowed(url.host);
  }

  void progress(uint8_t phase, uint32_t received, int32_t total) override {
    if (xSemaphoreTake(_owner._mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    _slot.state = phase == HttpClient::Connecting ? State::Connecting
                : phase == HttpClient::Sending ? State::Sending : State::Receiving;
    _slot.received = received;
    _slot.total = total;
    xSemaphoreGive(_owner._mutex);
  }

  bool begin(int status, const String &contentType, int32_t) override {
    _json = _spec.json && status >= 200 && status < 300 && jsonContentType(contentType);
    _streamed = 0;
    if (_json) {
      _result.json = new (std::nothrow) JsonPick(AppNetWorker::allocate, AppNetWorker::free);
      if (!_result.json || !_result.json->begin(_spec.jsonOptions)) {
        _error = "App memory unavailable";
        return false;
      }
      return true;
    }
    _limit = _spec.json ? NonJsonBodyBytes : _spec.maxBytes;
    return true;
  }

  bool write(const uint8_t *data, size_t length) override {
    _streamed += length;
    if (_json) {
      if (_streamed > MaxJsonStreamBytes) { _error = "Response too large"; return false; }
      if (!_result.json->feed(data, length)) { _error = _result.json->error(); return false; }
      return true;
    }
    const size_t room = _limit - _length;
    const size_t keep = length < room ? length : room;
    if (keep) {
      if (!reserve(_length + keep)) { _error = "App memory unavailable"; return false; }
      memcpy(_buffer + _length, data, keep);
      _length += keep;
    }
    if (keep < length) {
      // The app asked for at most `max_bytes`; stop reading here.
      _result.truncated = true;
      _stoppedAtLimit = true;
      return false;
    }
    return true;
  }

  bool finish() override {
    if (_json) {
      if (!_result.json->finish()) { _error = _result.json->error(); return false; }
      _result.truncated = _result.json->truncated();
      return true;
    }
    return true;
  }

  // Hands the text body to the result.
  void complete() {
    if (_json) return;
    if (!reserve(_length + 1)) return;
    _buffer[_length] = 0;
    _result.body = reinterpret_cast<char *>(_buffer);
    _result.bodyLength = _length;
    _buffer = nullptr;
  }

  const String &error() const { return _error; }
  bool stoppedAtLimit() const { return _stoppedAtLimit; }

private:
  AppNetWorker &_owner;
  Slot &_slot;
  Result &_result;
  Spec &_spec;
  bool _json = false;
  bool _stoppedAtLimit = false;
  uint32_t _streamed = 0;
  uint8_t *_buffer = nullptr;
  size_t _capacity = 0;
  size_t _length = 0;
  size_t _limit = 0;
  String _error;

  bool reserve(size_t bytes) {
    if (bytes <= _capacity) return true;
    size_t next = _capacity ? _capacity * 2 : 4096;
    while (next < bytes) next *= 2;
    uint8_t *grown = static_cast<uint8_t *>(
      heap_caps_realloc(_buffer, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!grown) return false;
    _buffer = grown;
    _capacity = next;
    return true;
  }
};

void *AppNetWorker::allocate(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void AppNetWorker::free(void *pointer) { heap_caps_free(pointer); }

AppNetWorker::~AppNetWorker() {
  while (!stop()) vTaskDelay(pdMS_TO_TICKS(10));
  if (_mutex) { vSemaphoreDelete(_mutex); _mutex = nullptr; }
}

void AppNetWorker::configure(const String hosts[], uint8_t hostCount, const String &userAgent) {
  _hostCount = hostCount > MaxHosts ? MaxHosts : hostCount;
  for (uint8_t i = 0; i < _hostCount; ++i) _hosts[i] = hosts[i];
  _userAgent = userAgent;
  if (!_mutex) _mutex = xSemaphoreCreateMutex();
  _stopping = false;
}

bool AppNetWorker::hostAllowed(const String &host) const {
  for (uint8_t i = 0; i < _hostCount; ++i) if (_hosts[i] == host) return true;
  return false;
}

bool AppNetWorker::ensureTask() {
  if (_task) return true;
  _stopping = false;
  _running = true;
  if (xTaskCreatePinnedToCore(taskMain, "app-net", WorkerStackBytes, this,
                              tskIDLE_PRIORITY + 1, &_task, 0) != pdPASS) {
    _task = nullptr;
    _running = false;
    return false;
  }
  return true;
}

uint32_t AppNetWorker::submit(Spec *spec, String &error) {
  if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    error = "Network busy";
    if (spec) { free(spec->body); delete spec; }
    return 0;
  }
  Slot *slot = nullptr;
  for (Slot &candidate : _slots) if (candidate.state == State::Free) { slot = &candidate; break; }
  if (!slot) {
    xSemaphoreGive(_mutex);
    error = "Too many requests";
    free(spec->body);
    delete spec;
    return 0;
  }
  slot->id = _nextId++;
  if (_nextId == 0) _nextId = 1;
  slot->state = State::Queued;
  slot->cancel = false;
  slot->received = 0;
  slot->total = -1;
  slot->spec = spec;
  slot->result = nullptr;
  const uint32_t id = slot->id;
  xSemaphoreGive(_mutex);
  if (!ensureTask()) {
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
      free(slot->spec->body);
      delete slot->spec;
      *slot = Slot();
      xSemaphoreGive(_mutex);
    }
    error = "Network task unavailable";
    return 0;
  }
  xTaskNotifyGive(_task);
  return id;
}

bool AppNetWorker::status(uint32_t id, State &state, uint32_t &received, int32_t &total) {
  if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  bool found = false;
  for (const Slot &slot : _slots) {
    if (slot.state != State::Free && slot.id == id) {
      state = slot.state;
      received = slot.received;
      total = slot.total;
      found = true;
      break;
    }
  }
  xSemaphoreGive(_mutex);
  return found;
}

void AppNetWorker::cancel(uint32_t id) {
  if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  for (Slot &slot : _slots) {
    if (slot.state != State::Free && slot.state != State::Done && (id == 0 || slot.id == id))
      slot.cancel = true;
  }
  xSemaphoreGive(_mutex);
}

bool AppNetWorker::active() {
  if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return true;
  bool busy = false;
  for (const Slot &slot : _slots) if (slot.state != State::Free) { busy = true; break; }
  xSemaphoreGive(_mutex);
  return busy;
}

bool AppNetWorker::take(Result *&result) {
  result = nullptr;
  if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return false;
  Slot *oldest = nullptr;
  for (Slot &slot : _slots) {
    if (slot.state == State::Done && (!oldest || slot.id < oldest->id)) oldest = &slot;
  }
  if (oldest) {
    result = oldest->result;
    *oldest = Slot();
  }
  xSemaphoreGive(_mutex);
  return result != nullptr;
}

void AppNetWorker::release(Result *result) {
  if (!result) return;
  if (result->body) free(result->body);
  delete result->json;
  delete result;
}

bool AppNetWorker::stop() {
  if (_task) {
    _stopping = true;
    cancel(0);
    xTaskNotifyGive(_task);
    if (_running) return false;
    vTaskDelete(_task);
    _task = nullptr;
  }
  if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
    for (Slot &slot : _slots) {
      if (slot.spec) { free(slot.spec->body); delete slot.spec; }
      release(slot.result);
      slot = Slot();
    }
    xSemaphoreGive(_mutex);
  }
  return true;
}

void AppNetWorker::taskMain(void *context) {
  AppNetWorker *worker = static_cast<AppNetWorker *>(context);
  worker->run();
  worker->_running = false;
  vTaskDelay(portMAX_DELAY);
}

int AppNetWorker::nextQueued() {
  int index = -1;
  if (xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return -1;
  for (int i = 0; i < MaxRequests; ++i) {
    if (_slots[i].state == State::Queued && (index < 0 || _slots[i].id < _slots[index].id)) index = i;
  }
  xSemaphoreGive(_mutex);
  return index;
}

void AppNetWorker::run() {
  _client = new (std::nothrow) HttpClient();
  if (_client) _client->begin();
  while (!_stopping) {
    const int index = nextQueued();
    if (index < 0) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }
    execute(_slots[index]);
  }
  if (_client) {
    _client->end();
    delete _client;
    _client = nullptr;
  }
}

void AppNetWorker::execute(Slot &slot) {
  Result *result = new (std::nothrow) Result();
  if (result) result->id = slot.id;
  if (!result || !_client) {
    if (result) result->error = "App memory unavailable";
  } else if (slot.cancel || _stopping) {
    result->error = "Cancelled";
  } else {
    Spec &spec = *slot.spec;
    HttpClient::Request request;
    request.method = spec.method;
    request.url = spec.url;
    request.headers = spec.headers;
    request.headerCount = spec.headerCount;
    request.body = spec.body;
    request.bodyLength = spec.bodyLength;
    request.followRedirects = spec.followRedirects;
    request.timeoutMs = spec.timeoutMs;
    request.userAgent = _userAgent;
    HttpClient::Response response;
    Transfer transfer(*this, slot, *result);
    const bool performed = _client->perform(request, transfer, transfer, response);
    result->status = response.status;
    result->finalUrl = response.finalUrl;
    result->headerCount = response.headerCount;
    for (uint8_t i = 0; i < response.headerCount; ++i) result->headers[i] = response.headers[i];
    if (performed || transfer.stoppedAtLimit()) {
      transfer.complete();
    } else if (slot.cancel || _stopping) {
      result->error = "Cancelled";
    } else if (transfer.error().length()) {
      result->error = transfer.error();
    } else {
      result->error = response.error.length() ? response.error : String("Request failed");
    }
    if (result->error.length() && result->json) {
      delete result->json;
      result->json = nullptr;
    }
    // A request cut at the app's own byte limit is still a response.
    if (transfer.stoppedAtLimit()) result->error = String();
  }
  if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
    if (slot.spec) { free(slot.spec->body); delete slot.spec; slot.spec = nullptr; }
    slot.result = result;
    slot.state = result ? State::Done : State::Free;
    xSemaphoreGive(_mutex);
  }
}
