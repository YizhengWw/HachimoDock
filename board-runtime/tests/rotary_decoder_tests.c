#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "rotary_decoder.h"

static void clockwise_rotation_triggers_next_page(void) {
  br_rotary_decoder decoder;
  br_rotary_decoder_init(&decoder, 1, 1, 4);

  assert(br_rotary_decoder_update(&decoder, 0, 1) == BR_ROTARY_NONE);
  assert(br_rotary_decoder_update(&decoder, 0, 0) == BR_ROTARY_NONE);
  assert(br_rotary_decoder_update(&decoder, 1, 0) == BR_ROTARY_NONE);
  assert(br_rotary_decoder_update(&decoder, 1, 1) == BR_ROTARY_CLOCKWISE);
}

static void counter_clockwise_rotation_triggers_previous_page(void) {
  br_rotary_decoder decoder;
  br_rotary_decoder_init(&decoder, 1, 1, 4);

  assert(br_rotary_decoder_update(&decoder, 1, 0) == BR_ROTARY_NONE);
  assert(br_rotary_decoder_update(&decoder, 0, 0) == BR_ROTARY_NONE);
  assert(br_rotary_decoder_update(&decoder, 0, 1) == BR_ROTARY_NONE);
  assert(br_rotary_decoder_update(&decoder, 1, 1) == BR_ROTARY_COUNTER_CLOCKWISE);
}

static void invalid_jump_is_ignored(void) {
  br_rotary_decoder decoder;
  br_rotary_decoder_init(&decoder, 1, 1, 4);

  assert(br_rotary_decoder_update(&decoder, 0, 0) == BR_ROTARY_NONE);
  assert(br_rotary_decoder_update(&decoder, 1, 1) == BR_ROTARY_NONE);
}

static void toggle_mode_switches_to_other_page(void) {
  char page[32];

  assert(br_rotary_select_page("main", "stats", "main", true, BR_ROTARY_CLOCKWISE, page, sizeof(page)));
  assert(strcmp(page, "stats") == 0);
  assert(br_rotary_select_page("stats", "stats", "main", true, BR_ROTARY_CLOCKWISE, page, sizeof(page)));
  assert(strcmp(page, "main") == 0);
  assert(br_rotary_select_page("stats", "stats", "main", true, BR_ROTARY_COUNTER_CLOCKWISE, page, sizeof(page)));
  assert(strcmp(page, "main") == 0);
}

static void direction_mode_keeps_directional_pages(void) {
  char page[32];

  assert(br_rotary_select_page("stats", "stats", "main", false, BR_ROTARY_CLOCKWISE, page, sizeof(page)));
  assert(strcmp(page, "stats") == 0);
  assert(br_rotary_select_page("stats", "stats", "main", false, BR_ROTARY_COUNTER_CLOCKWISE, page, sizeof(page)));
  assert(strcmp(page, "main") == 0);
}

int main(void) {
  clockwise_rotation_triggers_next_page();
  counter_clockwise_rotation_triggers_previous_page();
  invalid_jump_is_ignored();
  toggle_mode_switches_to_other_page();
  direction_mode_keeps_directional_pages();
  return 0;
}
