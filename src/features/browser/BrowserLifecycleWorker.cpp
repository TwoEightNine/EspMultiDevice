#include "BrowserLifecycleWorker.h"

BrowserLifecycleWorker::BrowserLifecycleWorker(BrowserLifecycleDelegate &delegate)
    : _delegate(delegate),
      _jobs(nullptr),
      _task(nullptr),
      _loading(false),
      _drawPending(false),
      _generation(0),
      _activeGeneration(0) {}

bool BrowserLifecycleWorker::begin(BaseType_t core) {
  if (_task) return true;
  if (!_jobs) _jobs = xQueueCreate(BrowserLimits::LifecycleJobQueueLength, sizeof(Job));
  if (!_jobs) return false;
  return xTaskCreatePinnedToCore(taskMain, "browser-startup", 6144, this,
                                 tskIDLE_PRIORITY, &_task, core) == pdPASS;
}

bool BrowserLifecycleWorker::end() {
  cancel();
  if (_activeGeneration != 0) return false;
  if (_task && eTaskGetState(_task) != eBlocked) return false;
  if (_task) {
    vTaskDelete(_task);
    _task = nullptr;
  }
  if (_jobs) {
    vQueueDelete(_jobs);
    _jobs = nullptr;
  }
  return true;
}

bool BrowserLifecycleWorker::schedule() {
  ++_generation;
  if (_generation == 0) _generation = 1;
  _loading = true;
  _drawPending = false;
  const Job job{_generation};
  if (!_jobs || xQueueSend(_jobs, &job, 0) != pdTRUE) {
    _loading = false;
    return false;
  }
  return true;
}

void BrowserLifecycleWorker::cancel() {
  ++_generation;
  if (_generation == 0) _generation = 1;
  _loading = false;
  _drawPending = false;
}

bool BrowserLifecycleWorker::takeDrawPending() {
  if (!_drawPending) return false;
  _drawPending = false;
  return true;
}

void BrowserLifecycleWorker::taskMain(void *context) {
  BrowserLifecycleWorker *worker = static_cast<BrowserLifecycleWorker *>(context);
  Job job{};
  while (true) {
    if (xQueueReceive(worker->_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
    if (!worker->isCurrent(job.generation)) continue;
    worker->_activeGeneration = job.generation;
    worker->_delegate.runBrowserLifecycle(job.generation);
    worker->_activeGeneration = 0;
    if (!worker->isCurrent(job.generation)) continue;
    worker->_loading = false;
    worker->_drawPending = true;
  }
}
