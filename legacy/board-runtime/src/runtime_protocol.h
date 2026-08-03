#ifndef BOARD_RUNTIME_PROTOCOL_H
#define BOARD_RUNTIME_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>

#include "runtime_json.h"

typedef struct {
  char type[32];

  bool has_x;
  double x;
  bool has_y;
  double y;
  bool has_start_x;
  double start_x;
  bool has_start_y;
  double start_y;
  bool has_dx;
  double dx;
  bool has_dy;
  double dy;
  bool has_duration_ms;
  double duration_ms;
  bool has_viewport_width;
  double viewport_width;
  bool has_viewport_height;
  double viewport_height;

  char view[96];
  char state[96];
  char target_state[96];
  char active_detail_id[96];
} br_input_action;

typedef struct {
  bool matched;
  char target_device_id[128];
  char target_source[128];
} br_remote_binding;

typedef struct {
  long long total_tokens;
  long long input_tokens;
  long long output_tokens;
  long long cached_input_tokens;
  long long cache_creation_input_tokens;
  long long reasoning_output_tokens;
  long long last_total_tokens;
  long long last_input_tokens;
  long long last_output_tokens;
  long long last_cached_input_tokens;
  long long last_cache_creation_input_tokens;
  long long last_reasoning_output_tokens;
  double estimated_cost_usd;
  bool has_total_tokens;
  bool has_input_tokens;
  bool has_output_tokens;
  bool has_cached_input_tokens;
  bool has_cache_creation_input_tokens;
  bool has_reasoning_output_tokens;
  bool has_last_total_tokens;
  bool has_last_input_tokens;
  bool has_last_output_tokens;
  bool has_last_cached_input_tokens;
  bool has_last_cache_creation_input_tokens;
  bool has_last_reasoning_output_tokens;
  bool has_estimated_cost_usd;
} br_token_usage;

typedef struct {
  bool should_write;
  bool is_speech;
  bool allow_interrupt;
  char state[64];
  char event[128];
  char reason[128];
  char speech[1024];
  long long payload_ts_ms;
  bool has_payload_ts_ms;
  char source[128];
  char session_id[128];
  char run_id[128];
  char session_key[128];
  bool has_token_usage;
  br_token_usage token_usage;
} br_bridge_state_update;

typedef struct {
  char text[1024];
  char title[256];
  char body[768];
  char source[128];
  char session_id[128];
  char run_id[128];
  char session_key[128];
  long long payload_ts_ms;
  bool has_payload_ts_ms;
  long long expires_at_ms;
  bool has_expires_at_ms;
} br_speech_update;

typedef struct {
  bool enabled;
  char action[16];
  char pc_ip[64];
  int pc_port;
  int listen_port;
  char capture_dev[96];
  char play_dev[96];
  char voice_button[64];
} br_audio_bridge_command;

#define BR_BUTTON_CONFIG_MAX_BINDINGS 16

typedef struct {
  char event[96];
  char action[64];
} br_button_config_binding;

typedef struct {
  int version;
  bool voice_enabled;
  char request_id[96];
  char voice_button[64];
  size_t binding_count;
  br_button_config_binding bindings[BR_BUTTON_CONFIG_MAX_BINDINGS];
} br_button_config_command;

bool br_parse_input_action_json(
  const char *json_text,
  br_input_action *action,
  char *error,
  size_t error_size
);
int br_build_input_action_payload(
  const char *board_device_id,
  const char *local_device_id,
  const char *source,
  const br_input_action *action,
  long long ts_ms,
  char *output,
  size_t output_size
);
bool br_parse_remote_binding_json(const char *json_text, br_remote_binding *binding);
bool br_parse_audio_bridge_command_json(const char *json_text, br_audio_bridge_command *command);
bool br_parse_button_config_command_json(const char *json_text, br_button_config_command *command);
bool br_button_config_find_action_json(
  const char *json_text,
  const char *event,
  char *action,
  size_t action_size
);
bool br_parse_speech_update(const char *json_text, br_speech_update *update);
bool br_parse_speech_text(const char *json_text, char *output, size_t output_size);
bool br_build_message_complete_json(
  const char *json_text,
  const char *fallback_text,
  char *output,
  size_t output_size
);
bool br_bridge_state_from_message(
  const char *topic,
  const char *payload_text,
  br_bridge_state_update *update
);
bool br_payload_to_json_object(
  const char *payload_text,
  char *output,
  size_t output_size,
  long long *payload_ts_ms,
  char *source,
  size_t source_size
);

#endif
