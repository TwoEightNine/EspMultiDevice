#pragma once

#include <Arduino.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "BrowserLimits.h"

class BrowserLifecycleDelegate {
public:
  virtual ~BrowserLifecycleDelegate() = default;
  virtual void runBrowserLifecycle(uint32_t generation) = 0;
};

// Owns the bounded startup-work queue and its worker task. The delegate owns
// browser-specific SD/state operations; this component owns scheduling,
// generation invalidation, and UI-notification state.
class BrowserLifecycleWorker {
public:
  explicit BrowserLifecycleWorker(BrowserLifecycleDelegate &delegate);

  bool begin(BaseType_t core);
  bool schedule();
  void cancel();
  bool loading() const { return _loading; }
  uint32_t generation() const { return _generation; }
  bool takeDrawPending();
  bool isCurrent(uint32_t generation) const { return generation == _generation; }

private:
  struct Job { uint32_t generation; };

  BrowserLifecycleDelegate &_delegate;
  QueueHandle_t _jobs;
  TaskHandle_t _task;
  volatile bool _loading;
  volatile bool _drawPending;
  volatile uint32_t _generation;

  static void taskMain(void *context);
};
