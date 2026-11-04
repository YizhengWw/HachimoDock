/* Private, default-off numeric native exception evidence. Not a crash dump,
 * reset authority, recovery mechanism or replacement for the SDK handler. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef PET_P4_NATIVE_PANIC_TRACE
#define PET_P4_NATIVE_PANIC_TRACE 0
#endif
#if PET_P4_NATIVE_PANIC_TRACE
typedef struct {
  uint32_t pc,return_pc,cause,core,exception,flags;
} pet_native_panic_site_t;
typedef struct {
  bool same_build;
  uint8_t valid_mask;
  pet_native_panic_site_t sites[2];
} pet_native_panic_snapshot_t;
void pet_native_panic_boot(void);
bool pet_native_panic_previous(pet_native_panic_snapshot_t *out);
#endif
