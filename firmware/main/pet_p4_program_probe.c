#include "pet_p4_program_probe.h"
#include "pet_p4_program_vectors.h"
#include "pet_p4_program_deadline.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_audio.h"
#include "pet_p4_sched_trace.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define PROBE_SAMPLES 8U
#define PROBE_INTERVAL_US INT64_C(5000000)
typedef struct {
  pet_resource_owner_t owner;
  pet_resource_lease_t lease;
  bool audio_seen;
} probe_authority_t;
typedef struct {
  uint64_t min, max, sum, wall_min, wall_max, wall_sum, quantum_max;
  unsigned samples, fuel, quanta,deadline_yields,zero_progress_yields;
  pet_program_result_t result;
} probe_case_t;
enum { PHASE_BEGIN, PHASE_STEP, PHASE_COMMIT, PHASE_CURRENT, PHASE_STACK, PHASE_COUNT };
typedef struct {
  uint64_t phase[PHASE_COUNT], wall, started_us, ended_us;
  unsigned kind, sample, slice;
} probe_quantum_t;
static portMUX_TYPE probe_mux = portMUX_INITIALIZER_UNLOCKED;
static bool probe_active, probe_has_started;
static int64_t probe_last_start;
/* Only an admitted protocol worker can touch these. None is user/app state. */
static pet_program_state_t probe_state, probe_before;
static pet_program_workspace_t probe_workspace;
static probe_authority_t probe_authority;
static probe_case_t probe_cases[4];
static uint32_t probe_stack_min;
static uint64_t probe_quantum_max;
static unsigned probe_quanta;
static probe_quantum_t probe_quantum, probe_slowest;
static char probe_reply[2048];
static const pet_program_insn_t backedge_code[] = {{PET_OP_JUMP, 0, 0, 0, 0}};
static const pet_program_t backedge = {backedge_code, 1, PET_PROGRAM_FUEL, 0, 0};
/* HALT is zero; all admitted code slots are checked, not executed. */
_Static_assert(PET_OP_HALT == 0, "zero-filled validation vector must remain HALT");
/* Native-only zero vector; never modified or exposed to components. */
static EXT_RAM_BSS_ATTR pet_program_insn_t validation_code[PET_PROGRAM_CODE];
static const pet_program_t validation = {validation_code, PET_PROGRAM_CODE, 1, 0, 0};
static int64_t deadline_clock(void *unused){(void)unused;return esp_timer_get_time();}

