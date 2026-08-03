#include "runtime_protocol.h"

#include <stdio.h>
#include <string.h>

#include "runtime_common.h"
#include "voice_button.h"

static bool br_string_fits_token(const char *json, const br_json_token *token, char *output, size_t output_size) {
  if (!token || token->type != BR_JSON_STRING) {
    return false;
  }
  return br_json_token_to_string(json, token, output, output_size);
}

static bool br_extract_string_key(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  const char *key,
  char *output,
  size_t output_size
) {
  int value_index = br_json_find_key(json, tokens, token_count, object_index, key);
  if (value_index < 0) {
    return false;
  }
  return br_string_fits_token(json, &tokens[value_index], output, output_size);
}

static bool br_extract_first_string_key(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  const char *const *keys,
  size_t key_count,
  char *output,
  size_t output_size
) {
  char temp[1024];
  for (size_t i = 0; i < key_count; i += 1) {
    if (br_extract_string_key(json, tokens, token_count, object_index, keys[i], temp, sizeof(temp))) {
      return br_normalize_text(temp, "", output, output_size);
    }
  }
  if (output && output_size > 0) {
    output[0] = '\0';
  }
  return false;
}

static bool br_extract_double_key(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  const char *key,
  double *value
) {
  int value_index = br_json_find_key(json, tokens, token_count, object_index, key);
  if (value_index < 0) {
    return false;
  }
  return br_json_token_to_double(json, &tokens[value_index], value);
}

static void br_copy_text_local(const char *input, char *output, size_t output_size) {
  if (!output || output_size == 0) {
    return;
  }
  br_normalize_text(input, "", output, output_size);
}

static bool br_extract_bool_key(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  const char *key,
  bool *value
) {
  int value_index = br_json_find_key(json, tokens, token_count, object_index, key);
  if (value_index < 0) {
    return false;
  }
  return br_json_token_to_bool(json, &tokens[value_index], value);
}

static void br_assign_optional_text(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  const char *key,
  char *output,
  size_t output_size
) {
  char temp[128];
  if (br_extract_string_key(json, tokens, token_count, object_index, key, temp, sizeof(temp))) {
    br_normalize_text(temp, "", output, output_size);
  } else {
    output[0] = '\0';
  }
}

static bool br_is_allowed_action(const char *type) {
  static const char *allowed[] = {
    "tap",
    "long_press",
    "swipe_left",
    "swipe_right",
    "swipe_up",
    "swipe_down",
  };
  size_t total = sizeof(allowed) / sizeof(allowed[0]);
  for (size_t i = 0; i < total; i += 1) {
    if (strcmp(type, allowed[i]) == 0) {
      return true;
    }
  }
  return false;
}

static bool br_is_allowed_button_config_event(const char *event) {
  static const char *allowed[] = {
    "button.encoder.short_press",
    "button.encoder.long_press",
    "knob.rotate_cw / knob.rotate_ccw",
    "screen.region.tap",
    "screen.region.long_press",
  };
  size_t total = sizeof(allowed) / sizeof(allowed[0]);
  for (size_t i = 0; i < total; i += 1) {
    if (strcmp(event, allowed[i]) == 0) {
      return true;
    }
  }
  return false;
}

static bool br_is_allowed_button_config_action(const char *action) {
  static const char *allowed[] = {
    "voice_ptt",
    "system_page",
    "system_reset",
    "volume_adjust",
    "disabled",
  };
  size_t total = sizeof(allowed) / sizeof(allowed[0]);
  for (size_t i = 0; i < total; i += 1) {
    if (strcmp(action, allowed[i]) == 0) {
      return true;
    }
  }
  return false;
}

