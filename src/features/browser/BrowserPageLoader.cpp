#include "BrowserPageLoader.h"

#include <time.h>
#include <utility>

#include "BrowserLimits.h"
#include "BrowserPersistentState.h"
#include "BrowserSessionCache.h"
#include "BrowserText.h"

namespace {
void loaderLog(const __FlashStringHelper *message) {
  Serial.print(F("[Browser loader] "));
  Serial.println(message);
}

void loaderLog(const String &message) {
  Serial.print(F("[Browser loader] "));
  Serial.println(message);
}

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
}  // namespace

BrowserPageLoader::BrowserPageLoader(BrowserTransport &transport,
                                     BrowserSessionCache &sessionCache,
                                     BrowserPersistentState &persistentState)
    : _transport(transport),
      _sessionCache(sessionCache),
      _persistentState(persistentState),
      _stateMutex(xSemaphoreCreateMutex()),
      _task(nullptr),
      _jobPending(false),
      _busy(false),
      _resultReady(false),
      _progressPending(false),
      _generation(1),
      _activeGeneration(0) {}

bool BrowserPageLoader::begin() {
  if (_task) return true;
  if (!_stateMutex) {
    loaderLog(F("worker unavailable: state mutex allocation failed"));
    return false;
  }
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  const BaseType_t workerCore = 0;
#else
  const BaseType_t workerCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
  const bool started = xTaskCreatePinnedToCore(taskMain, "browser-pages", 16384, this,
                                               tskIDLE_PRIORITY + 1, &_task,
                                               workerCore) == pdPASS;
  if (started) loaderLog(String(F("worker started on core ")) + workerCore);
  else loaderLog(F("worker task creation failed"));
  return started;
}

bool BrowserPageLoader::load(const Browser::HttpRequest &request,
                             const Browser::LoadOptions &options) {
  if (!_task || !_stateMutex) return false;
  {
    StateLock lock(_stateMutex);
    if (!lock.locked() || _busy || _resultReady) return false;
    ++_generation;
    if (_generation == 0) _generation = 1;
    _job.generation = _generation;
    _job.request = request;
    _job.options = options;
    _requestUrl = request.url;
    _progress = {};
    _result = {};
    _jobPending = true;
    _busy = true;
    _resultReady = false;
    _progressPending = false;
  }
  xTaskNotifyGive(_task);
  return true;
}

void BrowserPageLoader::cancel() {
  if (!_stateMutex) return;
  StateLock lock(_stateMutex);
  if (!lock.locked() || !_busy) return;
  ++_generation;
  if (_generation == 0) _generation = 1;
  _jobPending = false;
  _busy = false;
  _progressPending = false;
  _result = cancelledResult(_requestUrl);
  _resultReady = true;
  loaderLog(F("cancellation published"));
}

bool BrowserPageLoader::takeProgress(Browser::PageLoadProgress &progress) {
  if (!_stateMutex) return false;
  StateLock lock(_stateMutex);
  if (!lock.locked() || !_progressPending) return false;
  progress = _progress;
  _progressPending = false;
  return true;
}

bool BrowserPageLoader::takeResult(Browser::PageLoadResult &result) {
  if (!_stateMutex) return false;
  StateLock lock(_stateMutex);
  if (!lock.locked() || !_resultReady) return false;
  result = std::move(_result);
  _result = {};
  _resultReady = false;
  _requestUrl = "";
  return true;
}

void BrowserPageLoader::taskMain(void *context) {
  BrowserPageLoader *loader = static_cast<BrowserPageLoader *>(context);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Job job;
    {
      StateLock lock(loader->_stateMutex);
      if (!lock.locked() || !loader->_jobPending) continue;
      job = std::move(loader->_job);
      loader->_job = {};
      loader->_jobPending = false;
      loader->_activeGeneration = job.generation;
    }

    Browser::PageLoadResult result;
    loader->run(job, result);
    loader->_activeGeneration = 0;
    if (!loader->publish(job.generation, result) && result.downloadPath.length()) {
      loaderLog(String(F("discarding stale download: ")) + result.downloadPath);
      loader->_transport.discardDownload(result.downloadPath);
    }
  }
}

