/* Production probe + real production resource adapter/core. Only platform IO
 * observations are stubbed; a wrapper observes physical run return boundaries. */
#include "pet_p4_program_probe.h"
#include "pet_p4_program_core.h"
#include "pet_p4_resources_device.h"
#include "freertos/task.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_llong fake_time = 1000000;
static unsigned flag_reads[4], selected_flag, trigger_read, delays, runs, steps, cancelled_runs;
static unsigned clock_calls, fail_clock_at;
enum { CLOCK_BEGIN, CLOCK_STEP, CLOCK_COMMIT, CLOCK_CURRENT, CLOCK_STACK, CLOCK_NONE };
static unsigned clock_phase = CLOCK_NONE, negative_phase = CLOCK_NONE;
static bool phase_peaks, json_boundary;
static bool backwards_clock;
static bool flags[4], concurrent, concurrent_done, send_reentry, after_ready, between_slices;
static bool after_commit, after_commit_yield;
static unsigned commits;
static char response[2048], nested_response[2048];
static int route, nested_route;
static const pet_program_state_t *observed_state;
static pet_program_state_t observed_before;
static pet_resource_usage_t usage(void) {
  pet_resource_usage_t u;
  pet_resources_device_usage(&u);
  return u;
}
int64_t esp_timer_get_time(void) {
  clock_calls++;
  if (fail_clock_at && clock_calls == fail_clock_at) return -1;
  unsigned phase = clock_phase; clock_phase = CLOCK_NONE;
  if (runs && negative_phase != CLOCK_NONE && phase == negative_phase) {
    negative_phase = CLOCK_NONE;
    return backwards_clock ? atomic_load(&fake_time) - 20 : -1;
  }
  if (phase_peaks && delays == 0 && phase == CLOCK_STEP) atomic_fetch_add(&fake_time, 1000);
  if (phase_peaks && delays == 1 && phase == CLOCK_STACK) atomic_fetch_add(&fake_time, 900);
  if (json_boundary && delays == 0 && phase == CLOCK_STEP) atomic_fetch_add(&fake_time, UINT32_MAX);
  return atomic_fetch_add(&fake_time, 10);
}
static bool read_flag(unsigned flag) {
  flag_reads[flag]++;
  if (trigger_read && flag == selected_flag && flag_reads[flag] >= trigger_read) flags[flag] = true;
  if (flag == 3) clock_phase = CLOCK_CURRENT;
  return flags[flag];
}
bool pet_p4_audio_active(void) { return read_flag(0); }
bool pet_p4_audio_playback_active(void) { return read_flag(1); }
bool pet_p4_audio_conversation_active(void) { return read_flag(2); }
bool pet_p4_audio_music_active(void) { return read_flag(3); }
static void nested_receive(const char *line, void *context) {
  assert(context == &nested_route && strlen(line) < sizeof(nested_response));
  strcpy(nested_response, line);
  assert(strstr(line, "\"error\":\"busy\""));
}
static void receive(const char *line, void *context) {
  assert(context == &route && strlen(line) < sizeof(response));
  assert(usage().leases[PET_RESOURCE_NATIVE_PROBE] == 0);
  strcpy(response, line);
  if (send_reentry) {
    pet_p4_program_probe(nested_receive, &nested_route);
    assert(strcmp(response, line) == 0); /* Reentry cannot overwrite in-flight reply. */
  }
}
static void resource_receive(const char *line, void *context) {
  assert(context == &route && usage().leases[PET_RESOURCE_NATIVE_PROBE] == 1);
  assert(strstr(line, "\"error\":\"resource_busy\""));
  puts(line);
}
static void *nested_thread(void *ignored) {
  (void)ignored;
  pet_p4_program_probe(nested_receive, &nested_route);
  return NULL;
}
void vTaskDelay(TickType_t ticks) {
  assert(ticks == 1);
  delays++;
  assert(usage().leases[PET_RESOURCE_NATIVE_PROBE] == 1);
  if (between_slices) { flags[selected_flag] = true; between_slices = false; }
  if (after_commit_yield && observed_state && observed_state->revision == 1) {
    flags[selected_flag] = true;
    after_commit_yield = false;
  }
  if (concurrent && !concurrent_done) {
    concurrent_done = true;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, nested_thread, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(usage().leases[PET_RESOURCE_NATIVE_PROBE] == 1);
  }
  atomic_fetch_add(&fake_time, 1000);
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
  assert(!task);
  clock_phase = CLOCK_STACK;
  return 2048;
}
pet_program_result_t probe_real_begin(const pet_program_t *, const pet_program_event_t *,
  const pet_program_state_t *, const pet_program_authority_t *, pet_program_workspace_t *);