bool br_parse_input_action_json(
  const char *json_text,
  br_input_action *action,
  char *error,
  size_t error_size
) {
  br_json_token tokens[128];
  int count;

  if (error && error_size > 0) {
    error[0] = '\0';
  }
  if (!json_text || !action) {
    if (error && error_size > 0) {
      snprintf(error, error_size, "invalid_json");
    }
    return false;
  }

  memset(action, 0, sizeof(*action));
  count = br_json_parse(json_text, strlen(json_text), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) {
    if (error && error_size > 0) {
      snprintf(error, error_size, "invalid_json");
    }
    return false;
  }

  if (!br_extract_string_key(json_text, tokens, count, 0, "type", action->type, sizeof(action->type)) ||
      !br_is_allowed_action(action->type)) {
    if (error && error_size > 0) {
      snprintf(error, error_size, "invalid_input_action");
    }
    return false;
  }

  action->has_x = br_extract_double_key(json_text, tokens, count, 0, "x", &action->x);
  action->has_y = br_extract_double_key(json_text, tokens, count, 0, "y", &action->y);
  action->has_start_x = br_extract_double_key(json_text, tokens, count, 0, "startX", &action->start_x);
  action->has_start_y = br_extract_double_key(json_text, tokens, count, 0, "startY", &action->start_y);
  action->has_dx = br_extract_double_key(json_text, tokens, count, 0, "dx", &action->dx);
  action->has_dy = br_extract_double_key(json_text, tokens, count, 0, "dy", &action->dy);
  action->has_duration_ms = br_extract_double_key(json_text, tokens, count, 0, "durationMs", &action->duration_ms);
  action->has_viewport_width = br_extract_double_key(json_text, tokens, count, 0, "viewportWidth", &action->viewport_width);
  action->has_viewport_height = br_extract_double_key(json_text, tokens, count, 0, "viewportHeight", &action->viewport_height);

  br_assign_optional_text(json_text, tokens, count, 0, "view", action->view, sizeof(action->view));
  br_assign_optional_text(json_text, tokens, count, 0, "state", action->state, sizeof(action->state));
  br_assign_optional_text(json_text, tokens, count, 0, "targetState", action->target_state, sizeof(action->target_state));
  br_assign_optional_text(json_text, tokens, count, 0, "activeDetailId", action->active_detail_id, sizeof(action->active_detail_id));

  return true;
}

bool br_parse_button_config_command_json(const char *json_text, br_button_config_command *command) {
  br_json_token tokens[256];
  int count;
  char type[32];
  char request_id[96];
  double number = 0;
  int bindings_index;
  static const char *keys_voice_enabled[] = { "voice_enabled", "voiceEnabled" };

  if (!json_text || !command) {
    return false;
  }
  memset(command, 0, sizeof(*command));
  command->version = 1;
  br_copy_text_local(br_voice_button_default(), command->voice_button, sizeof(command->voice_button));

  count = br_json_parse(json_text, strlen(json_text), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) {
    return false;
  }
  if (!br_extract_string_key(json_text, tokens, count, 0, "type", type, sizeof(type)) ||
      strcmp(type, "button_config") != 0) {
    return false;
  }
  if (br_extract_double_key(json_text, tokens, count, 0, "version", &number)) {
    command->version = (int) number;
  }
  if (br_extract_string_key(json_text, tokens, count, 0, "request_id", request_id, sizeof(request_id)) ||
      br_extract_string_key(json_text, tokens, count, 0, "requestId", request_id, sizeof(request_id))) {
    br_copy_text_local(request_id, command->request_id, sizeof(command->request_id));
  }
  for (size_t i = 0; i < sizeof(keys_voice_enabled) / sizeof(keys_voice_enabled[0]); i += 1) {
    bool enabled = false;
    if (br_extract_bool_key(json_text, tokens, count, 0, keys_voice_enabled[i], &enabled)) {
      command->voice_enabled = enabled;
      break;
    }
  }
  {
    char voice_button[64];
    if (br_extract_string_key(json_text, tokens, count, 0, "voice_button", voice_button, sizeof(voice_button)) ||
        br_extract_string_key(json_text, tokens, count, 0, "voiceButton", voice_button, sizeof(voice_button))) {
      if (!br_voice_button_normalize(voice_button, command->voice_button, sizeof(command->voice_button))) {
        return false;
      }
    }
  }

  bindings_index = br_json_find_key(json_text, tokens, count, 0, "bindings");
  if (bindings_index < 0 || tokens[bindings_index].type != BR_JSON_ARRAY) {
    return false;
  }

  for (int i = bindings_index + 1; i < count; i += 1) {
    char event[96];
    char action[64];
    if (tokens[i].parent != bindings_index || tokens[i].type != BR_JSON_OBJECT) {
      continue;
    }
    if (command->binding_count >= BR_BUTTON_CONFIG_MAX_BINDINGS) {
      return false;
    }
    if (!br_extract_string_key(json_text, tokens, count, i, "event", event, sizeof(event)) ||
        !br_extract_string_key(json_text, tokens, count, i, "action", action, sizeof(action))) {
      return false;
    }
    if (!br_is_allowed_button_config_event(event) ||
        !br_is_allowed_button_config_action(action)) {
      return false;
    }
    br_copy_text_local(event,
                       command->bindings[command->binding_count].event,
                       sizeof(command->bindings[command->binding_count].event));
    br_copy_text_local(action,
                       command->bindings[command->binding_count].action,
                       sizeof(command->bindings[command->binding_count].action));
    command->binding_count += 1;
  }

  return command->version == 1 && command->binding_count > 0;
}

