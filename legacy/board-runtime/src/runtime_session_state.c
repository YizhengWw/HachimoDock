#include "runtime_session_state.h"

#include <stdio.h>
#include <string.h>

#include "runtime_common.h"

static void br_session_copy(char *dst, size_t dst_size, const char *src) {
  br_normalize_text(src, "", dst, dst_size);
}

static void br_session_build_key(
  const br_bridge_state_update *update,
  char *output,
  size_t output_size
) {
  const char *source = update->source[0] ? update->source : "unknown";
  const char *kind = "source";
  const char *id = source;
  if (update->session_id[0]) {
    kind = "session";
    id = update->session_id;
  } else if (update->run_id[0]) {
    kind = "run";
    id = update->run_id;
  } else if (update->session_key[0]) {
    kind = "key";
    id = update->session_key;
  }
  snprintf(output, output_size, "%s:%s:%s", source, kind, id);
}

static bool br_session_record_expired(
  const br_session_record *record,
  long long now_ms,
  long long stale_ms
) {
  if (!record->active) return true;
  if (strcmp(record->state, "done") == 0 &&
      record->display_until_ms > 0 &&
      now_ms >= record->display_until_ms) {
    return true;
  }
  if (stale_ms > 0 &&
      record->updated_at_ms > 0 &&
      now_ms - record->updated_at_ms > stale_ms) {
    return true;
  }
  return false;
}

static void br_session_cleanup(br_session_machine *machine, long long now_ms) {
  int i;
  for (i = 0; i < BR_SESSION_MAX_RECORDS; ++i) {
    if (br_session_record_expired(&machine->records[i], now_ms, machine->stale_ms)) {
      memset(&machine->records[i], 0, sizeof(machine->records[i]));
    }
  }
}

static br_session_record *br_session_find_slot(br_session_machine *machine, const char *key) {
  int i;
  int empty_index = -1;
  int oldest_index = 0;
  for (i = 0; i < BR_SESSION_MAX_RECORDS; ++i) {
    if (machine->records[i].active && strcmp(machine->records[i].key, key) == 0) {
      return &machine->records[i];
    }
    if (!machine->records[i].active && empty_index < 0) {
      empty_index = i;
    }
    if (machine->records[i].updated_at_ms < machine->records[oldest_index].updated_at_ms) {
      oldest_index = i;
    }
  }
  return empty_index >= 0 ? &machine->records[empty_index] : &machine->records[oldest_index];
}

static bool br_session_text_is_probe_reason(const char *text) {
  if (!text || !text[0]) return false;
  return strcmp(text, "process.missing") == 0 ||
         strcmp(text, "process.detected") == 0 ||
         strcmp(text, "source.disabled") == 0 ||
         strcmp(text, "startup") == 0 ||
         strcmp(text, "heartbeat") == 0;
}

static bool br_session_record_is_idle_probe(const br_session_record *record) {
  if (!record || !record->active) return false;
  if (strcmp(record->state, "idle") != 0) return false;
  return br_session_text_is_probe_reason(record->event) ||
         br_session_text_is_probe_reason(record->reason);
}

static const br_session_record *br_session_pick_newer(
  const br_session_record *best,
  const br_session_record *record
) {
  if (!record || !record->active) return best;
  if (!best || record->updated_at_ms > best->updated_at_ms) {
    return record;
  }
  return best;
}

static const br_session_record *br_session_pick_active(const br_session_machine *machine) {
  int i;
  const br_session_record *best = NULL;
  const br_session_record *fallback = NULL;
  for (i = 0; i < BR_SESSION_MAX_RECORDS; ++i) {
    const br_session_record *record = &machine->records[i];
    if (!record->active) continue;
    if (br_session_record_is_idle_probe(record)) {
      fallback = br_session_pick_newer(fallback, record);
      continue;
    }
    best = br_session_pick_newer(best, record);
  }
  return best ? best : fallback;
}

static bool br_session_should_keep_working_for_idle_gap(
  const br_session_machine *machine,
  const char *previous_state,
  const char *next_state,
  long long now_ms
) {
  long long elapsed_ms;
  if (!machine || strcmp(previous_state, "working") != 0 || strcmp(next_state, "idle") != 0) {
    return false;
  }
  if (machine->working_idle_buffer_ms <= 0 || machine->last_working_at_ms <= 0) {
    return false;
  }
  elapsed_ms = now_ms - machine->last_working_at_ms;
  return elapsed_ms >= 0 && elapsed_ms < machine->working_idle_buffer_ms;
}

