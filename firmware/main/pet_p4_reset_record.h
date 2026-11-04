/* Private retained numeric evidence, NEVER device authority or an executable
 * package service. No pointers, task names, payloads or user data on the wire.
 * The caller owns serialization and fixed RTC storage; old/torn data is unknown.
 * Same-build/header/boot checks are integrity/correlation, not authentication. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifndef PET_P4_RESET_PHASE_TRACE
#define PET_P4_RESET_PHASE_TRACE 0
#endif
#define PET_RESET_ACTORS 4U
#define PET_RESET_RECORD_SCHEMA 1U
typedef struct {
  uint32_t sequence,boot_nonce,phase_core,cycles,checksum,commit;
} pet_reset_phase_record_t;
typedef struct {
  uint32_t magic,schema,build[8],boot_nonce,checksum,commit;
} pet_reset_record_header_t;
typedef struct {
  pet_reset_record_header_t header;
  pet_reset_phase_record_t records[PET_RESET_ACTORS][2];
} pet_reset_record_storage_t;
typedef struct {
  bool header_valid,same_build;
  uint8_t valid_mask,saturated_mask;
  pet_reset_phase_record_t records[PET_RESET_ACTORS];
} pet_reset_record_snapshot_t;
_Static_assert(sizeof(pet_reset_record_storage_t)==244,"fixed RTC evidence size");
_Static_assert(_Alignof(pet_reset_record_storage_t)>=4,"RTC words must be aligned");

#if PET_P4_RESET_PHASE_TRACE || defined(PET_RESET_PHASE_HOST_TEST)
/* Boot observes the old storage before initialize. Output/input/storage must
 * be disjoint. No physical effects except writes to this exact RTC object.
 * initialize FIRST invalidates the old header, then resets only its own words,
 * and commits the new header LAST. Any interrupted boot stays unknown.
 * record uses actor-local sequence and double banks, with commit word last;
 * caller serializes each actor and never wraps its uint32 sequence. The cycles
 * are core-local wrapping counters, NOT wall time or cross-core chronology. */
bool pet_reset_record_observe(const volatile pet_reset_record_storage_t *storage,
  const uint32_t build[8],pet_reset_record_snapshot_t *out);
bool pet_reset_record_initialize(volatile pet_reset_record_storage_t *storage,
  const uint32_t build[8],uint32_t boot_nonce);
bool pet_reset_record_write(volatile pet_reset_record_storage_t *storage,
  uint32_t actor,uint32_t boot_nonce,uint32_t sequence,uint32_t phase,
  uint32_t core,uint32_t cycles);
#endif