bool br_button_config_find_action_json(
  const char *json_text,
  const char *event,
  char *action,
  size_t action_size
) {
  br_button_config_command command;
  if (!action || action_size == 0) {
    return false;
  }
  action[0] = '\0';
  if (!event || !br_parse_button_config_command_json(json_text, &command)) {
    return false;
  }
  for (size_t i = 0; i < command.binding_count; i += 1) {
    if (strcmp(command.bindings[i].event, event) == 0) {
      br_copy_text_local(command.bindings[i].action, action, action_size);
      return action[0] != '\0';
    }
  }
  return false;
}

static void br_append_optional_number(char *output, size_t output_size, size_t *used, const char *key, bool present, double value) {
  if (!present) {
    return;
  }
  br_snprintf_append(output, output_size, used, ",\"%s\":%.3f", key, value);
}

static void br_append_optional_text(char *output, size_t output_size, size_t *used, const char *key, const char *value) {
  if (!value || *value == '\0') {
    return;
  }
  br_snprintf_append(output, output_size, used, ",\"%s\":\"", key);
  br_json_escape_append(output, output_size, used, value);
  br_snprintf_append(output, output_size, used, "\"");
}

int br_build_input_action_payload(
  const char *board_device_id,
  const char *local_device_id,
  const char *source,
  const br_input_action *action,
  long long ts_ms,
  char *output,
  size_t output_size
) {
  char iso[64];
  size_t used = 0;

  if (!board_device_id || !local_device_id || !source || !action || !output || output_size == 0) {
    return -1;
  }

  br_iso8601_now(iso, sizeof(iso));
  output[0] = '\0';
  if (br_snprintf_append(output, output_size, &used, "{") != 0) return -1;
  if (br_snprintf_append(output, output_size, &used, "\"boardDeviceId\":\"") != 0) return -1;
  br_json_escape_append(output, output_size, &used, board_device_id);
  if (br_snprintf_append(output, output_size, &used, "\",\"localDeviceId\":\"") != 0) return -1;
  br_json_escape_append(output, output_size, &used, local_device_id);
  if (br_snprintf_append(output, output_size, &used, "\",\"source\":\"") != 0) return -1;
  br_json_escape_append(output, output_size, &used, source);
  if (br_snprintf_append(output, output_size, &used, "\",\"type\":\"") != 0) return -1;
  br_json_escape_append(output, output_size, &used, action->type);
  if (br_snprintf_append(output, output_size, &used, "\",\"ts\":\"") != 0) return -1;
  br_json_escape_append(output, output_size, &used, iso);
  if (br_snprintf_append(output, output_size, &used, "\",\"tsMs\":%lld", ts_ms) != 0) return -1;
  br_append_optional_number(output, output_size, &used, "x", action->has_x, action->x);
  br_append_optional_number(output, output_size, &used, "y", action->has_y, action->y);
  br_append_optional_number(output, output_size, &used, "startX", action->has_start_x, action->start_x);
  br_append_optional_number(output, output_size, &used, "startY", action->has_start_y, action->start_y);
  br_append_optional_number(output, output_size, &used, "dx", action->has_dx, action->dx);
  br_append_optional_number(output, output_size, &used, "dy", action->has_dy, action->dy);
  br_append_optional_number(output, output_size, &used, "durationMs", action->has_duration_ms, action->duration_ms);
  br_append_optional_number(output, output_size, &used, "viewportWidth", action->has_viewport_width, action->viewport_width);
  br_append_optional_number(output, output_size, &used, "viewportHeight", action->has_viewport_height, action->viewport_height);
  br_append_optional_text(output, output_size, &used, "view", action->view);
  br_append_optional_text(output, output_size, &used, "state", action->state);
  br_append_optional_text(output, output_size, &used, "targetState", action->target_state);
  br_append_optional_text(output, output_size, &used, "activeDetailId", action->active_detail_id);
  if (br_snprintf_append(output, output_size, &used, "}") != 0) return -1;
  return 0;
}

bool br_parse_remote_binding_json(const char *json_text, br_remote_binding *binding) {
  br_json_token tokens[64];
  int count;
  bool enabled = false;
  char command[64];

  if (!json_text || !binding) {
    return false;
  }
  memset(binding, 0, sizeof(*binding));

  count = br_json_parse(json_text, strlen(json_text), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) {
    return false;
  }

  if (!br_extract_string_key(json_text, tokens, count, 0, "command", command, sizeof(command))) {
    return false;
  }
  if (strcmp(command, "remote_cli_binding.update") != 0) {
    return false;
  }
  if (!br_extract_bool_key(json_text, tokens, count, 0, "enabled", &enabled) || !enabled) {
    return false;
  }
  if (!br_extract_string_key(json_text, tokens, count, 0, "targetDeviceId", binding->target_device_id, sizeof(binding->target_device_id))) {
    return false;
  }
  br_normalize_topic_part(binding->target_device_id, "", binding->target_device_id, sizeof(binding->target_device_id));
  br_assign_optional_text(json_text, tokens, count, 0, "targetSource", binding->target_source, sizeof(binding->target_source));
  br_normalize_topic_part(binding->target_source, "", binding->target_source, sizeof(binding->target_source));
  binding->matched = binding->target_device_id[0] != '\0';
  return binding->matched;
}

