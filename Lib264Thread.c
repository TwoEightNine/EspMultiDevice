// lib264 uses the ithread ABI for container bookkeeping even in its
// single-core mode. Video conversion runs synchronously, so no task creation
// or blocking synchronisation is needed on ESP32-S3.
#include <stdint.h>

#include "third_party/lib264/libthread/include/ithread.h"

uint32_t ithread_get_handle_size(void) { return 1; }
uint32_t ithread_get_mutex_lock_size(void) { return 1; }
int32_t ithread_create(void *, void *, ThreadFxn, void *) { return -1; }
void ithread_exit(void *) {}
int32_t ithread_join(void *, void **) { return 0; }
int32_t ithread_get_mutex_struct_size(void) { return 1; }
int32_t ithread_mutex_init(void *) { return 0; }
int32_t ithread_mutex_destroy(void *) { return 0; }
int32_t ithread_mutex_lock(void *) { return 0; }
int32_t ithread_mutex_unlock(void *) { return 0; }
void ithread_yield(void) {}
void ithread_sleep(uint32_t) {}
void ithread_msleep(uint32_t) {}
void ithread_usleep(uint32_t) {}
uint32_t ithread_get_sem_struct_size(void) { return 1; }
int32_t ithread_sem_init(void *, int32_t, uint32_t) { return 0; }
int32_t ithread_sem_post(void *) { return 0; }
int32_t ithread_sem_wait(void *) { return 0; }
int32_t ithread_sem_destroy(void *) { return 0; }
int32_t ithread_set_affinity(int32_t) { return 0; }
void ithread_set_name(char *) {}