static void br_session_fill_resolution(
  const br_session_machine *machine,
  const char *state,
  const char *event,
  const char *reason,
  const char *active_key,
  bool changed,
  bool should_interrupt,
  long long now_ms,
  br_session_resolution *resolution
) {
  if (!resolution) return;
  br_session_copy(resolution->state, sizeof(resolution->state), state);
  br_session_copy(resolution->event, sizeof(resolution->event), event);
  br_session_copy(resolution->reason, sizeof(resolution->reason), reason);
  br_session_copy(resolution->active_key, sizeof(resolution->active_key), active_key);
  resolution->changed = changed;
  resolution->should_interrupt = should_interrupt;
  resolution->updated_at_ms = now_ms;
  (void) machine;
}

static bool br_session_resolve(
  br_session_machine *machine,
  long long now_ms,
  bool from_push,
  bool allow_interrupt,
  br_session_resolution *resolution
) {
  const br_session_record *best;
  char previous_state[64];
  char next_state[64];
  char next_event[128];
  char next_reason[128];
  char next_key[256];
  bool changed;
  bool state_changed;
  bool should_interrupt;

  br_session_copy(previous_state, sizeof(previous_state), machine->current_state);
  br_session_cleanup(machine, now_ms);
  best = br_session_pick_active(machine);

  br_session_copy(next_state, sizeof(next_state), best ? best->state : "idle");
  br_session_copy(next_event, sizeof(next_event), best ? best->event : "");
  br_session_copy(next_reason, sizeof(next_reason), best ? best->reason : "session.no_sources");
  br_session_copy(next_key, sizeof(next_key), best ? best->key : "");

  if (br_session_should_keep_working_for_idle_gap(machine, previous_state, next_state, now_ms)) {
    br_session_copy(next_state, sizeof(next_state), machine->current_state);
    br_session_copy(next_event, sizeof(next_event), machine->current_event);
    br_session_copy(next_reason, sizeof(next_reason), machine->current_reason);
    br_session_copy(next_key, sizeof(next_key), machine->current_key);
  }

  state_changed = strcmp(next_state, previous_state) != 0;
  changed = state_changed ||
            strcmp(next_event, machine->current_event) != 0 ||
            strcmp(next_key, machine->current_key) != 0;
  should_interrupt =
    changed &&
    state_changed &&
    from_push &&
    allow_interrupt &&
    (strcmp(next_state, "error") == 0 || strcmp(next_state, "done") == 0);

  br_session_copy(machine->current_state, sizeof(machine->current_state), next_state);
  br_session_copy(machine->current_event, sizeof(machine->current_event), next_event);
  br_session_copy(machine->current_reason, sizeof(machine->current_reason), next_reason);
  br_session_copy(machine->current_key, sizeof(machine->current_key), next_key);

  br_session_fill_resolution(machine,
                             next_state,
                             next_event,
                             next_reason,
                             next_key,
                             changed,
                             should_interrupt,
                             now_ms,
                             resolution);
  return changed;
}

void br_session_machine_init(br_session_machine *machine, long long done_hold_ms, long long stale_ms) {
  if (!machine) return;
  memset(machine, 0, sizeof(*machine));
  machine->done_hold_ms = done_hold_ms > 0 ? done_hold_ms : 3000;
  machine->stale_ms = stale_ms > 0 ? stale_ms : 300000;
  machine->working_idle_buffer_ms = 3000;
  br_session_copy(machine->current_state, sizeof(machine->current_state), "idle");
  br_session_copy(machine->current_reason, sizeof(machine->current_reason), "session.init");
}

