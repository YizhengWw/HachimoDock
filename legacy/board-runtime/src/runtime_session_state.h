#ifndef BOARD_RUNTIME_SESSION_STATE_H
#define BOARD_RUNTIME_SESSION_STATE_H

#include <stdbool.h>
#include <stddef.h>

#include "runtime_protocol.h"

#define BR_SESSION_MAX_RECORDS 16

typedef struct {
  bool active;
  char key[256];
  char source[128];
  char session_id[128];
  char run_id[128];
  char session_key[128];
  char state[64];
  char event[128];
  char reason[128];
  long long updated_at_ms;
  long long display_until_ms;
} br_session_record;

typedef struct {
  char state[64];
  char event[128];
  char reason[128];
  char active_key[256];
  bool should_interrupt;
  bool changed;
  long long updated_at_ms;
} br_session_resolution;

typedef struct {
  br_session_record records[BR_SESSION_MAX_RECORDS];
  char current_state[64];
  char current_event[128];
  char current_reason[128];
  char current_key[256];
  long long done_hold_ms;
  long long stale_ms;
  long long working_idle_buffer_ms;
  long long last_working_at_ms;
} br_session_machine;

void br_session_machine_init(br_session_machine *machine, long long done_hold_ms, long long stale_ms);
bool br_session_machine_apply(
  br_session_machine *machine,
  const br_bridge_state_update *update,
  long long now_ms,
  br_session_resolution *resolution
);
bool br_session_machine_tick(
  br_session_machine *machine,
  long long now_ms,
  br_session_resolution *resolution
);
void br_session_machine_debug_json(
  const br_session_machine *machine,
  const br_session_resolution *resolution,
  char *output,
  size_t output_size
);

#endif
