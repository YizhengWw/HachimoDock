/* Private candidate policy, not a component grant or a task-priority change. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PET_APP_SAVE_TASK_STACK_BYTES 4096U
#ifndef PET_APP_SAVE_OWNED_PENDING_ENABLED
#define PET_APP_SAVE_OWNED_PENDING_ENABLED 0
#endif
#ifndef PET_P4_SAVE_TRACE_DETACHED
#define PET_P4_SAVE_TRACE_DETACHED 0
#endif
#if PET_P4_SAVE_TRACE_DETACHED && (!PET_APP_SAVE_OWNED_PENDING_ENABLED || !PET_P4_RESET_PHASE_TRACE)
#error Detached SAVE trace requires the owned pending and native trace parent
#endif
#if PET_APP_SAVE_OWNED_PENDING_ENABLED
typedef struct {
  uint32_t observations,minimum_free_bytes,invalid_samples;
  bool available;
} pet_app_save_stack_t;
/* Cached numeric observation only. Never examines another task's stack or
 * exports an address, handle, save content or stack word. */
bool pet_app_store_worker_stack(pet_app_save_stack_t *out);
#endif