static void br_speech_compose_text(br_speech_update *update) {
  if (!update) {
    return;
  }
  if (update->title[0] && update->body[0] && strcmp(update->title, update->body) != 0) {
    snprintf(update->text, sizeof(update->text), "%s\n%s", update->title, update->body);
  } else {
    br_normalize_text(update->title[0] ? update->title : update->body, "", update->text, sizeof(update->text));
  }
}

bool br_parse_speech_update(const char *json_text, br_speech_update *update) {
  br_json_token tokens[128];
  int count;

  if (!json_text || !update) {
    return false;
  }
  memset(update, 0, sizeof(*update));

  count = br_json_parse(json_text, strlen(json_text), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count > 0 && tokens[0].type == BR_JSON_OBJECT) {
    int object_index = 0;
    int payload_index = br_json_find_key(json_text, tokens, count, 0, "payload");
    const char *const title_keys[] = {
      "displayTitle",
      "sessionTitle",
      "sessionName",
      "title",
      "name",
    };
    const char *const body_keys[] = {
      "displayContent",
      "content",
      "text",
      "body",
      "summary",
      "statusText",
      "displayStatus",
    };
    bool has_title;
    bool has_body;
    double number = 0;

    if (payload_index >= 0 && tokens[payload_index].type == BR_JSON_OBJECT) {
      object_index = payload_index;
    }

    br_assign_optional_text(json_text, tokens, count, object_index, "source", update->source, sizeof(update->source));
    br_assign_optional_text(json_text, tokens, count, object_index, "sessionId", update->session_id, sizeof(update->session_id));
    br_assign_optional_text(json_text, tokens, count, object_index, "runId", update->run_id, sizeof(update->run_id));
    br_assign_optional_text(json_text, tokens, count, object_index, "sessionKey", update->session_key, sizeof(update->session_key));
    if (br_extract_double_key(json_text, tokens, count, object_index, "tsMs", &number)) {
      update->payload_ts_ms = (long long) number;
      update->has_payload_ts_ms = true;
    }
    if (br_extract_double_key(json_text, tokens, count, object_index, "expiresAtMs", &number)) {
      update->expires_at_ms = (long long) number;
      update->has_expires_at_ms = true;
    }

    has_title = br_extract_first_string_key(
      json_text, tokens, count, object_index, title_keys, sizeof(title_keys) / sizeof(title_keys[0]),
      update->title, sizeof(update->title)
    );
    has_body = br_extract_first_string_key(
      json_text, tokens, count, object_index, body_keys, sizeof(body_keys) / sizeof(body_keys[0]),
      update->body, sizeof(update->body)
    );

    if (has_title || has_body) {
      br_speech_compose_text(update);
      return update->text[0] != '\0';
    }

    if (br_extract_string_key(json_text, tokens, count, object_index, "displayContent", update->text, sizeof(update->text)) ||
        br_extract_string_key(json_text, tokens, count, object_index, "content", update->text, sizeof(update->text)) ||
        br_extract_string_key(json_text, tokens, count, object_index, "text", update->text, sizeof(update->text))) {
      br_normalize_text(update->text, "", update->text, sizeof(update->text));
      return true;
    }
  }

  return br_normalize_text(json_text, "", update->text, sizeof(update->text));
}

bool br_parse_speech_text(const char *json_text, char *output, size_t output_size) {
  br_speech_update update;

  if (!output || output_size == 0) {
    return false;
  }
  output[0] = '\0';
  if (!br_parse_speech_update(json_text, &update)) {
    return false;
  }
  snprintf(output, output_size, "%s", update.text);
  return output[0] != '\0';
}

bool br_build_message_complete_json(
  const char *json_text,
  const char *fallback_text,
  char *output,
  size_t output_size
) {
  char response[1024];
  size_t used = 0;
  const char *effective = fallback_text ? fallback_text : "";

  if (!output || output_size == 0) {
    return false;
  }
  if (json_text && br_parse_speech_text(json_text, response, sizeof(response))) {
    effective = response;
  }

  output[0] = '\0';
  if (br_snprintf_append(output, output_size, &used, "{\"type\":\"message_complete\",\"source\":\"mqtt-speech\",\"sender\":\"Devon\",\"response\":\"") != 0) {
    return false;
  }
  br_json_escape_append(output, output_size, &used, effective);
  return br_snprintf_append(output, output_size, &used, "\"}") == 0;
}

