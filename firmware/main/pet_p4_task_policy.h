/* Control/input must preempt best-effort animation. Audio priorities are unchanged.
 * Numeric policy only: no task handles, ownership grants or scheduler callbacks. */
#pragma once
#include <stdbool.h>

#define PET_P4_PROTOCOL_TASK_PRIORITY 11U
#define PET_P4_TRANSPORT_TASK_PRIORITY 10U
#define PET_P4_INPUT_TASK_PRIORITY 9U
/* Only the protocol owner's unlocked filesystem phase. Share the established
 * main/save-worker base; never outrank UI, RX/control or audio. Idle-only work
 * was measured to starve during continuous animation, so is not acceptable. */
#define PET_P4_STORAGE_IO_TASK_PRIORITY 1U
/* Private: create the protocol worker and the save worker pinned to the second
 * core so their unlocked filesystem phases never time-slice the UI core with the
 * renderer. Priorities, lanes, ownership and every acceptance gate are unchanged;
 * this isolates storage work, it does not boost rendering. Default off. */
#ifndef PET_P4_STORAGE_WORKERS_CORE1
#define PET_P4_STORAGE_WORKERS_CORE1 0
#endif
#define PET_P4_STORAGE_WORKER_CORE 1
/* Same arguments as xTaskCreate; only the private candidate pins the worker. */
#if PET_P4_STORAGE_WORKERS_CORE1
#define pet_p4_storage_worker_create(fn,name,stack,arg,priority,handle) \
  xTaskCreatePinnedToCore(fn,name,stack,arg,priority,handle,PET_P4_STORAGE_WORKER_CORE)
#else
#define pet_p4_storage_worker_create(fn,name,stack,arg,priority,handle) \
  xTaskCreate(fn,name,stack,arg,priority,handle)
#endif
/* Isolated A/B reference image only. Never combine it with guest execution or
 * present its deliberately old control ordering as the accepted UI firmware. */
#if defined(PET_P4_RENDER_SCHEDULING_REFERENCE) && PET_P4_RENDER_SCHEDULING_REFERENCE
#if defined(PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED) && PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED
#error "Render scheduling reference cannot enable component program execution"
#endif
#ifndef PET_P4_RENDER_REFERENCE_PRIORITY
#define PET_P4_RENDER_REFERENCE_PRIORITY 16U
#endif
#if PET_P4_RENDER_REFERENCE_PRIORITY != 9 && PET_P4_RENDER_REFERENCE_PRIORITY != 16
#error "Render timing reference accepts only reviewed comparison priorities"
#endif
#define PET_P4_H264_TASK_PRIORITY PET_P4_RENDER_REFERENCE_PRIORITY
#else
#define PET_P4_H264_TASK_PRIORITY 8U
#endif
/* The SDK secondary decoder is pinned to core 1. Keep its established codec
 * scheduling for the next controlled throughput comparison; primary/pack on
 * the main path remain below input/control. Audio is still priority 18. */
#define PET_P4_H264_SECONDARY_TASK_PRIORITY 17U

/* uxTaskPriorityGet reports effective priority. Restoring an inherited value
 * with vTaskPrioritySet would incorrectly promote the task's base priority.
 * Only boost the renderer from its known native main-task base priority. */
static inline bool pet_p4_render_can_boost(unsigned current, unsigned main_base) {
  return current == main_base && main_base < PET_P4_H264_TASK_PRIORITY;
}
