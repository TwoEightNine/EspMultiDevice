#pragma once

#include <Arduino.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "BrowserTransport.h"
#include "BrowserTypes.h"

class BrowserPersistentState;
class BrowserSessionCache;

// Owns foreground page/cache/download work on a worker task. It reports only
// copied progress snapshots and one terminal result; the UI task remains the
// sole owner of parsing, drawing, history, and Navigator operations.
class BrowserPageLoader : private Browser::TransferObserver,
                          private Browser::CookieProvider {
public:
  BrowserPageLoader(BrowserTransport &transport,
                    BrowserSessionCache &sessionCache,
                    BrowserPersistentState &persistentState);

  bool begin();
  bool load(const Browser::HttpRequest &request, const Browser::LoadOptions &options);
  void cancel();
  bool takeProgress(Browser::PageLoadProgress &progress);
  bool takeResult(Browser::PageLoadResult &result);

private:
  struct Job {
    uint32_t generation = 0;
    Browser::HttpRequest request;
    Browser::LoadOptions options;
  };

  BrowserTransport &_transport;
  BrowserSessionCache &_sessionCache;
  BrowserPersistentState &_persistentState;
  SemaphoreHandle_t _stateMutex;
  TaskHandle_t _task;
  Job _job;
  Browser::PageLoadResult _result;
  Browser::PageLoadProgress _progress;
  String _requestUrl;
  bool _jobPending;
  bool _busy;
  bool _resultReady;
  bool _progressPending;
  volatile uint32_t _generation;
  volatile uint32_t _activeGeneration;

  static void taskMain(void *context);
  void run(const Job &job, Browser::PageLoadResult &result);
  bool publish(uint32_t generation, Browser::PageLoadResult &result);
  bool isCurrent(uint32_t generation) const;
  Browser::PageLoadResult cancelledResult(const String &url) const;
  static uint64_t nowEpoch();

  bool cancelled() const override;
  void phaseChanged(const String &phase, int32_t totalBytes) override;
  void bytesReceived(uint32_t bytes) override;
  void cookieReceived(const String &header, const Browser::Url &url) override;
  String cookieHeaderFor(const Browser::Url &url) const override;
};
