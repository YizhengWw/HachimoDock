/* Numeric PPA wall-time diagnostics. Caller serializes every access with a
 * short ISR-safe lock. No allocation, waiting, I/O, or buffer ownership here.
 * The SDK callback observes completion after ISR housekeeping, not the instant
 * the hardware finishes. A valid split must never be called pure DMA time. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#ifndef PET_P4_PPA_QUEUE_PROBE
#define PET_P4_PPA_QUEUE_PROBE 0
#endif

typedef enum {
  PET_PPA_TRACE_SCALE=0, PET_PPA_TRACE_CONVERT=1, PET_PPA_TRACE_ROTATE=2,
  PET_PPA_TRACE_OP_COUNT=3
} pet_ppa_trace_op_t;
enum {
  PET_PPA_TRACE_ATTEMPTED=1U<<0,
  PET_PPA_TRACE_RETURNED=1U<<1,
  PET_PPA_TRACE_CALLBACK_SEEN=1U<<2,
  PET_PPA_TRACE_VALID_SPLIT=1U<<3,
  PET_PPA_TRACE_LATE=1U<<4,
  PET_PPA_TRACE_MISSING=1U<<5,
  PET_PPA_TRACE_ERROR=1U<<6,
  PET_PPA_TRACE_UNAVAILABLE=1U<<7,
  PET_PPA_TRACE_EXHAUSTED=1U<<8,
  PET_PPA_TRACE_CLOCK_INVALID=1U<<9,
  PET_PPA_TRACE_REPEATED=1U<<10,
  PET_PPA_TRACE_QUEUE_SEEN=1U<<11,
  PET_PPA_TRACE_QUEUE_REPEATED=1U<<12,
  PET_PPA_TRACE_QUEUE_VALID=1U<<13,
  PET_PPA_TRACE_QUEUE_INVALID=1U<<14
};
typedef struct {
  uint32_t api_us, call_to_callback_us, callback_to_return_us, flags;
#if PET_P4_PPA_QUEUE_PROBE
  uint32_t prepare_us, queue_to_callback_us;
#endif
} pet_ppa_trace_sample_t;
typedef struct { pet_ppa_trace_sample_t op[PET_PPA_TRACE_OP_COUNT]; } pet_ppa_trace_frame_t;
typedef struct {
  uint64_t start_us, callback_us, return_us;
  uint32_t cookie, flags;
#if PET_P4_PPA_QUEUE_PROBE
  uint64_t queue_us;
#endif
} pet_ppa_trace_record_t;
typedef struct {
  pet_ppa_trace_record_t op[PET_PPA_TRACE_OP_COUNT];
  uint32_t last_cookie;
  bool available;
} pet_ppa_trace_state_t;

static inline void pet_ppa_trace_set_available(pet_ppa_trace_state_t *state,bool available) {
  state->available=available;
}
static inline void pet_ppa_trace_frame_begin(pet_ppa_trace_state_t *state) {
  /* Do not reset last_cookie: late callbacks from an old frame cannot revive. */
  memset(state->op,0,sizeof(state->op));
}
static inline uint32_t pet_ppa_trace_begin(pet_ppa_trace_state_t *state,pet_ppa_trace_op_t kind,uint64_t now_us) {
  if((unsigned)kind>=PET_PPA_TRACE_OP_COUNT)return 0;
  pet_ppa_trace_record_t *record=&state->op[kind];
  if(record->flags&PET_PPA_TRACE_ATTEMPTED){
    /* The renderer has at most one of each operation per frame. If that ever
     * changes, report unknown instead of combining unrelated transaction times. */
    memset(record,0,sizeof(*record));
    record->flags=PET_PPA_TRACE_ATTEMPTED|PET_PPA_TRACE_REPEATED;
    return 0;
  }
  record->flags=PET_PPA_TRACE_ATTEMPTED;
  if(!state->available){record->flags|=PET_PPA_TRACE_UNAVAILABLE;return 0;}
  if(state->last_cookie==UINT32_MAX){record->flags|=PET_PPA_TRACE_EXHAUSTED;return 0;}
  record->start_us=now_us;
  record->cookie=++state->last_cookie;
  return record->cookie;
}
/* Always inline into the adapter's IRAM callback; this path calls no helpers
 * whose code or data placement could introduce a flash dependency. */
