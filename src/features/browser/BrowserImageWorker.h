#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "BrowserLimits.h"
#include "BrowserTransport.h"
#include "BrowserTypes.h"

class BrowserPersistentState;
class BrowserSessionCache;

// Owns the Browser's complete embedded-image pipeline. BrowserDocument creates
// POD jobs on the UI task and borrows completed pixel buffers for rendering;
// this worker retains and eventually releases every pixel allocation.
class BrowserImageWorker : private Browser::TransferObserver,
                           private Browser::CookieProvider {
public:
  BrowserImageWorker(BrowserTransport &transport,
                     BrowserSessionCache &sessionCache,
                     BrowserPersistentState &persistentState);

  bool begin();
  // Releases all previews, queues, synchronization objects, and the worker
  // stack after any cancelled decode/network operation has unwound.
  bool end();
  bool available() const { return _task != nullptr; }
  bool active() const { return _activeGeneration != 0; }
  uint32_t generation() const { return _generation; }

  bool enqueue(const Browser::ImageJob &job, const String &userAgent);
  // Copies one completion record to the UI task. Its pixel pointer remains
  // worker-owned: BrowserDocument may borrow it after acceptance, while a
  // rejected result must be returned through discard().
  bool take(Browser::ImageResult &result);
  // Marks a completion as borrowed by the current document. Pending-only
  // cancellation retains this allocation so an already drawn preview stays
  // valid, while discard() and destructive cancellation still release it.
  bool adopt(const Browser::ImageResult &result);
  void discard(const Browser::ImageResult &result);
  // Invalidates queued/active work and releases every completed allocation.
  // The UI task must clear its borrowed document descriptors immediately.
  void cancel();
  // Invalidates queued/active work without releasing document-adopted pixel
  // buffers. Use this for an in-page user cancellation that leaves the
  // current document on screen.
  void cancelPending();

private:
  static constexpr uint16_t UserAgentMaxLength = 192;

  struct QueuedJob {
    Browser::ImageJob image;
    char userAgent[UserAgentMaxLength];
  };

  struct CompletedImage {
    uint16_t imageIndex;
    uint32_t generation;
    uint16_t *pixels;
    bool adopted;
  };

  BrowserTransport &_transport;
  BrowserSessionCache &_sessionCache;
  BrowserPersistentState &_persistentState;
  QueueHandle_t _jobs;
  QueueHandle_t _results;
  SemaphoreHandle_t _stateMutex;
  TaskHandle_t _task;
  volatile uint32_t _generation;
  volatile uint32_t _activeGeneration;
  CompletedImage _completed[BrowserLimits::MaxEmbeddedImages];

  static void taskMain(void *context);
  void run(const QueuedJob &job);
  bool activate(uint32_t generation);
  void deactivate(uint32_t generation);
  bool isCurrent(uint32_t generation) const;
  bool retainCompleted(const Browser::ImageResult &result);
  void cancel(bool releaseAdopted);
  void releaseCompleted(uint16_t imageIndex, uint32_t generation, uint16_t *pixels);
  bool fetch(const String &url, const String &userAgent, uint16_t backgroundColor,
             bool useSessionCache, uint16_t *&pixels, uint8_t &width, uint8_t &height);
  Browser::RequestOptions requestOptions(const String &url, const String &userAgent) const;

  bool cancelled() const override;
  void phaseChanged(const String &phase, int32_t totalBytes) override;
  void bytesReceived(uint32_t bytes) override;
  void cookieReceived(const String &header, const Browser::Url &url) override;
  String cookieHeaderFor(const Browser::Url &url) const override;
  static uint64_t nowEpoch();
};