bool br_session_machine_apply(
  br_session_machine *machine,
  const br_bridge_state_update *update,
  long long now_ms,
  br_session_resolution *resolution
) {
  char key[256];
  br_session_record *record;
  long long base_ms;
  long long display_until_ms = 0;

  if (!machine || !update || !update->should_write || !update->state[0]) return false;

  base_ms = update->has_payload_ts_ms && update->payload_ts_ms > 0
    ? update->payload_ts_ms
    : now_ms;
  if (strcmp(update->state, "done") == 0) {
    if (update->has_payload_ts_ms &&
        update->payload_ts_ms > 0 &&
        update->payload_ts_ms + machine->done_hold_ms <= now_ms) {
      return br_session_resolve(machine, now_ms, false, false, resolution);
    }
    display_until_ms = now_ms + machine->done_hold_ms;
  }

  br_session_build_key(update, key, sizeof(key));
  record = br_session_find_slot(machine, key);
  memset(record, 0, sizeof(*record));
  record->active = true;
  br_session_copy(record->key, sizeof(record->key), key);
  br_session_copy(record->source, sizeof(record->source), update->source[0] ? update->source : "unknown");
  br_session_copy(record->session_id, sizeof(record->session_id), update->session_id);
  br_session_copy(record->run_id, sizeof(record->run_id), update->run_id);
  br_session_copy(record->session_key, sizeof(record->session_key), update->session_key);
  br_session_copy(record->state, sizeof(record->state), update->state);
  br_session_copy(record->event, sizeof(record->event), update->event);
  br_session_copy(record->reason, sizeof(record->reason), update->reason);
  record->updated_at_ms = base_ms;
  record->display_until_ms = display_until_ms;

  if (strcmp(record->state, "working") == 0) {
    machine->last_working_at_ms = now_ms;
  }

  return br_session_resolve(machine, now_ms, true, update->allow_interrupt, resolution);
}

bool br_session_machine_tick(
  br_session_machine *machine,
  long long now_ms,
  br_session_resolution *resolution
) {
  if (!machine) return false;
  return br_session_resolve(machine, now_ms, false, false, resolution);
}

void br_session_machine_debug_json(
  const br_session_machine *machine,
  const br_session_resolution *resolution,
  char *output,
  size_t output_size
) {
  size_t used = 0;
  int i;
  bool first = true;
  const char *state;
  const char *event;
  const char *reason;
  const char *active_key;

  if (!machine || !output || output_size == 0) return;
  state = resolution && resolution->state[0] ? resolution->state : machine->current_state;
  event = resolution ? resolution->event : machine->current_event;
  reason = resolution ? resolution->reason : machine->current_reason;
  active_key = resolution ? resolution->active_key : machine->current_key;

  output[0] = '\0';
  br_snprintf_append(output, output_size, &used, "{\"resolvedState\":\"");
  br_json_escape_append(output, output_size, &used, state);
  br_snprintf_append(output, output_size, &used, "\",\"resolvedEvent\":\"");
  br_json_escape_append(output, output_size, &used, event);
  br_snprintf_append(output, output_size, &used, "\",\"activeSessionKey\":\"");
  br_json_escape_append(output, output_size, &used, active_key);
  br_snprintf_append(output, output_size, &used, "\",\"lastReason\":\"");
  br_json_escape_append(output, output_size, &used, reason);
  br_snprintf_append(output, output_size, &used, "\",\"updatedAtMs\":%lld,\"records\":[",
                     resolution ? resolution->updated_at_ms : 0);
  for (i = 0; i < BR_SESSION_MAX_RECORDS; ++i) {
    const br_session_record *record = &machine->records[i];
    if (!record->active) continue;
    if (!first) {
      br_snprintf_append(output, output_size, &used, ",");
    }
    first = false;
    br_snprintf_append(output, output_size, &used, "{\"sessionKey\":\"");
    br_json_escape_append(output, output_size, &used, record->key);
    br_snprintf_append(output, output_size, &used, "\",\"source\":\"");
    br_json_escape_append(output, output_size, &used, record->source);
    br_snprintf_append(output, output_size, &used, "\",\"state\":\"");
    br_json_escape_append(output, output_size, &used, record->state);
    br_snprintf_append(output, output_size, &used, "\",\"event\":\"");
    br_json_escape_append(output, output_size, &used, record->event);
    br_snprintf_append(output, output_size, &used, "\",\"seq\":0,\"updatedAtMs\":%lld,\"displayUntilMs\":%lld,\"candidate\":%s}",
                       record->updated_at_ms,
                       record->display_until_ms,
                       strcmp(record->key, active_key) == 0 ? "true" : "false");
  }
  br_snprintf_append(output, output_size, &used, "]}");
}
