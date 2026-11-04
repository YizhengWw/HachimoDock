#pragma once
#include "pet_p4_sched_trace.h"

#ifndef PET_P4_SCHED_PROBE_ENABLE
#define PET_P4_SCHED_PROBE_ENABLE 0
#endif
#define PET_SCHED_PROBE_ARM_US UINT64_C(35000000)
#define PET_SCHED_PROBE_FROZEN_US UINT64_C(30000000)
#define PET_SCHED_PROBE_PAGE_EVENTS 16U

/* Trusted native C only. Never accept owner/ticket values from package code or
 * decode them from a wire payload. A future transport adapter owns these tokens
 * and must revoke on teardown and recheck its transport epoch before delivery.
 * This file adds no protocol, worker, timer, audio action or installation cap.
 * All APIs except ppa_mark are TASK context only, outside state/trace/broker
 * locks. Only one control operation can proceed; contention returns BUSY.
 * poll must be called by a future bounded trusted service; without that wiring
 * arm/frozen expiry and external audio detection are not autonomous guarantees.
 */
typedef struct { uint64_t generation; } pet_sched_probe_owner_t;
typedef struct { uint64_t owner, revision; } pet_sched_probe_ticket_t;
typedef struct { pet_sched_probe_ticket_t ticket; uint32_t trace_generation; } pet_sched_probe_save_t;
typedef enum {
    PET_SCHED_PROBE_OK, PET_SCHED_PROBE_DISABLED, PET_SCHED_PROBE_BUSY,
    PET_SCHED_PROBE_INVALID, PET_SCHED_PROBE_STALE, PET_SCHED_PROBE_AUDIO_BUSY,
    PET_SCHED_PROBE_RESOURCE_BUSY, PET_SCHED_PROBE_NOT_READY, PET_SCHED_PROBE_FAULT
} pet_sched_probe_result_t;
typedef enum {
    PET_SCHED_PROBE_IDLE, PET_SCHED_PROBE_ARMED, PET_SCHED_PROBE_CAPTURING,
    PET_SCHED_PROBE_FROZEN, PET_SCHED_PROBE_DRAINING, PET_SCHED_PROBE_FAILED
} pet_sched_probe_state_t;
typedef enum {
    PET_SCHED_PROBE_NONE, PET_SCHED_PROBE_COMPLETE, PET_SCHED_PROBE_WINDOW_END,
    PET_SCHED_PROBE_ARM_TIMEOUT, PET_SCHED_PROBE_AUDIO, PET_SCHED_PROBE_CANCELLED,
    PET_SCHED_PROBE_OWNER_LOST, PET_SCHED_PROBE_CLOCK, PET_SCHED_PROBE_TRACE_ERROR,
    PET_SCHED_PROBE_RESOURCE_LOST, PET_SCHED_PROBE_EXPIRED,
    PET_SCHED_PROBE_DISCARDED, PET_SCHED_PROBE_FINISH_FAILED
} pet_sched_probe_reason_t;
typedef struct {
    pet_sched_probe_ticket_t ticket;
    pet_sched_probe_state_t state;
    pet_sched_probe_reason_t reason;
    pet_sched_trace_status_t trace;
    uint64_t save_completed_us;
    bool save_inflight, save_ok, marker_contended;
} pet_sched_probe_status_t;
typedef struct {
    pet_sched_probe_status_t status;
    uint32_t index, count, next_index;
    bool done;
    pet_sched_event_t events[PET_SCHED_PROBE_PAGE_EVENTS];
} pet_sched_probe_page_t;

#if PET_P4_SCHED_PROBE_ENABLE
/* Existing long-lived task handles only, once, before creating an owner. */
pet_sched_probe_result_t pet_sched_probe_init(void *main_task,void *save_task,void *protocol_task);
pet_sched_probe_result_t pet_sched_probe_owner_open(pet_sched_probe_owner_t *out);
/* Nonblocking revocation latch even if another control operation is busy.
 * Authority is lost immediately; physical cleanup may require poll/save_end. */
