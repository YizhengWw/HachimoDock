/* Fixed native boot milestones, not a health verdict or a package capability.
 * Each timestamp is elapsed from app_main; no IO/heap/timer under the mux.
 * Snapshot never restarts measurement or exports paths, content or credentials.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef PET_P4_BOOT_TRACE_ENABLE
#define PET_P4_BOOT_TRACE_ENABLE 0
#endif
typedef enum {
  PET_BOOT_STORAGE_DONE,PET_BOOT_CATALOG_DONE,PET_BOOT_BUILTINS_DONE,
  PET_BOOT_MANIFEST_DONE,PET_BOOT_HARDWARE_DONE,PET_BOOT_FIRST_FRAME,
  PET_BOOT_TRANSPORT_STARTED,PET_BOOT_IMAGE_VALID,PET_BOOT_MARK_COUNT
} pet_boot_mark_t;
typedef struct {
  uint32_t elapsed_us[PET_BOOT_MARK_COUNT];
  uint8_t mask;
  bool available,invalid_clock;
} pet_boot_trace_t;
#if PET_P4_BOOT_TRACE_ENABLE
bool pet_boot_trace_begin(void);
bool pet_boot_trace_mark(pet_boot_mark_t mark);
bool pet_boot_trace_observe(pet_boot_trace_t *out);
#else
static inline bool pet_boot_trace_begin(void){return false;}
static inline bool pet_boot_trace_mark(pet_boot_mark_t mark){(void)mark;return false;}
static inline bool pet_boot_trace_observe(pet_boot_trace_t *out){(void)out;return false;}
#endif
