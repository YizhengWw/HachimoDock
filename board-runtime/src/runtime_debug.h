#ifndef BOARD_RUNTIME_DEBUG_H
#define BOARD_RUNTIME_DEBUG_H

#include <stdbool.h>
#include <stddef.h>

#include "runtime_common.h"

void br_debug_overlay_flag_path(const char *root_dir, char *output, size_t output_size);
void br_debug_session_snapshot_path(const char *root_dir, char *output, size_t output_size);
void br_debug_screen_snapshot_path(const char *root_dir, char *output, size_t output_size);
bool br_debug_parse_overlay_toggle_json(const char *json_text, bool *enabled);
bool br_debug_overlay_enabled(const char *root_dir);
bool br_debug_set_overlay_enabled(const char *root_dir, bool enabled);
bool br_debug_build_state_json(const char *root_dir, char *output, size_t output_size);

#endif
