#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private, opt-in diagnostic only. No package/protocol capability. Enable the
 * macro AND the five --wrap symbols listed below in a separately audited build.
 * Default builds have no wrappers, ring, task registration or observer calls.
 *
 * Supported ABI: ESP-IDF 5.5.1/5.5.4, legacy dual-core RISC-V FreeRTOS (not the
 * optional FreeRTOS-Kernel-SMP). Required linker options:
 * --wrap=spiffs_api_lock --wrap=spiffs_api_unlock
 * --wrap=xTaskPriorityInherit --wrap=xTaskPriorityDisinherit
 * --wrap=vTaskSwitchContext
 *
 * IMPORTANT COVERAGE / INTERPRETATION:
 * - GNU --wrap redirects undefined cross-object references, NOT same-TU calls.
 *   Audit the final ELF/map, including portasm, for each enabled build. This is
 *   not a complete scheduler, ISR, mutex, flash or CPU-time trace.
 * - SWITCH_SELECT means the scheduler selected a TCB, before the interrupt
 *   epilogue actually restores it. Interrupt time is not distinguished.
 * - INHERIT/DISINHERIT value is the real BaseType_t return bit-pattern, NOT an
 *   effective priority. Inherit can return true for an already boosted owner.
 *   vTaskPriorityDisinheritAfterTimeout and explicit priority changes are not
 *   wrapped. No private TCB layout or kernel-lock-taking priority getters used.
 * - FS release occurs BETWEEN RELEASE_BEGIN and RELEASE_RETURN. Preemption
 *   after the real give can make RETURN late. FS events do not identify paths,
 *   files, or different SPIFFS mounts; future multi-mount use needs a new audit.
 * - A window can start/end inside an existing wait/hold. Paired observations
 *   begun in an earlier window never complete in a new window. No replay.
 * - Registered handles must be the three existing, live, long-lived tasks and
 *   remain valid until the window stops. No task names or pointers are exported.
 * - Clock values are sampled before the observer lock. Cross-core and nested
 *   interrupt records can be appended in a different timestamp order. Sort by
 *   elapsed_us for analysis; do not infer causality from ring order alone.
 */
#ifndef PET_P4_SCHED_TRACE_ENABLE
#define PET_P4_SCHED_TRACE_ENABLE 0
#endif

#define PET_SCHED_TRACE_CAPACITY 1024U
#define PET_SCHED_TRACE_MAX_WINDOW_US 600000U

typedef enum {
    PET_SCHED_TASK_OTHER=0, PET_SCHED_TASK_MAIN=1,
    PET_SCHED_TASK_SAVE=2, PET_SCHED_TASK_PROTOCOL=3
} pet_sched_task_t;
typedef enum {
    PET_SCHED_FS_WAIT=1, PET_SCHED_FS_ACQUIRED=2,
    PET_SCHED_FS_RELEASE_BEGIN=3, PET_SCHED_FS_RELEASE_RETURN=4,
    PET_SCHED_INHERIT_RETURN=5, PET_SCHED_DISINHERIT_RETURN=6,
    PET_SCHED_SWITCH_SELECT=7, PET_SCHED_MARK=8
} pet_sched_event_kind_t;
typedef enum {
    PET_SCHED_MARK_PPA_BEGIN=1, PET_SCHED_MARK_PPA_CALLBACK=2,
    PET_SCHED_MARK_PPA_RETURN=3, PET_SCHED_MARK_SAVE_BEGIN=4,
    PET_SCHED_MARK_SAVE_END=5, PET_SCHED_MARK_PROGRAM_BEGIN=6,
    PET_SCHED_MARK_PROGRAM_END=7
} pet_sched_marker_t;
enum {
    PET_SCHED_FLAG_OVERFLOW=1U, PET_SCHED_FLAG_EXPIRED=2U,
    PET_SCHED_FLAG_BAD_CLOCK=4U, PET_SCHED_FLAG_BAD_CORE=8U,
    PET_SCHED_FLAG_PARTIAL_WRAP_COVERAGE=16U
};
typedef struct {
    uint32_t elapsed_us;
    uint32_t value; /* SDK result bit-pattern, or trusted numeric marker tag */
    uint16_t marker; /* MARK only; bounded enum above */
    uint8_t kind, core, actor, peer;
    uint8_t reserved[2];
} pet_sched_event_t;
typedef struct {
    uint32_t generation, duration_us, count, overwritten, flags;
    uint64_t started_us, ended_us;
    bool active;
} pet_sched_trace_status_t;
/* Independent of the lossy raw ring. Only registered long-lived actors are
 * paired: OTHER may represent several tasks and must never be paired together.
 * Durations are wall time, not CPU time; holdBeforeRelease ends BEFORE the
 * actual give, release spans BEGIN..RETURN (the give is somewhere inside it).
 * phase!=0 at window end is incomplete, not an invented completed interval.
 * Counts of zero return bits do NOT mean no mutex/priority operation occurred. */