static bool br_extract_long_long_with_aliases(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  const char *const *keys,
  size_t key_count,
  long long *value
) {
  for (size_t i = 0; i < key_count; i += 1) {
    double temp = 0;
    if (br_extract_double_key(json, tokens, token_count, object_index, keys[i], &temp)) {
      *value = (long long) temp;
      return true;
    }
  }
  return false;
}

static bool br_extract_double_with_aliases(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  const char *const *keys,
  size_t key_count,
  double *value
) {
  for (size_t i = 0; i < key_count; i += 1) {
    if (br_extract_double_key(json, tokens, token_count, object_index, keys[i], value)) {
      return true;
    }
  }
  return false;
}

static bool br_assign_optional_string_with_aliases(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  const char *const *keys,
  size_t key_count,
  char *output,
  size_t output_size
) {
  return br_extract_first_string_key(json, tokens, token_count, object_index,
                                     keys, key_count, output, output_size);
}

bool br_parse_audio_bridge_command_json(const char *json_text, br_audio_bridge_command *command) {
  br_json_token tokens[96];
  int count;
  char type[32];
  char action[16];
  char voice_button[64];
  char optional_text[128];
  double number = 0;
  static const char *keys_pc_ip[] = { "pc_ip", "pcIp" };
  static const char *keys_pc_port[] = { "pc_port", "pcPort" };
  static const char *keys_listen_port[] = { "listen_port", "listenPort" };
  static const char *keys_capture_dev[] = { "capture_dev", "captureDev" };
  static const char *keys_play_dev[] = { "play_dev", "playDev" };
  static const char *keys_voice_button[] = { "voice_button", "voiceButton" };

  if (!json_text || !command) {
    return false;
  }
  memset(command, 0, sizeof(*command));
  count = br_json_parse(json_text, strlen(json_text), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) {
    return false;
  }
  if (!br_extract_string_key(json_text, tokens, count, 0, "type", type, sizeof(type)) ||
      strcmp(type, "audio_bridge") != 0) {
    return false;
  }
  if (!br_extract_string_key(json_text, tokens, count, 0, "action", action, sizeof(action))) {
    return false;
  }
  if (strcmp(action, "start") != 0 && strcmp(action, "stop") != 0) {
    return false;
  }

  br_copy_text_local(action, command->action, sizeof(command->action));
  command->enabled = strcmp(action, "start") == 0;
  command->pc_port = 50001;
  command->listen_port = 50002;
  br_copy_text_local("default", command->capture_dev, sizeof(command->capture_dev));
  br_copy_text_local("default", command->play_dev, sizeof(command->play_dev));
  br_copy_text_local(br_voice_button_default(), command->voice_button, sizeof(command->voice_button));

  if (command->enabled &&
      !br_assign_optional_string_with_aliases(json_text, tokens, count, 0,
                                              keys_pc_ip, sizeof(keys_pc_ip) / sizeof(keys_pc_ip[0]),
                                              command->pc_ip, sizeof(command->pc_ip))) {
    return false;
  }
  if (br_extract_double_with_aliases(json_text, tokens, count, 0,
                                     keys_pc_port, sizeof(keys_pc_port) / sizeof(keys_pc_port[0]),
                                     &number)) {
    command->pc_port = (int) number;
  }
  if (br_extract_double_with_aliases(json_text, tokens, count, 0,
                                     keys_listen_port, sizeof(keys_listen_port) / sizeof(keys_listen_port[0]),
                                     &number)) {
    command->listen_port = (int) number;
  }
  if (br_assign_optional_string_with_aliases(json_text, tokens, count, 0,
                                             keys_capture_dev, sizeof(keys_capture_dev) / sizeof(keys_capture_dev[0]),
                                             optional_text, sizeof(optional_text))) {
    br_copy_text_local(optional_text, command->capture_dev, sizeof(command->capture_dev));
  }
  if (br_assign_optional_string_with_aliases(json_text, tokens, count, 0,
                                             keys_play_dev, sizeof(keys_play_dev) / sizeof(keys_play_dev[0]),
                                             optional_text, sizeof(optional_text))) {
    br_copy_text_local(optional_text, command->play_dev, sizeof(command->play_dev));
  }
  if (br_assign_optional_string_with_aliases(json_text, tokens, count, 0,
                                             keys_voice_button, sizeof(keys_voice_button) / sizeof(keys_voice_button[0]),
                                             voice_button, sizeof(voice_button))) {
    if (!br_voice_button_normalize(voice_button, command->voice_button, sizeof(command->voice_button))) {
      return false;
    }
  }

  if (command->enabled &&
      (command->pc_ip[0] == '\0' ||
       command->pc_port <= 0 || command->pc_port > 65535 ||
       command->listen_port <= 0 || command->listen_port > 65535)) {
    return false;
  }
  return true;
}

