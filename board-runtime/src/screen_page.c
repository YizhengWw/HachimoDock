#include "screen_page.h"

#include <stdio.h>
#include <string.h>

#include "runtime_common.h"
#include "runtime_json.h"

const char *br_screen_page_default_page(void) {
  return "main";
}

bool br_screen_page_resolve(const char *input, char *out, size_t out_size) {
  if (!input || !out || out_size == 0) return false;
  while (*input && (*input == ' ' || *input == '\t' || *input == '\n' || *input == '\r')) {
    input += 1;
  }
  if (*input == '\0') return false;

  char value[64] = "";
  if (*input == '{') {
    br_json_token tokens[16];
    int count = br_json_parse(input, strlen(input), tokens,
                              (int) (sizeof(tokens) / sizeof(tokens[0])));
    if (count > 0 && tokens[0].type == BR_JSON_OBJECT) {
      int idx = br_json_find_key(input, tokens, count, 0, "page");
      if (idx >= 0 && tokens[idx].type == BR_JSON_STRING) {
        if (!br_json_token_to_string(input, &tokens[idx], value, sizeof(value))) {
          return false;
        }
      }
    }
  } else {
    snprintf(value, sizeof(value), "%s", input);
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r'
                       || value[len - 1] == ' ' || value[len - 1] == '\t')) {
      value[--len] = '\0';
    }
  }

  if (value[0] == '\0') return false;
  if (strstr(value, "\xE2\x9E\xA1") != NULL || strstr(value, "\xE2\x9E\x9C") != NULL) {
    return br_normalize_text("stats", "", out, out_size);
  }
  if (strcmp(value, "stats") == 0 || strcmp(value, "0") == 0) {
    return br_normalize_text("stats", "", out, out_size);
  }
  if (strcmp(value, "main") == 0 || strcmp(value, "home") == 0 || strcmp(value, "1") == 0) {
    return br_normalize_text("main", "", out, out_size);
  }
  return false;
}

bool br_screen_page_toggle_main_stats(const char *current_page, char *output, size_t output_size) {
  if (!output || output_size == 0) {
    return false;
  }
  if (current_page && strcmp(current_page, "stats") == 0) {
    return br_normalize_text("main", "", output, output_size);
  }
  return br_normalize_text("stats", "", output, output_size);
}

bool br_screen_page_touch_action_should_toggle(const char *action_type) {
  return action_type &&
         (strcmp(action_type, "swipe_left") == 0 ||
          strcmp(action_type, "swipe_right") == 0 ||
          strcmp(action_type, "swipe_up") == 0 ||
          strcmp(action_type, "swipe_down") == 0);
}

int br_button_press_resolve_threshold_ms(int configured_ms, int default_ms) {
  return configured_ms > 0 ? configured_ms : default_ms;
}

br_button_press_kind br_button_press_classify(long long duration_ms, int long_press_ms) {
  int threshold_ms = br_button_press_resolve_threshold_ms(long_press_ms, 1500);
  return duration_ms >= threshold_ms ? BR_BUTTON_PRESS_LONG : BR_BUTTON_PRESS_SHORT;
}

br_primary_button_action br_primary_button_resolve_action(
  br_button_press_kind press_kind,
  bool active_widget_on_stats
) {
  (void) active_widget_on_stats;
  return press_kind == BR_BUTTON_PRESS_LONG
    ? BR_PRIMARY_BUTTON_RESTART_RUNTIME
    : BR_PRIMARY_BUTTON_TOGGLE_PAGE;
}
