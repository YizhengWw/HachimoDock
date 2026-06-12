#ifndef BOARD_RUNTIME_SCREEN_PAGE_H
#define BOARD_RUNTIME_SCREEN_PAGE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  BR_BUTTON_PRESS_SHORT = 0,
  BR_BUTTON_PRESS_LONG = 1
} br_button_press_kind;

typedef enum {
  BR_PRIMARY_BUTTON_TOGGLE_PAGE = 0,
  BR_PRIMARY_BUTTON_RESTART_RUNTIME = 1
} br_primary_button_action;

const char *br_screen_page_default_page(void);
bool br_screen_page_resolve(const char *input, char *out, size_t out_size);
bool br_screen_page_toggle_main_stats(const char *current_page, char *output, size_t output_size);
bool br_screen_page_touch_action_should_toggle(const char *action_type);
int br_button_press_resolve_threshold_ms(int configured_ms, int default_ms);
br_button_press_kind br_button_press_classify(long long duration_ms, int long_press_ms);
br_primary_button_action br_primary_button_resolve_action(
  br_button_press_kind press_kind,
  bool active_widget_on_stats
);

#endif