static bool br_extract_token_usage(
  const char *json,
  const br_json_token *tokens,
  int token_count,
  int object_index,
  br_token_usage *usage
) {
  static const char *keys_total[] = { "totalTokens", "total_tokens" };
  static const char *keys_input[] = { "inputTokens", "input_tokens", "promptTokens", "prompt_tokens" };
  static const char *keys_output[] = { "outputTokens", "output_tokens", "completionTokens", "completion_tokens" };
  static const char *keys_cached[] = {
    "cachedInputTokens", "cached_input_tokens",
    "cacheReadInputTokens", "cache_read_input_tokens"
  };
  static const char *keys_cache_create[] = {
    "cacheCreationInputTokens", "cache_creation_input_tokens"
  };
  static const char *keys_reasoning[] = {
    "reasoningOutputTokens", "reasoning_output_tokens",
    "reasoningTokens", "reasoning_tokens"
  };
  static const char *keys_last_total[] = {
    "lastTotalTokens", "last_total_tokens",
    "deltaTotalTokens", "delta_total_tokens"
  };
  static const char *keys_last_input[] = {
    "lastInputTokens", "last_input_tokens",
    "deltaInputTokens", "delta_input_tokens"
  };
  static const char *keys_last_output[] = {
    "lastOutputTokens", "last_output_tokens",
    "deltaOutputTokens", "delta_output_tokens"
  };
  static const char *keys_last_cached[] = {
    "lastCachedInputTokens", "last_cached_input_tokens",
    "deltaCachedInputTokens", "delta_cached_input_tokens"
  };
  static const char *keys_last_cache_create[] = {
    "lastCacheCreationInputTokens", "last_cache_creation_input_tokens",
    "deltaCacheCreationInputTokens", "delta_cache_creation_input_tokens"
  };
  static const char *keys_last_reasoning[] = {
    "lastReasoningOutputTokens", "last_reasoning_output_tokens",
    "deltaReasoningOutputTokens", "delta_reasoning_output_tokens"
  };
  static const char *keys_cost[] = {
    "estimatedCostUsd", "estimated_cost_usd", "costUsd", "cost_usd"
  };

  if (object_index < 0 || object_index >= token_count) {
    return false;
  }
  if (tokens[object_index].type != BR_JSON_OBJECT) {
    return false;
  }

  memset(usage, 0, sizeof(*usage));
  usage->has_total_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_total, sizeof(keys_total) / sizeof(keys_total[0]),
    &usage->total_tokens);
  usage->has_input_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_input, sizeof(keys_input) / sizeof(keys_input[0]),
    &usage->input_tokens);
  usage->has_output_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_output, sizeof(keys_output) / sizeof(keys_output[0]),
    &usage->output_tokens);
  usage->has_cached_input_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_cached, sizeof(keys_cached) / sizeof(keys_cached[0]),
    &usage->cached_input_tokens);
  usage->has_cache_creation_input_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_cache_create,
    sizeof(keys_cache_create) / sizeof(keys_cache_create[0]),
    &usage->cache_creation_input_tokens);
  usage->has_reasoning_output_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_reasoning,
    sizeof(keys_reasoning) / sizeof(keys_reasoning[0]),
    &usage->reasoning_output_tokens);
  usage->has_last_total_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_last_total,
    sizeof(keys_last_total) / sizeof(keys_last_total[0]),
    &usage->last_total_tokens);
  usage->has_last_input_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_last_input,
    sizeof(keys_last_input) / sizeof(keys_last_input[0]),
    &usage->last_input_tokens);
  usage->has_last_output_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_last_output,
    sizeof(keys_last_output) / sizeof(keys_last_output[0]),
    &usage->last_output_tokens);
  usage->has_last_cached_input_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_last_cached,
    sizeof(keys_last_cached) / sizeof(keys_last_cached[0]),
    &usage->last_cached_input_tokens);
  usage->has_last_cache_creation_input_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_last_cache_create,
    sizeof(keys_last_cache_create) / sizeof(keys_last_cache_create[0]),
    &usage->last_cache_creation_input_tokens);
  usage->has_last_reasoning_output_tokens = br_extract_long_long_with_aliases(
    json, tokens, token_count, object_index, keys_last_reasoning,
    sizeof(keys_last_reasoning) / sizeof(keys_last_reasoning[0]),
    &usage->last_reasoning_output_tokens);
  usage->has_estimated_cost_usd = br_extract_double_with_aliases(
    json, tokens, token_count, object_index, keys_cost, sizeof(keys_cost) / sizeof(keys_cost[0]),
    &usage->estimated_cost_usd);

  if (!usage->has_total_tokens) {
    long long derived = 0;
    if (usage->has_input_tokens) derived += usage->input_tokens;
    if (usage->has_cached_input_tokens) derived += usage->cached_input_tokens;
    if (usage->has_cache_creation_input_tokens) derived += usage->cache_creation_input_tokens;
    if (usage->has_output_tokens) derived += usage->output_tokens;
    if (derived > 0) {
      usage->total_tokens = derived;
      usage->has_total_tokens = true;
    }
  }

  return usage->has_total_tokens
      || usage->has_input_tokens
      || usage->has_output_tokens
      || usage->has_cached_input_tokens
      || usage->has_cache_creation_input_tokens
      || usage->has_reasoning_output_tokens
      || usage->has_last_total_tokens
      || usage->has_last_input_tokens
      || usage->has_last_output_tokens
      || usage->has_last_cached_input_tokens
      || usage->has_last_cache_creation_input_tokens
      || usage->has_last_reasoning_output_tokens
      || usage->has_estimated_cost_usd;
}

