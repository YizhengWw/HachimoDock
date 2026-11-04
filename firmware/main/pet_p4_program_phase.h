#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#ifndef PET_PROGRAM_PHASE_TRACE
#define PET_PROGRAM_PHASE_TRACE 0
#endif
/* Native, numeric wall observations only. No owners, events or guest data.
 * Phase totals are nested inside the unchanged aggregate parent wall sample.
 * Invalid clocks are retained rather than subtracted or accepted as fast. */
typedef struct {
  uint64_t start_us,end_us;
  uint32_t scheduler_us,collect_us,capture_us,parent_us,registry_us;
  uint8_t collected,invalid_clock;
} pet_program_phase_t;
static inline void pet_program_phase_add(pet_program_phase_t *p,uint32_t *field,int64_t begin,int64_t end){
  if(begin<0||end<begin){if(p->invalid_clock!=UINT8_MAX)p->invalid_clock++;return;}
  uint64_t elapsed=(uint64_t)(end-begin);
  *field=elapsed>UINT32_MAX-*field?UINT32_MAX:*field+(uint32_t)elapsed;
}