static bool audio_busy(void) {
  /* Do not short-circuit: read all four current service flags each time. */
  bool capture = pet_p4_audio_active();
  bool playback = pet_p4_audio_playback_active();
  bool conversation = pet_p4_audio_conversation_active();
  bool music = pet_p4_audio_music_active();
  return capture || playback || conversation || music;
}
static bool probe_current(void *context, uint64_t generation) {
  probe_authority_t *a = context;
  bool busy = audio_busy();
  if (busy) { a->audio_seen = true; pet_resources_device_revoke(a->owner); }
  bool lease_current = pet_resources_device_current(a->lease);
  return !busy && generation == a->owner.generation && lease_current;
}
static bool release_owner(void) {
  /* Only after run/commit/validation returned: cancellation never frees an
   * executing core's slot, and no failed case automatically restarts. */
  bool finished = true;
  if (probe_authority.owner.generation) pet_resources_device_revoke(probe_authority.owner);
  if (probe_authority.lease.serial)
    finished = pet_resources_device_finish(probe_authority.lease) == PET_RESOURCE_FINISH_REVOKED;
  memset(&probe_authority.owner, 0, sizeof(probe_authority.owner));
  memset(&probe_authority.lease, 0, sizeof(probe_authority.lease));
  return finished;
}
static const char *open_owner(void) {
  if (audio_busy()) { probe_authority.audio_seen = true; return "audio_busy"; }
  pet_resource_result_t result = pet_resources_device_owner_open(
    PET_RESOURCE_MASK(PET_RESOURCE_NATIVE_PROBE), &probe_authority.owner);
  if (result == PET_RESOURCE_OK)
    result = pet_resources_device_acquire(probe_authority.owner, PET_RESOURCE_NATIVE_PROBE,
      PET_PROGRAM_FUEL, &probe_authority.lease);
  if (result != PET_RESOURCE_OK) { (void)release_owner(); return "resource_busy"; }
  return NULL;
}
static pet_program_authority_t core_authority(void) {
  return (pet_program_authority_t){probe_authority.owner.generation, probe_current,
    &probe_authority, &pet_program_collection_grant, 1};
}
static void observe_stack(void) {
  /* ESP-IDF RISC-V StackType_t is uint8_t; multiplication also preserves units
   * for other FreeRTOS ports and host stubs. This is task-lifetime minimum. */
  uint64_t bytes = (uint64_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
  uint32_t bounded = bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
  if (bounded < probe_stack_min) probe_stack_min = bounded;
}
static const char *cancel_error(void) {
  return probe_authority.audio_seen ? "audio_busy" : "cancelled";
}
static bool phase_end(unsigned phase, int64_t *cursor) {
  int64_t now = esp_timer_get_time();
  if (now < 0 || now < *cursor) return false;
  probe_quantum.phase[phase] += (uint64_t)(now - *cursor);
  *cursor = now;
  return true;
}
static void remember_result(probe_case_t *measured, pet_program_result_t result, bool validate_only) {
  if (!measured) return;
  measured->result = result;
  measured->fuel = validate_only ? 0 : probe_workspace.used_fuel;
}
/* First quantum includes begin's structural validation/copy; final quantum
 * includes commit when requested. No scheduling wait is inside quantum timing.
 * Full invocation wall time below separately includes every one-tick yield. */
static const char *execute_sliced(const pet_program_t *program, bool publish, bool validate_only,
    probe_case_t *measured, pet_program_result_t *result, uint64_t *active_us,
    unsigned kind, unsigned sample) {
  bool first = true;
  unsigned slice = 0,idle_slices=0;
  *active_us = 0;
  do {
    if(kind==3&&slice>=512)return "slice_limit";
    memset(&probe_quantum, 0, sizeof(probe_quantum));
    probe_quantum.kind = kind; probe_quantum.sample = sample; probe_quantum.slice = ++slice;
    pet_program_authority_t authority = core_authority();
    uint32_t trace_tag=(kind<<24)|(sample<<16)|slice;
    pet_sched_trace_mark(PET_SCHED_MARK_PROGRAM_BEGIN,trace_tag);
    int64_t started = esp_timer_get_time();
    if (started < 0) return "clock_error";
    probe_quantum.started_us=(uint64_t)started;
    int64_t cursor = started;
    if (validate_only) {
      bool live = probe_current(&probe_authority, authority.generation);
      if (!phase_end(PHASE_CURRENT, &cursor)) return "clock_error";
      if (!live) return cancel_error();
      *result = pet_program_validate(program);
      remember_result(measured, *result, true);
      if (!phase_end(PHASE_BEGIN, &cursor)) return "clock_error";
    } else {
      if (first) {
        *result = pet_program_begin(program, &pet_program_collection_event,
          &probe_state, &authority, &probe_workspace);
        remember_result(measured, *result, false);
        if (!phase_end(PHASE_BEGIN, &cursor)) return "clock_error";
      }
      if (*result == PET_PROGRAM_RUNNING) {
        if(kind==3){
          pet_program_deadline_t deadline={0};
          if(!pet_program_deadline_begin(&deadline,deadline_clock,NULL,750))return "clock_error";
          unsigned previous_fuel=probe_workspace.used_fuel;
          *result=pet_program_step_with_checkpoint(&probe_workspace,&authority,PET_PROGRAM_QUANTUM,pet_program_deadline_check,&deadline);
          remember_result(measured,*result,false);
          if(deadline.failed)return "clock_error";
          if(measured&&deadline.yielded){measured->deadline_yields++;if(previous_fuel==probe_workspace.used_fuel)measured->zero_progress_yields++;}
          if(deadline.yielded&&previous_fuel==probe_workspace.used_fuel){if(++idle_slices>=8)return "budget_stalled";}
          else idle_slices=0;
        }else *result = pet_program_step(&probe_workspace, &authority, PET_PROGRAM_QUANTUM);
        remember_result(measured, *result, false);
        if (!phase_end(PHASE_STEP, &cursor)) return "clock_error";
      }
      if (*result == PET_PROGRAM_READY && publish) {
        *result = pet_program_commit(&probe_state, &probe_workspace, &authority);
        remember_result(measured, *result, false);
        if (!phase_end(PHASE_COMMIT, &cursor)) return "clock_error";
      }
    }
    /* The actual core outcome is retained before later clock/cancel checks. */
    bool live = probe_current(&probe_authority, authority.generation);
    if (!phase_end(PHASE_CURRENT, &cursor)) return "clock_error";
    observe_stack(); /* Include the watermark scan in measured pre-yield work. */
    if (!phase_end(PHASE_STACK, &cursor)) return "clock_error";
    uint64_t elapsed = (uint64_t)(cursor - started);
    probe_quantum.wall = elapsed;
    probe_quantum.ended_us=(uint64_t)cursor;
    pet_sched_trace_mark(PET_SCHED_MARK_PROGRAM_END,trace_tag);
    if (!probe_quanta || elapsed > probe_quantum_max) {
      probe_quantum_max = elapsed;
      probe_slowest = probe_quantum; /* One real slice, never independent maxima. */
    }
    probe_quanta++;
    if (measured) {
      if (elapsed > measured->quantum_max) measured->quantum_max = elapsed;
      measured->quanta++;
    }
    *active_us += elapsed;
    if (!live || *result == PET_PROGRAM_CANCELLED) return cancel_error();
    if (*result != PET_PROGRAM_RUNNING && *result != PET_PROGRAM_READY
        && *result != PET_PROGRAM_COMMITTED && *result != PET_PROGRAM_FUEL_EXHAUSTED) return "probe_failed";
    vTaskDelay(1); /* Every successful quantum, not only the completed program. */
    first = false;
  } while (*result == PET_PROGRAM_RUNNING);
  return NULL;
}
static const char *measure_case(unsigned kind) {
  for (unsigned sample = 0; sample < PROBE_SAMPLES; sample++) {
    pet_program_authority_t authority = core_authority();
    if (!probe_current(&probe_authority, authority.generation)) return cancel_error();
    pet_program_collection_init(&probe_state, authority.generation);
    memcpy(&probe_before, &probe_state, sizeof(probe_state));
    int64_t started = esp_timer_get_time();
    pet_program_result_t result = PET_PROGRAM_EMPTY;
    uint64_t active_us = 0;
    const char *error = execute_sliced(kind == 0 ? &pet_program_collection : kind == 2 ? &validation : &backedge,
      kind == 0, kind == 2, &probe_cases[kind], &result, &active_us, kind, sample + 1);
    int64_t ended = esp_timer_get_time();
    if (error) return error;
    if (!probe_current(&probe_authority, authority.generation)) return cancel_error();
    if (ended < started) return "clock_error";
    unsigned fuel = kind == 2 ? 0 : probe_workspace.used_fuel;
    if ((kind == 0 && (result != PET_PROGRAM_COMMITTED || fuel != 89
          || probe_state.outputs[0] != 51 || probe_state.outputs[1] != 4
          || probe_state.outbox_count != 1 || probe_state.revision != 1))
        || ((kind == 1||kind==3) && (result != PET_PROGRAM_FUEL_EXHAUSTED || fuel != PET_PROGRAM_FUEL))
        || (kind == 2 && result != PET_PROGRAM_READY)
        || (kind != 0 && memcmp(&probe_before, &probe_state, sizeof(probe_state)))) return "probe_failed";
    probe_case_t *measured = &probe_cases[kind];
    uint64_t wall = (uint64_t)(ended - started);
    if (!measured->samples || active_us < measured->min) measured->min = active_us;
    if (active_us > measured->max) measured->max = active_us;
    if (!measured->samples || wall < measured->wall_min) measured->wall_min = wall;
    if (wall > measured->wall_max) measured->wall_max = wall;
    measured->sum += active_us; measured->wall_sum += wall;
    measured->samples++; measured->fuel = fuel;
  }
  return NULL;
}
static const char *check_commit_boundaries(bool *cancelled, bool *stale) {
  pet_program_authority_t authority = core_authority();
  pet_program_collection_init(&probe_state, authority.generation);
  memcpy(&probe_before, &probe_state, sizeof(probe_state));
  pet_program_result_t result = PET_PROGRAM_EMPTY;
  uint64_t active_us;
  const char *error = execute_sliced(&pet_program_collection, false, false, NULL, &result, &active_us, 4, 1);
  if (error) return error;
  if (result != PET_PROGRAM_READY) return "probe_failed";
  /* Real revocation; occupancy stays held until the attempted commit returns. */
  pet_resources_device_revoke(probe_authority.owner);
  *cancelled = pet_program_commit(&probe_state, &probe_workspace, &authority) == PET_PROGRAM_CANCELLED
    && !memcmp(&probe_before, &probe_state, sizeof(probe_state));
  if (!release_owner() || !*cancelled) return "probe_failed";
  if (probe_authority.audio_seen) return "audio_busy";
  error = open_owner();
  if (error) return error;
  authority = core_authority();
  pet_program_collection_init(&probe_state, authority.generation);
  error = execute_sliced(&pet_program_collection, false, false, NULL, &result, &active_us, 5, 1);
  if (error) return error;
  if (result != PET_PROGRAM_READY) return "probe_failed";
  uint64_t old_generation = authority.generation;
  /* Old candidate stays private; its physical run exited before release. */
  if (!release_owner()) return "probe_failed";
  error = open_owner();
  if (error) return error;
  authority = core_authority();
  pet_program_collection_init(&probe_state, authority.generation);
  memcpy(&probe_before, &probe_state, sizeof(probe_state));
  *stale = old_generation != authority.generation
    && pet_program_commit(&probe_state, &probe_workspace, &authority) == PET_PROGRAM_CANCELLED
    && !memcmp(&probe_before, &probe_state, sizeof(probe_state));
  if (!*stale) return "probe_failed";
  if (!probe_current(&probe_authority, authority.generation)) return cancel_error();
  observe_stack();
  return NULL;
}
static void send_early_error(pet_program_probe_send_fn send, void *context, const char *error) {
  char reply[128]; /* Fixed internal error code, never input text. */
  (void)snprintf(reply, sizeof(reply),
    "{\"topic\":\"debug/program_probe_result\",\"payload\":{\"ok\":false,\"error\":\"%s\",\"cases\":[]}}", error);
  send(reply, context);
}
static const char *result_name(pet_program_result_t result) {
  switch (result) {
    case PET_PROGRAM_EMPTY: return "not_run";
    case PET_PROGRAM_RUNNING: return "running";
    case PET_PROGRAM_READY: return "valid";
    case PET_PROGRAM_COMMITTED: return "committed";
    case PET_PROGRAM_FUEL_EXHAUSTED: return "fuel_exhausted";
    case PET_PROGRAM_CANCELLED: return "cancelled";
    default: return "failed";
  }
}
static bool format_reply(const char *error, bool cancelled, bool stale, uint64_t wall_us) {
  static const char *const kinds[] = {"filter_map_reduce", "backedge_4096", "validate_256", "backedge_4096_deadline"};
  int n = snprintf(probe_reply, sizeof(probe_reply),
    "{\"topic\":\"debug/program_probe_result\",\"payload\":{\"ok\":%s,\"error\":\"%s\",\"cases\":[",
    error ? "false" : "true", error ? error : "");
  if (n < 0 || (size_t)n >= sizeof(probe_reply)) return false;
  size_t used = (size_t)n;
  for (unsigned i = 0; i < 4; i++) {
    const probe_case_t *c = &probe_cases[i];
    n = snprintf(probe_reply + used, sizeof(probe_reply) - used,
      "%s{\"kind\":\"%s\",\"samples\":%u,\"minUs\":%" PRIu64 ",\"maxUs\":%" PRIu64
      ",\"sumUs\":%" PRIu64 ",\"wallMinUs\":%" PRIu64 ",\"wallMaxUs\":%" PRIu64
      ",\"wallSumUs\":%" PRIu64 ",\"quantumMaxUs\":%" PRIu64 ",\"quanta\":%u,\"result\":\"%s\",\"usedFuel\":%u,\"deadlineYields\":%u,\"zeroProgressYields\":%u}",
      i ? "," : "", kinds[i], c->samples, c->min, c->max, c->sum,
      c->wall_min, c->wall_max, c->wall_sum, c->quantum_max, c->quanta,
      result_name(c->result), c->fuel,c->deadline_yields,c->zero_progress_yields);
    if (n < 0 || (size_t)n >= sizeof(probe_reply) - used) return false;
    used += (size_t)n;
  }
  n = snprintf(probe_reply + used, sizeof(probe_reply) - used,
    "],\"cancelBeforeCommitRejected\":%s,\"staleOwnerRejected\":%s,"
    "\"stateBytes\":%u,\"workspaceBytes\":%u,\"taskStackMinBytes\":%u,\"totalWallUs\":%" PRIu64
    ",\"quantumFuel\":%u,\"quantumMaxUs\":%" PRIu64 ",\"quanta\":%u,\"guardBudgetUs\":750,\"slowestQuantum\":",
    cancelled ? "true" : "false", stale ? "true" : "false", (unsigned)sizeof(probe_state),
    (unsigned)sizeof(probe_workspace), (unsigned)probe_stack_min, wall_us,
    PET_PROGRAM_QUANTUM, probe_quantum_max, probe_quanta);
  if (n < 0 || (size_t)n >= sizeof(probe_reply) - used) return false;
  used += (size_t)n;
  if (!probe_quanta) n = snprintf(probe_reply + used, sizeof(probe_reply) - used, "null}}");
  else {
    static const char *const names[] = {"filter_map_reduce", "backedge_4096", "validate_256", "backedge_4096_deadline", "cancel_before_commit", "stale_owner"};
    const probe_quantum_t *q = &probe_slowest;
    n = snprintf(probe_reply + used, sizeof(probe_reply) - used,
      "{\"kind\":\"%s\",\"sample\":%u,\"slice\":%u,\"beginUs\":%" PRIu64
      ",\"stepUs\":%" PRIu64 ",\"commitUs\":%" PRIu64 ",\"freshCurrentUs\":%" PRIu64
      ",\"stackScanUs\":%" PRIu64 ",\"wallUs\":%" PRIu64
      ",\"startedUs\":%" PRIu64 ",\"endedUs\":%" PRIu64 "}}}",
      names[q->kind], q->sample, q->slice, q->phase[PHASE_BEGIN], q->phase[PHASE_STEP],
      q->phase[PHASE_COMMIT], q->phase[PHASE_CURRENT], q->phase[PHASE_STACK], q->wall,q->started_us,q->ended_us);
  }
  return n >= 0 && (size_t)n < sizeof(probe_reply) - used;
}
void pet_p4_program_probe(pet_program_probe_send_fn send, void *context) {
  if (!send) return;
  int64_t started = esp_timer_get_time();
  const char *error = NULL;
  portENTER_CRITICAL(&probe_mux);
  if (probe_active) error = "busy";
  else if (started < 0 || (probe_has_started && started < probe_last_start)) error = "clock_error";
  else if (probe_has_started && started - probe_last_start < PROBE_INTERVAL_US) error = "rate_limited";
  else { probe_active = true; probe_has_started = true; probe_last_start = started; }
  portEXIT_CRITICAL(&probe_mux);
  if (error) { send_early_error(send, context, error); return; }
  memset(&probe_authority, 0, sizeof(probe_authority));
  memset(probe_cases, 0, sizeof(probe_cases));
  probe_stack_min = UINT32_MAX;
  probe_quantum_max = 0; probe_quanta = 0;
  memset(&probe_slowest, 0, sizeof(probe_slowest));
  bool cancelled = false, stale = false;
  error = open_owner();
  for (unsigned i = 0; i < 4 && !error; i++) error = measure_case(i);
  if (!error) error = check_commit_boundaries(&cancelled, &stale);
  /* Core is physically finished on every path, including cancellation/trap. */
  if (!release_owner() && !error) error = "probe_failed";
  int64_t ended = esp_timer_get_time();
  if (ended < started) error = "clock_error";
  uint64_t wall = ended >= started ? (uint64_t)(ended - started) : 0;
  bool formatted = false;
  for (unsigned pass = 0; pass < 2; pass++) {
    observe_stack();
    formatted = format_reply(error, cancelled, stale, wall);
    if (!formatted) break;
  }
  if (formatted) send(probe_reply, context);
  else send_early_error(send, context, "reply_overflow");
  portENTER_CRITICAL(&probe_mux);
  probe_active = false;
  portEXIT_CRITICAL(&probe_mux);
}