static bool br_canonical_state_from_event(const char *event, char *output, size_t output_size) {
  if (!event || event[0] == '\0') {
    return false;
  }
  if (strcmp(event, "UserPromptSubmit") == 0 ||
      strcmp(event, "PreToolUse") == 0 ||
      strcmp(event, "SubagentStart") == 0 ||
      strcmp(event, "PreCompact") == 0 ||
      strcmp(event, "WorktreeCreate") == 0 ||
      strcmp(event, "PostToolUse") == 0 ||
      strcmp(event, "SubagentStop") == 0) {
    br_copy_text_local("working", output, output_size);
    return true;
  }
  if (strcmp(event, "AssistantMessage") == 0 ||
      strcmp(event, "Stop") == 0 ||
      strcmp(event, "PostCompact") == 0) {
    br_copy_text_local("done", output, output_size);
    return true;
  }
  if (strcmp(event, "PostToolUseFailure") == 0 ||
      strcmp(event, "StopFailure") == 0) {
    br_copy_text_local("error", output, output_size);
    return true;
  }
  if (strcmp(event, "Elicitation") == 0 ||
      strcmp(event, "PermissionRequest") == 0 ||
      strcmp(event, "Notification") == 0) {
    br_copy_text_local("waiting_user", output, output_size);
    return true;
  }
  if (strcmp(event, "SessionStart") == 0 ||
      strcmp(event, "SessionEnd") == 0) {
    br_copy_text_local("idle", output, output_size);
    return true;
  }
  return false;
}

static bool br_canonical_state_from_value(const char *value, char *output, size_t output_size) {
  if (!value || value[0] == '\0') {
    return false;
  }
  if (strcmp(value, "idle") == 0) {
    br_copy_text_local("idle", output, output_size);
    return true;
  }
  if (strcmp(value, "working") == 0 ||
      strcmp(value, "active") == 0 ||
      strcmp(value, "thinking") == 0 ||
      strcmp(value, "tool_running") == 0 ||
      strcmp(value, "speaking") == 0) {
    br_copy_text_local("working", output, output_size);
    return true;
  }
  if (strcmp(value, "waiting_user") == 0 ||
      strcmp(value, "notification") == 0) {
    br_copy_text_local("waiting_user", output, output_size);
    return true;
  }
  if (strcmp(value, "done") == 0) {
    br_copy_text_local("done", output, output_size);
    return true;
  }
  if (strcmp(value, "error") == 0) {
    br_copy_text_local("error", output, output_size);
    return true;
  }
  return false;
}

