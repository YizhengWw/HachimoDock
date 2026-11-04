/* Native cooperative wall-budget policy. No ambient clock or resource grant.
 * Caller creates one private policy per scheduler turn; never repeatedly
 * reinitialize/drain it within a turn. Not hard preemption or a WCET proof. */
#pragma once
#include "pet_p4_program_core.h"
typedef int64_t (*pet_program_clock_fn)(void *context);
typedef struct {
  pet_program_clock_fn clock;
  void *context;
  int64_t started,last;
  uint32_t budget_us;
  bool yielded,failed;
} pet_program_deadline_t;
static inline bool pet_program_deadline_begin(pet_program_deadline_t *out,
    pet_program_clock_fn clock,void *context,uint32_t budget_us){
  if(!out||!clock||!budget_us||budget_us>2000)return false;
  int64_t now=clock(context);if(now<0)return false;
  *out=(pet_program_deadline_t){.clock=clock,.context=context,.started=now,.last=now,.budget_us=budget_us};return true;
}
static inline pet_program_checkpoint_result_t pet_program_deadline_check(void *context){
  pet_program_deadline_t *d=context;
  if(!d||!d->clock||!d->budget_us||d->budget_us>2000||d->started<0||d->last<d->started||d->failed)return PET_PROGRAM_CANCEL_SLICE;
  if(d->yielded)return PET_PROGRAM_YIELD;
  int64_t now=d->clock(d->context);
  if(now<0||now<d->last){d->failed=true;return PET_PROGRAM_CANCEL_SLICE;}
  d->last=now;
  if((uint64_t)(now-d->started)>=d->budget_us){d->yielded=true;return PET_PROGRAM_YIELD;}
  return PET_PROGRAM_CONTINUE;
}