typedef struct {
    uint64_t wait_total_us,hold_before_release_total_us,release_total_us;
    uint32_t wait_pairs,hold_pairs,release_pairs;
    uint32_t wait_max_us,hold_before_release_max_us,release_max_us;
    uint32_t unpaired,inherit_zero,inherit_nonzero,disinherit_zero,disinherit_nonzero;
    uint8_t phase;
} pet_sched_actor_summary_t;

#if PET_P4_SCHED_TRACE_ENABLE
/* Trusted task-context setup only. No mutation is allowed during a window.
 * Handles are opaque and compared only, never dereferenced by this module. */
bool pet_sched_trace_register(pet_sched_task_t task, void *handle);
/* Returns a nonzero generation, or 0 on missing registration, active window,
 * invalid duration/clock, or exhausted generation. No old window is restarted. */
uint32_t pet_sched_trace_begin(uint32_t duration_us);
bool pet_sched_trace_stop(uint32_t generation);
/* Safe in task/ISR context. No I/O. Does not start or extend a window. */
void pet_sched_trace_mark(pet_sched_marker_t marker, uint32_t tag);
/* Status is a small copy. Read is append order, oldest retained
 * first, only after stop/expiry. No kernel/log/FS APIs run under the trace lock.
 * Caller outputs must be internal, live RAM (normally the trusted task's
 * internal stack), never a user buffer or memory-mapped/external resource.
 * An elapsed window is closed by the next hook/mark/stop, not by a new timer. */
bool pet_sched_trace_status(uint32_t generation, pet_sched_trace_status_t *out);
bool pet_sched_trace_read(uint32_t generation, uint32_t index, pet_sched_event_t *out);
/* After stop only, one bounded numeric copy. Stale/active/OTHER rejection leaves
 * caller output untouched. Summary availability never clears raw overflow. */
bool pet_sched_trace_summary(uint32_t generation,pet_sched_task_t actor,pet_sched_actor_summary_t *out);
#else
static inline bool pet_sched_trace_register(pet_sched_task_t t,void *h){(void)t;(void)h;return false;}
static inline uint32_t pet_sched_trace_begin(uint32_t us){(void)us;return 0;}
static inline bool pet_sched_trace_stop(uint32_t g){(void)g;return false;}
static inline void pet_sched_trace_mark(pet_sched_marker_t m,uint32_t t){(void)m;(void)t;}
static inline bool pet_sched_trace_status(uint32_t g,pet_sched_trace_status_t *o){(void)g;(void)o;return false;}
static inline bool pet_sched_trace_read(uint32_t g,uint32_t i,pet_sched_event_t *o){(void)g;(void)i;(void)o;return false;}
static inline bool pet_sched_trace_summary(uint32_t g,pet_sched_task_t t,pet_sched_actor_summary_t *o){(void)g;(void)t;(void)o;return false;}
#endif