pet_program_result_t probe_real_step(pet_program_workspace_t *, const pet_program_authority_t *, uint16_t);
pet_program_result_t probe_real_commit(pet_program_state_t *, pet_program_workspace_t *, const pet_program_authority_t *);
pet_program_result_t pet_program_begin(const pet_program_t *p, const pet_program_event_t *e,
  const pet_program_state_t *s, const pet_program_authority_t *a, pet_program_workspace_t *w) {
  runs++;
  assert(usage().leases[PET_RESOURCE_NATIVE_PROBE] == 1);
  if (runs == 1) { observed_state = s; memcpy(&observed_before, s, sizeof(*s)); }
  pet_program_result_t result = probe_real_begin(p, e, s, a, w);
  clock_phase = CLOCK_BEGIN;
  return result;
}
pet_program_result_t pet_program_step(pet_program_workspace_t *w, const pet_program_authority_t *a, uint16_t max_fuel) {
  assert(max_fuel == PET_PROGRAM_QUANTUM);
  unsigned previous = w->used_fuel;
  steps++;
  pet_program_result_t result = probe_real_step(w, a, max_fuel);
  assert(w->used_fuel - previous <= PET_PROGRAM_QUANTUM);
  assert(usage().leases[PET_RESOURCE_NATIVE_PROBE] == 1); /* Not freed inside execution. */
  if (result == PET_PROGRAM_CANCELLED) {
    cancelled_runs++;
    assert(usage().revoked_owners == 1);
    assert(memcmp(observed_state, &observed_before, sizeof(observed_before)) == 0);
  }
  if (after_ready && result == PET_PROGRAM_READY) { flags[0] = true; after_ready = false; }
  clock_phase = CLOCK_STEP;
  return result;
}
pet_program_result_t pet_program_run(const pet_program_t *p, const pet_program_event_t *e,
  const pet_program_state_t *s, const pet_program_authority_t *a, pet_program_workspace_t *w) {
  (void)p; (void)e; (void)s; (void)a; (void)w;
  assert(!"production probe must not drain the synchronous run wrapper");
  return PET_PROGRAM_INVALID;
}
pet_program_result_t pet_program_commit(pet_program_state_t *s, pet_program_workspace_t *w,
  const pet_program_authority_t *a) {
  commits++;
  pet_program_result_t result = probe_real_commit(s, w, a);
  assert(usage().leases[PET_RESOURCE_NATIVE_PROBE] == 1);
  if (after_commit && result == PET_PROGRAM_COMMITTED) {
    flags[selected_flag] = true;
    after_commit = false;
  }
  clock_phase = CLOCK_COMMIT;
  return result;
}
static void assert_clean(void) {
  pet_resource_usage_t u = usage();
  assert(u.live_owners == 0 && u.revoked_owners == 0);
  for (unsigned i = 0; i < PET_RESOURCE_KIND_COUNT; i++) assert(!u.leases[i] && !u.units[i]);
  for (unsigned i = 1; i < 4; i++) assert(flag_reads[i] == flag_reads[0]);
}
static void print_response(void) { puts(response); }
int main(int argc, char **argv) {
  assert(argc >= 2);
  const char *mode = argv[1];
  bool post_commit = strcmp(mode, "after_commit") == 0 || strcmp(mode, "after_commit_yield") == 0;
  pet_resource_owner_t other_owner = {0}; pet_resource_lease_t other_lease = {0};
  if (strcmp(mode, "null") == 0) {
    pet_p4_program_probe(NULL, NULL);
    assert(!clock_calls && !flag_reads[0] && !runs); assert_clean(); return 0;
  }
  if (strcmp(mode, "busy") == 0 || strcmp(mode, "mid_audio") == 0 || strcmp(mode, "between_slices") == 0) {
    assert(argc == 3); selected_flag = (unsigned)strtoul(argv[2], NULL, 10); assert(selected_flag < 4);
    if (strcmp(mode, "busy") == 0) flags[selected_flag] = true;
    else if (strcmp(mode, "mid_audio") == 0) trigger_read = 20;
    else between_slices = true;
  } else if (strcmp(mode, "negative_phase") == 0 || strcmp(mode, "backwards_phase") == 0) {
    assert(argc == 3); negative_phase = (unsigned)strtoul(argv[2], NULL, 10); assert(negative_phase < CLOCK_NONE);
    backwards_clock = strcmp(mode, "backwards_phase") == 0;
  } else if (strcmp(mode, "phase_peaks") == 0) phase_peaks = true;
  else if (strcmp(mode, "json_boundary") == 0) json_boundary = true;
  else if (post_commit) {
    assert(argc == 3); selected_flag = (unsigned)strtoul(argv[2], NULL, 10); assert(selected_flag < 4);
    after_commit = strcmp(mode, "after_commit") == 0;
    after_commit_yield = !after_commit;
    assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_HOST_VIEW), &other_owner) == PET_RESOURCE_OK);
    assert(pet_resources_device_acquire(other_owner, PET_RESOURCE_HOST_VIEW, 1, &other_lease) == PET_RESOURCE_OK);
  } else if (strcmp(mode, "concurrent") == 0) concurrent = true;
  else if (strcmp(mode, "send_reentry") == 0) send_reentry = true;
  else if (strcmp(mode, "before_commit") == 0) after_ready = true;
  else if (strcmp(mode, "clock") == 0) fail_clock_at = 3;
  else if (strcmp(mode, "resource_busy") == 0) {
    pet_resource_owner_t owner = {0}; pet_resource_lease_t lease = {0};
    assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_NATIVE_PROBE), &owner) == PET_RESOURCE_OK);
    assert(pet_resources_device_acquire(owner, PET_RESOURCE_NATIVE_PROBE, PET_PROGRAM_FUEL, &lease) == PET_RESOURCE_OK);
    /* Receive separately: the external owner's physical lease must remain. */
    pet_p4_program_probe(resource_receive, &route);
    assert(pet_resources_device_current(lease));
    pet_resources_device_revoke(owner); assert(pet_resources_device_finish(lease) == PET_RESOURCE_FINISH_REVOKED);
    assert_clean(); return 0;
  } else assert(strcmp(mode, "success") == 0);
  pet_p4_program_probe(receive, &route); print_response();
  if (post_commit) {
    /* Audio observation may revoke only the probe, not another service owner. */
    assert(pet_resources_device_current(other_lease));
    assert(usage().leases[PET_RESOURCE_HOST_VIEW] == 1 && usage().units[PET_RESOURCE_HOST_VIEW] == 1);
    pet_resources_device_revoke(other_owner);
    assert(pet_resources_device_finish(other_lease) == PET_RESOURCE_FINISH_REVOKED);
  }
  assert_clean();
  if (strcmp(mode, "busy") == 0) {
    assert(!runs && !delays && strstr(response, "audio_busy"));
  } else if (strcmp(mode, "mid_audio") == 0) {
    assert(runs == 1 && cancelled_runs == 1 && !delays && strstr(response, "audio_busy"));
    assert(memcmp(observed_state, &observed_before, sizeof(observed_before)) == 0);
  } else if (strcmp(mode, "before_commit") == 0) {
    assert(runs == 1 && steps == 2 && !cancelled_runs && delays == 1 && strstr(response, "audio_busy"));
    assert(memcmp(observed_state, &observed_before, sizeof(observed_before)) == 0);
  } else if (strcmp(mode, "between_slices") == 0) {
    assert(runs == 1 && steps == 2 && cancelled_runs == 1 && delays == 1 && strstr(response, "audio_busy"));
    assert(memcmp(observed_state, &observed_before, sizeof(observed_before)) == 0);
  } else if (post_commit) {
    assert(runs == 1 && steps == 2 && commits == 1 && !cancelled_runs && strstr(response, "audio_busy"));
    assert(delays == (strcmp(mode, "after_commit") == 0 ? 1U : 2U));
    assert(observed_state->revision == 1 && observed_state->outputs[0] == 51
      && observed_state->outputs[1] == 4 && observed_state->outbox_count == 1);
  } else if (strcmp(mode, "negative_phase") == 0 || strcmp(mode, "backwards_phase") == 0) {
    assert(strstr(response, "clock_error") && runs == 1 && delays <= 1);
    unsigned phase = (unsigned)strtoul(argv[2], NULL, 10);
    if (phase == CLOCK_COMMIT) assert(commits == 1 && observed_state->revision == 1);
    else assert(!commits && observed_state->revision == 0);
  } else if (strcmp(mode, "clock") == 0) assert(strstr(response, "clock_error") && !delays);
  else {
    assert(delays == 1052 && runs == 26 && steps == 532 && strstr(response, "\"ok\":true"));
    assert(strstr(response, "\"cancelBeforeCommitRejected\":true"));
    assert(strstr(response, "\"staleOwnerRejected\":true"));
    if (concurrent || send_reentry) { assert(strstr(nested_response, "busy")); puts(nested_response); }
  }
  if (json_boundary) return 0; /* Its simulated duration legitimately exceeds the rate limit. */
  /* Explicit later request is throttled; clearing audio never auto-resumes. */
  memset(flags, 0, sizeof(flags)); trigger_read = 0; send_reentry = false;
  unsigned old_runs = runs;
  pet_p4_program_probe(receive, &route); print_response();
  assert(strstr(response, "rate_limited") && runs == old_runs); assert_clean();
  if (strcmp(mode, "success") == 0 || strcmp(mode, "mid_audio") == 0) {
    atomic_fetch_add(&fake_time, INT64_C(5000000));
    pet_p4_program_probe(receive, &route); print_response();
    assert(strstr(response, "\"ok\":true") && runs == old_runs + 26); assert_clean();
  }
  return 0;
}
