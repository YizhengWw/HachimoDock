/* Immutable query replies may outlive the state lock, never their dispatch.
 * Private comparison only: no new queue/task/copy and no mutation ACK replay. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef PET_P4_QUERY_REPLY_DEFERRED
#define PET_P4_QUERY_REPLY_DEFERRED 0
#endif
#ifndef PET_P4_QUERY_REPLY_TRACE
#define PET_P4_QUERY_REPLY_TRACE 0
#endif
/* Private: the install ACK line (widget-install-ack) of the request being
 * dispatched is handed back to the dispatcher and transmitted after the state
 * lock is released, exactly like deferred diagnostics. One line per request;
 * any other line flushes it first, so wire order is unchanged. */
#ifndef PET_P4_MUTATION_ACK_DEFERRED
#define PET_P4_MUTATION_ACK_DEFERRED 0
#endif
/* Private: the two large single-line replies of a dispatched request (the
 * widget/inventory reply to widget/list and the hello reply to bind/ack,
 * ~5.4-6.5 KB each, ~50-60 ms on the paced UART) use the same held slot, so
 * their transmission no longer runs under the state lock. Unsolicited hello
 * lines from the main task have no slot and are unchanged. */
#ifndef PET_P4_LARGE_REPLY_DEFERRED
#define PET_P4_LARGE_REPLY_DEFERRED 0
#endif
#if PET_P4_LARGE_REPLY_DEFERRED && !PET_P4_MUTATION_ACK_DEFERRED
#error "Large reply deferral requires the deferred install ACK parent"
#endif
#ifndef PET_P4_HELLO_ASYNC
#define PET_P4_HELLO_ASYNC 0
#endif
#ifndef PET_P4_QUERY_OBSERVATION
#define PET_P4_QUERY_OBSERVATION 0
#endif
#if PET_P4_QUERY_OBSERVATION && !PET_P4_QUERY_REPLY_TRACE
#error "Query observation requires the reviewed query timing parent"
#endif
#if PET_P4_QUERY_OBSERVATION
enum {PET_QUERY_ORDINARY,PET_QUERY_DEBUG_NATIVE,PET_QUERY_HELLO_NATIVE,PET_QUERY_ACTIVATION_NATIVE};
/* Native numeric interval only. The caller serializes updates/snapshots;
 * endpoint overlap is observation, never proof of exclusive CPU attribution. */
typedef struct {
  uint64_t sequence,begin_us,unlocked_us,end_us;
  bool active,invalid_clock,unlocked;
  uint8_t native_kind;
} pet_p4_query_observation_t;
static inline void pet_p4_query_observation_begin(pet_p4_query_observation_t *o,uint64_t now){
  o->invalid_clock|=o->active||now>INT64_MAX||(o->sequence&&now<o->end_us);
  if(o->sequence!=UINT64_MAX)o->sequence++;else o->invalid_clock=true;
  o->begin_us=now;o->unlocked_us=o->end_us=0;o->active=true;o->unlocked=false;o->native_kind=PET_QUERY_ORDINARY;
}
static inline void pet_p4_query_observation_unlock(pet_p4_query_observation_t *o,uint64_t now){
  o->invalid_clock|=!o->active||o->unlocked||now<o->begin_us||now>INT64_MAX;o->unlocked_us=now;o->unlocked=true;
}
static inline void pet_p4_query_observation_end(pet_p4_query_observation_t *o,uint64_t now){
  o->invalid_clock|=!o->active||!o->unlocked||now<o->unlocked_us||now>INT64_MAX;o->end_us=now;o->active=false;
}
bool pet_p4_protocol_query_observation(pet_p4_query_observation_t *out);
#endif

typedef struct {
  /* cJSON-owned serialization; caller must send once AFTER releasing the
   * state lock, then cJSON_free it. NULL means no detached response. */
  char *line;
  bool diagnostic_query;
} pet_p4_query_reply_t;

#if PET_P4_QUERY_REPLY_TRACE
/* Previous COMPLETED diagnostic query only. No IDs, topics or payload text.
 * Its single protocol worker is also the sole observer; other tasks get no
 * evidence. No global critical section or new synchronisation is required. */
typedef struct {
  uint64_t sequence, begin_us, end_us;
  uint32_t lock_wait_us, state_held_us, post_unlock_tx_us;
  uint32_t max_lock_wait_us, max_state_held_us, max_post_unlock_tx_us;
  bool detached, invalid_clock;
} pet_p4_query_timing_t;
static inline uint32_t pet_p4_query_span(uint64_t begin, uint64_t end) {
  uint64_t n = end >= begin ? end - begin : 0;
  return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}
static inline void pet_p4_query_timing_record(pet_p4_query_timing_t *m,
  uint64_t begin, uint64_t locked, uint64_t unlocked, uint64_t end, bool detached) {
  if (m->sequence != UINT64_MAX) m->sequence++;
  m->invalid_clock |= locked < begin || unlocked < locked || end < unlocked;
  m->begin_us=begin; m->end_us=end; m->detached=detached;
  m->lock_wait_us=pet_p4_query_span(begin,locked);
  m->state_held_us=pet_p4_query_span(locked,unlocked);
  m->post_unlock_tx_us=pet_p4_query_span(unlocked,end);
  if(m->lock_wait_us>m->max_lock_wait_us)m->max_lock_wait_us=m->lock_wait_us;
  if(m->state_held_us>m->max_state_held_us)m->max_state_held_us=m->state_held_us;
  if(m->post_unlock_tx_us>m->max_post_unlock_tx_us)m->max_post_unlock_tx_us=m->post_unlock_tx_us;
}
bool pet_p4_protocol_query_timing(pet_p4_query_timing_t *out);
#endif