static inline __attribute__((always_inline)) bool pet_ppa_trace_callback(
  pet_ppa_trace_state_t *state,uint32_t cookie,uint64_t now_us
) {
  if(!cookie)return false;
  for(unsigned i=0;i<PET_PPA_TRACE_OP_COUNT;i++){
    pet_ppa_trace_record_t *record=&state->op[i];
    if(record->cookie!=cookie)continue;
    if(record->flags&PET_PPA_TRACE_CALLBACK_SEEN)return false;
    record->callback_us=now_us;
    record->flags|=PET_PPA_TRACE_CALLBACK_SEEN;
    return true;
  }
  return false;
}
static inline bool pet_ppa_trace_return(pet_ppa_trace_state_t *state,uint32_t cookie,bool ok,uint64_t now_us) {
  if(!cookie)return false;
  for(unsigned i=0;i<PET_PPA_TRACE_OP_COUNT;i++){
    pet_ppa_trace_record_t *record=&state->op[i];
    if(record->cookie!=cookie)continue;
    if(record->flags&PET_PPA_TRACE_RETURNED)return false;
    record->return_us=now_us;
    record->flags|=PET_PPA_TRACE_RETURNED|(ok?0:PET_PPA_TRACE_ERROR);
    return true;
  }
  return false;
}
static inline uint32_t pet_ppa_trace_duration(uint64_t start,uint64_t end) {
  uint64_t duration=end-start;
  return duration>UINT32_MAX?UINT32_MAX:(uint32_t)duration;
}
#if PET_P4_PPA_QUEUE_PROBE
/* Original SDK entry after synchronous cache/descriptor preparation. No claim
 * of pure DMA time: the remaining interval still includes queues and ISR work. */
static inline bool pet_ppa_trace_queue(pet_ppa_trace_state_t *state,uint32_t cookie,uint64_t now_us){
  if(!cookie)return false;
  for(unsigned i=0;i<PET_PPA_TRACE_OP_COUNT;i++){
    pet_ppa_trace_record_t *record=&state->op[i];
    if(record->cookie!=cookie)continue;
    if(record->flags&PET_PPA_TRACE_QUEUE_SEEN){record->flags|=PET_PPA_TRACE_QUEUE_REPEATED;return false;}
    record->queue_us=now_us;record->flags|=PET_PPA_TRACE_QUEUE_SEEN;return true;
  }
  return false;
}
#endif
static inline void pet_ppa_trace_snapshot(const pet_ppa_trace_state_t *state,pet_ppa_trace_frame_t *out) {
  memset(out,0,sizeof(*out));
  for(unsigned i=0;i<PET_PPA_TRACE_OP_COUNT;i++){
    const pet_ppa_trace_record_t *record=&state->op[i];
    pet_ppa_trace_sample_t *sample=&out->op[i];
    sample->flags=record->flags;
    if(!record->cookie)continue;
    bool returned=(record->flags&PET_PPA_TRACE_RETURNED)!=0;
    bool seen=(record->flags&PET_PPA_TRACE_CALLBACK_SEEN)!=0;
    if((returned&&record->return_us<record->start_us)||(seen&&record->callback_us<record->start_us)){
      sample->flags|=PET_PPA_TRACE_CLOCK_INVALID;continue;
    }
    if(returned)sample->api_us=pet_ppa_trace_duration(record->start_us,record->return_us);
    if(returned&&!seen){sample->flags|=PET_PPA_TRACE_MISSING;continue;}
    if(!returned||!seen)continue;
    if(record->callback_us>record->return_us){sample->flags|=PET_PPA_TRACE_LATE;continue;}
    if(record->flags&PET_PPA_TRACE_ERROR)continue;
    sample->call_to_callback_us=pet_ppa_trace_duration(record->start_us,record->callback_us);
    sample->callback_to_return_us=pet_ppa_trace_duration(record->callback_us,record->return_us);
    sample->flags|=PET_PPA_TRACE_VALID_SPLIT;
#if PET_P4_PPA_QUEUE_PROBE
    if(record->flags&PET_PPA_TRACE_QUEUE_SEEN){
      if((record->flags&PET_PPA_TRACE_QUEUE_REPEATED)||record->queue_us<record->start_us||
         record->queue_us>record->callback_us||record->return_us-record->start_us>UINT32_MAX){
        sample->flags|=PET_PPA_TRACE_QUEUE_INVALID;
      }else{
        sample->prepare_us=(uint32_t)(record->queue_us-record->start_us);
        sample->queue_to_callback_us=(uint32_t)(record->callback_us-record->queue_us);
        sample->flags|=PET_PPA_TRACE_QUEUE_VALID;
      }
    }
#endif
  }
}