pet_sched_probe_result_t pet_sched_probe_owner_revoke(pet_sched_probe_owner_t owner);
pet_sched_probe_result_t pet_sched_probe_status(pet_sched_probe_owner_t owner,pet_sched_probe_status_t *out);
/* expected comes from the same owner's IDLE status. Success consumes that
 * revision and returns a new immutable capture ticket. No replay or overwrite. */
pet_sched_probe_result_t pet_sched_probe_arm(pet_sched_probe_ticket_t expected,uint32_t window_us,pet_sched_probe_ticket_t *out);
pet_sched_probe_result_t pet_sched_probe_cancel(pet_sched_probe_ticket_t ticket);
pet_sched_probe_result_t pet_sched_probe_discard(pet_sched_probe_ticket_t ticket);
pet_sched_probe_result_t pet_sched_probe_read(pet_sched_probe_ticket_t ticket,uint32_t index,pet_sched_probe_page_t *out);
pet_sched_probe_result_t pet_sched_probe_poll(void);
/* Call only at the next already-eligible save, before its original I/O, and
 * always pair a nonzero token with save_end AFTER that I/O returns. This is
 * observation, never permission to start/stop/retry the underlying save. A
 * cancellation during begin may still return a token needed for physical drain.
 * save_end latches completion even under claim contention; do not replay save. */
pet_sched_probe_save_t pet_sched_probe_save_begin(uint32_t diagnostic_sequence);
pet_sched_probe_result_t pet_sched_probe_save_end(pet_sched_probe_save_t token,bool ok);
/* ISR-safe numeric PPA markers only. No clock/audio/broker/output under the
 * adapter lock, and no user callbacks. Contended markers are dropped+flagged. */
void pet_sched_probe_ppa_mark(pet_sched_marker_t marker,uint32_t cookie);
#else
static inline pet_sched_probe_result_t pet_sched_probe_init(void*a,void*b,void*c){(void)a;(void)b;(void)c;return PET_SCHED_PROBE_DISABLED;}
static inline pet_sched_probe_result_t pet_sched_probe_owner_open(pet_sched_probe_owner_t*o){(void)o;return PET_SCHED_PROBE_DISABLED;}
static inline pet_sched_probe_result_t pet_sched_probe_owner_revoke(pet_sched_probe_owner_t o){(void)o;return PET_SCHED_PROBE_DISABLED;}
static inline pet_sched_probe_result_t pet_sched_probe_status(pet_sched_probe_owner_t o,pet_sched_probe_status_t*s){(void)o;(void)s;return PET_SCHED_PROBE_DISABLED;}
static inline pet_sched_probe_result_t pet_sched_probe_arm(pet_sched_probe_ticket_t t,uint32_t u,pet_sched_probe_ticket_t*o){(void)t;(void)u;(void)o;return PET_SCHED_PROBE_DISABLED;}
static inline pet_sched_probe_result_t pet_sched_probe_cancel(pet_sched_probe_ticket_t t){(void)t;return PET_SCHED_PROBE_DISABLED;}
static inline pet_sched_probe_result_t pet_sched_probe_discard(pet_sched_probe_ticket_t t){(void)t;return PET_SCHED_PROBE_DISABLED;}
static inline pet_sched_probe_result_t pet_sched_probe_read(pet_sched_probe_ticket_t t,uint32_t i,pet_sched_probe_page_t*o){(void)t;(void)i;(void)o;return PET_SCHED_PROBE_DISABLED;}
static inline pet_sched_probe_result_t pet_sched_probe_poll(void){return PET_SCHED_PROBE_DISABLED;}
static inline pet_sched_probe_save_t pet_sched_probe_save_begin(uint32_t s){(void)s;return (pet_sched_probe_save_t){0};}
static inline pet_sched_probe_result_t pet_sched_probe_save_end(pet_sched_probe_save_t t,bool o){(void)t;(void)o;return PET_SCHED_PROBE_DISABLED;}
static inline void pet_sched_probe_ppa_mark(pet_sched_marker_t m,uint32_t c){(void)m;(void)c;}
#endif
