#include "runtime_debug.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "runtime_json.h"

static void br_debug_join(const char *root_dir, const char *name, char *output, size_t output_size) {
  snprintf(output, output_size, "%s/%s", root_dir && root_dir[0] ? root_dir : ".", name);
}

void br_debug_overlay_flag_path(const char *root_dir, char *output, size_t output_size) {
  br_debug_join(root_dir, ".debug-overlay-enabled", output, output_size);
}

void br_debug_session_snapshot_path(const char *root_dir, char *output, size_t output_size) {
  br_debug_join(root_dir, ".debug-session-state.json", output, output_size);
}

void br_debug_screen_snapshot_path(const char *root_dir, char *output, size_t output_size) {
  br_debug_join(root_dir, ".debug-screen-state.json", output, output_size);
}

bool br_debug_parse_overlay_toggle_json(const char *json_text, bool *enabled) {
  br_json_token tokens[32];
  int count;
  int index;
  if (!json_text || !enabled) {
    return false;
  }
  count = br_json_parse(json_text, strlen(json_text), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) {
    return false;
  }
  index = br_json_find_key(json_text, tokens, count, 0, "enabled");
  if (index < 0) {
    return false;
  }
  return br_json_token_to_bool(json_text, &tokens[index], enabled);
}

bool br_debug_overlay_enabled(const char *root_dir) {
  char path[BR_MAX_PATH];
  char text[32];
  br_debug_overlay_flag_path(root_dir, path, sizeof(path));
  if (!br_read_text_file(path, text, sizeof(text))) {
    return false;
  }
  br_trim(text);
  return strcmp(text, "0") != 0 && strcmp(text, "false") != 0;
}

bool br_debug_set_overlay_enabled(const char *root_dir, bool enabled) {
  char path[BR_MAX_PATH];
  br_debug_overlay_flag_path(root_dir, path, sizeof(path));
  if (enabled) {
    return br_atomic_write_text(path, "1");
  }
  if (unlink(path) == 0) {
    return true;
  }
  if (errno == ENOENT) {
    return true;
  }
  return br_atomic_write_text(path, "0");
}

static bool br_debug_read_json_file(const char *path, char *output, size_t output_size) {
  if (!br_read_text_file(path, output, output_size)) {
    snprintf(output, output_size, "null");
    return false;
  }
  br_trim(output);
  if (output[0] != '{') {
    snprintf(output, output_size, "null");
    return false;
  }
  return true;
}

bool br_debug_build_state_json(const char *root_dir, char *output, size_t output_size) {
  char session_path[BR_MAX_PATH];
  char screen_path[BR_MAX_PATH];
  char session_json[BR_MAX_JSON];
  char screen_json[BR_MAX_JSON];
  size_t used = 0;
  if (!output || output_size == 0) {
    return false;
  }
  br_debug_session_snapshot_path(root_dir, session_path, sizeof(session_path));
  br_debug_screen_snapshot_path(root_dir, screen_path, sizeof(screen_path));
  br_debug_read_json_file(session_path, session_json, sizeof(session_json));
  br_debug_read_json_file(screen_path, screen_json, sizeof(screen_json));
  output[0] = '\0';
  br_snprintf_append(output, output_size, &used, "{\"overlayEnabled\":%s,\"session\":%s,\"screen\":%s}",
                     br_debug_overlay_enabled(root_dir) ? "true" : "false",
                     session_json,
                     screen_json);
  return used < output_size;
}