bool br_bridge_state_from_message(
  const char *topic,
  const char *payload_text,
  br_bridge_state_update *update
) {
  br_json_token tokens[128];
  int count;
  int object_index = 0;
  char state[64] = "";
  char reason[64] = "";

  (void) topic;

  if (!payload_text || !update) {
    return false;
  }
  memset(update, 0, sizeof(*update));
  update->allow_interrupt = true;

  count = br_json_parse(payload_text, strlen(payload_text), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count > 0 && tokens[0].type == BR_JSON_OBJECT) {
    int payload_index = br_json_find_key(payload_text, tokens, count, 0, "payload");
    if (payload_index >= 0 && tokens[payload_index].type == BR_JSON_OBJECT) {
      object_index = payload_index;
    }
    br_assign_optional_text(payload_text, tokens, count, object_index, "state", state, sizeof(state));
    if (state[0] == '\0') {
      br_assign_optional_text(payload_text, tokens, count, object_index, "rawState", state, sizeof(state));
    }
    br_assign_optional_text(payload_text, tokens, count, object_index, "reason", reason, sizeof(reason));
    br_copy_text_local(reason, update->reason, sizeof(update->reason));
    br_assign_optional_text(payload_text, tokens, count, object_index, "event", update->event, sizeof(update->event));
    br_assign_optional_text(payload_text, tokens, count, object_index, "source", update->source, sizeof(update->source));
    br_assign_optional_text(payload_text, tokens, count, object_index, "sessionId", update->session_id, sizeof(update->session_id));
    br_assign_optional_text(payload_text, tokens, count, object_index, "runId", update->run_id, sizeof(update->run_id));
    br_assign_optional_text(payload_text, tokens, count, object_index, "sessionKey", update->session_key, sizeof(update->session_key));
    if (!br_extract_bool_key(payload_text, tokens, count, object_index, "screenInterrupt", &update->allow_interrupt)) {
      (void) br_extract_bool_key(payload_text, tokens, count, object_index, "allowInterrupt", &update->allow_interrupt);
    }
    {
      double ts_ms = 0;
      if (br_extract_double_key(payload_text, tokens, count, object_index, "tsMs", &ts_ms)) {
        update->payload_ts_ms = (long long) ts_ms;
        update->has_payload_ts_ms = true;
      }
    }
    if (state[0] == '\0' && strcmp(reason, "active.no_sources") == 0) {
      br_copy_text_local("idle", state, sizeof(state));
    }
    {
      int token_usage_index = br_json_find_key(payload_text, tokens, count, object_index, "tokenUsage");
      if (token_usage_index < 0) {
        token_usage_index = br_json_find_key(payload_text, tokens, count, object_index, "token_usage");
      }
      if (token_usage_index < 0 && object_index == 0) {
        int wrapped = br_json_find_key(payload_text, tokens, count, 0, "payload");
        if (wrapped >= 0 && tokens[wrapped].type == BR_JSON_OBJECT) {
          token_usage_index = br_json_find_key(payload_text, tokens, count, wrapped, "tokenUsage");
          if (token_usage_index < 0) {
            token_usage_index = br_json_find_key(payload_text, tokens, count, wrapped, "token_usage");
          }
        }
      }
      if (token_usage_index >= 0 && tokens[token_usage_index].type == BR_JSON_OBJECT) {
        if (br_extract_token_usage(payload_text, tokens, count, token_usage_index, &update->token_usage)) {
          update->has_token_usage = true;
        }
      }
    }
    if (br_canonical_state_from_event(update->event, update->state, sizeof(update->state)) ||
        br_canonical_state_from_value(state, update->state, sizeof(update->state))) {
      update->should_write = true;
      return true;
    }
  }

  if (br_normalize_text(payload_text, "", state, sizeof(state)) &&
      br_canonical_state_from_value(state, update->state, sizeof(update->state))) {
    update->should_write = true;
    return true;
  }
  return false;
}

bool br_payload_to_json_object(
  const char *payload_text,
  char *output,
  size_t output_size,
  long long *payload_ts_ms,
  char *source,
  size_t source_size
) {
  br_json_token tokens[128];
  int count;
  if (payload_ts_ms) {
    *payload_ts_ms = 0;
  }
  if (source && source_size > 0) {
    source[0] = '\0';
  }

  if (!payload_text || !output || output_size == 0) {
    return false;
  }
  count = br_json_parse(payload_text, strlen(payload_text), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count > 0 && tokens[0].type == BR_JSON_OBJECT) {
    if (!br_json_copy_raw(payload_text, &tokens[0], output, output_size)) {
      return false;
    }
    if (payload_ts_ms) {
      double temp = 0;
      if (br_extract_double_key(payload_text, tokens, count, 0, "tsMs", &temp)) {
        *payload_ts_ms = (long long) temp;
      }
    }
    if (source && source_size > 0) {
      br_assign_optional_text(payload_text, tokens, count, 0, "source", source, source_size);
    }
    return true;
  }

  size_t used = 0;
  output[0] = '\0';
  if (br_snprintf_append(output, output_size, &used, "{\"text\":\"") != 0) {
    return false;
  }
  br_json_escape_append(output, output_size, &used, payload_text);
  return br_snprintf_append(output, output_size, &used, "\"}") == 0;
}