void BrowserPageLoader::run(const Job &job, Browser::PageLoadResult &result) {
  result.finalUrl = job.request.url;
  Browser::HttpRequest request = job.request;
  String normalizedUrl;
  String error;
  if (!BrowserTransport::normalizeUrl(request.url, normalizedUrl, error)) {
    result.errorKind = Browser::PageLoadErrorKind::InvalidRequest;
    result.error = error;
    return;
  }
  request.url = normalizedUrl;
  result.finalUrl = normalizedUrl;
  if (request.method == Browser::HttpMethod::Post &&
      request.body.length() > BrowserLimits::MaxFormPostBytes) {
    result.errorKind = Browser::PageLoadErrorKind::InvalidRequest;
    result.error = "Form data is too large";
    return;
  }
  if (cancelled()) {
    result = cancelledResult(result.finalUrl);
    return;
  }

  String body;
  String contentType;
  const bool cacheEligible = job.options.useSessionCache &&
                             request.method == Browser::HttpMethod::Get;
  if (cacheEligible) {
    phaseChanged(F("Restoring cached page"), -1);
    if (_sessionCache.loadPage(request.url, body, contentType, this)) {
      result.fromSessionCache = true;
      loaderLog(String(F("session cache hit: ")) + request.url);
    } else if (cancelled()) {
      result = cancelledResult(result.finalUrl);
      return;
    }
  }

  if (!result.fromSessionCache) {
    Browser::Url parsedUrl;
    String cookies;
    if (BrowserTransport::parseUrl(request.url, parsedUrl)) cookies = cookieHeaderFor(parsedUrl);
    const Browser::RequestOptions transferOptions{
      job.options.userAgent,
      cookies,
      job.options.bodyLimit,
      job.options.downloadLimit
    };
    Browser::TransferResult transfer = _transport.transfer(request, transferOptions, *this, *this);
    result.finalUrl = transfer.finalUrl.length() ? transfer.finalUrl : request.url;
    result.contentType = transfer.contentType;
    result.downloadPath = std::move(transfer.downloadPath);
    body = std::move(transfer.retainedText);
    if (!transfer.succeeded()) {
      if (cancelled() || transfer.error == F("Loading cancelled")) {
        result = cancelledResult(result.finalUrl);
      } else {
        result.errorKind = Browser::PageLoadErrorKind::Transport;
        result.error = transfer.error;
      }
      return;
    }
  } else {
    result.contentType = contentType;
  }

  String canonicalUrl;
  String canonicalError;
  if (BrowserTransport::normalizeUrl(result.finalUrl, canonicalUrl, canonicalError)) {
    result.finalUrl = canonicalUrl;
  }

  if (result.downloadPath.length()) {
    if (cancelled()) {
      _transport.discardDownload(result.downloadPath);
      result = cancelledResult(result.finalUrl);
      return;
    }
    result.outcome = Browser::PageLoadOutcome::Download;
    result.errorKind = Browser::PageLoadErrorKind::None;
    return;
  }

  const bool htmlPage = result.contentType.indexOf("text/html") >= 0 ||
                        result.contentType.indexOf("application/xhtml") >= 0 ||
                        result.contentType.length() == 0;
  const bool textPage = result.contentType.indexOf("text/plain") >= 0;
  if (!htmlPage && !textPage) {
    result.errorKind = Browser::PageLoadErrorKind::UnsupportedContent;
    result.error = "Unsupported content: " +
      (result.contentType.length() ? result.contentType : String("unknown type"));
    return;
  }

  if (!result.fromSessionCache) {
    phaseChanged(F("Normalizing text"), -1);
    String normalizedBody;
    String encodingError;
    if (!BrowserText::normalizeToUtf8(body, result.contentType,
                                      normalizedBody, encodingError)) {
      result.errorKind = Browser::PageLoadErrorKind::TextEncoding;
      result.error = encodingError;
      return;
    }
    body = std::move(normalizedBody);
    if (cancelled()) {
      result = cancelledResult(result.finalUrl);
      return;
    }
    phaseChanged(F("Caching page"), -1);
    if (_sessionCache.storePage(result.finalUrl, result.contentType, body, this)) {
      loaderLog(String(F("session page stored: ")) + result.finalUrl);
    }
    if (cancelled()) {
      result = cancelledResult(result.finalUrl);
      return;
    }
  }

  result.outcome = Browser::PageLoadOutcome::Text;
  result.errorKind = Browser::PageLoadErrorKind::None;
  result.textBody = std::move(body);
}

bool BrowserPageLoader::publish(uint32_t generation, Browser::PageLoadResult &result) {
  StateLock lock(_stateMutex);
  if (!lock.locked() || generation != _generation || !_busy) return false;
  _result = std::move(result);
  _resultReady = true;
  _progressPending = false;
  _busy = false;
  return true;
}

bool BrowserPageLoader::isCurrent(uint32_t generation) const {
  return generation != 0 && generation == _generation;
}

Browser::PageLoadResult BrowserPageLoader::cancelledResult(const String &url) const {
  Browser::PageLoadResult result;
  result.outcome = Browser::PageLoadOutcome::Error;
  result.errorKind = Browser::PageLoadErrorKind::Cancelled;
  result.finalUrl = url;
  result.error = "Loading cancelled";
  result.cancelled = true;
  return result;
}

bool BrowserPageLoader::cancelled() const {
  return !isCurrent(_activeGeneration);
}

void BrowserPageLoader::phaseChanged(const String &phase, int32_t totalBytes) {
  if (!_stateMutex || cancelled()) return;
  StateLock lock(_stateMutex);
  if (!lock.locked() || cancelled()) return;
  _progress.phase = phase;
  _progress.totalBytes = totalBytes;
  _progressPending = true;
}

void BrowserPageLoader::bytesReceived(uint32_t bytes) {
  if (!_stateMutex || bytes == 0 || cancelled()) return;
  StateLock lock(_stateMutex);
  if (!lock.locked() || cancelled()) return;
  _progress.bytesReceived += bytes;
  _progressPending = true;
}

void BrowserPageLoader::cookieReceived(const String &header, const Browser::Url &url) {
  if (!cancelled()) _persistentState.acceptCookie(header, url, nowEpoch());
}

String BrowserPageLoader::cookieHeaderFor(const Browser::Url &url) const {
  return _persistentState.cookieHeaderFor(url, nowEpoch());
}

uint64_t BrowserPageLoader::nowEpoch() {
  const time_t value = time(nullptr);
  return value > 1577836800 ? static_cast<uint64_t>(value) : 0;
}
