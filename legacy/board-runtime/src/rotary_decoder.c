#include "rotary_decoder.h"

#include <stdlib.h>
#include <string.h>

static int br_rotary_state(int a_level, int b_level) {
  return ((a_level ? 1 : 0) << 1) | (b_level ? 1 : 0);
}

static int br_rotary_transition_delta(int previous_state, int current_state) {
  int transition = (previous_state << 2) | current_state;

  switch (transition) {
    case 0xD: /* 11 -> 01: A falls before B, clockwise for the wired module. */
    case 0x4: /* 01 -> 00 */
    case 0x2: /* 00 -> 10 */
    case 0xB: /* 10 -> 11 */
      return 1;
    case 0xE: /* 11 -> 10 */
    case 0x8: /* 10 -> 00 */
    case 0x1: /* 00 -> 01 */
    case 0x7: /* 01 -> 11 */
      return -1;
    default:
      return 0;
  }
}

void br_rotary_decoder_init(br_rotary_decoder *decoder, int a_level, int b_level, int trigger_steps) {
  if (!decoder) {
    return;
  }
  decoder->previous_state = br_rotary_state(a_level, b_level);
  decoder->accumulated_steps = 0;
  decoder->trigger_steps = trigger_steps > 0 ? trigger_steps : 4;
}

br_rotary_direction br_rotary_decoder_update(br_rotary_decoder *decoder, int a_level, int b_level) {
  int current_state;
  int delta;

  if (!decoder) {
    return BR_ROTARY_NONE;
  }

  current_state = br_rotary_state(a_level, b_level);
  if (current_state == decoder->previous_state) {
    return BR_ROTARY_NONE;
  }

  delta = br_rotary_transition_delta(decoder->previous_state, current_state);
  decoder->previous_state = current_state;
  if (delta == 0) {
    decoder->accumulated_steps = 0;
    return BR_ROTARY_NONE;
  }

  if ((decoder->accumulated_steps > 0 && delta < 0) ||
      (decoder->accumulated_steps < 0 && delta > 0)) {
    decoder->accumulated_steps = 0;
  }
  decoder->accumulated_steps += delta;

  if (decoder->accumulated_steps >= decoder->trigger_steps) {
    decoder->accumulated_steps = 0;
    return BR_ROTARY_CLOCKWISE;
  }
  if (abs(decoder->accumulated_steps) >= decoder->trigger_steps) {
    decoder->accumulated_steps = 0;
    return BR_ROTARY_COUNTER_CLOCKWISE;
  }
  return BR_ROTARY_NONE;
}

static bool br_rotary_copy_page(const char *page, char *output, size_t output_size) {
  size_t length;
  if (!page || !output || output_size == 0) {
    return false;
  }
  length = strlen(page);
  if (length + 1 > output_size) {
    return false;
  }
  memcpy(output, page, length + 1);
  return true;
}

static bool br_rotary_page_equals(const char *left, const char *right) {
  size_t length;
  if (!left || !right) {
    return false;
  }
  while (*left == ' ' || *left == '\t' || *left == '\n' || *left == '\r') {
    left += 1;
  }
  length = strlen(left);
  while (length > 0 &&
         (left[length - 1] == ' ' ||
          left[length - 1] == '\t' ||
          left[length - 1] == '\n' ||
          left[length - 1] == '\r')) {
    length -= 1;
  }
  return strlen(right) == length && strncmp(left, right, length) == 0;
}

bool br_rotary_select_page(
  const char *current_page,
  const char *clockwise_page,
  const char *counter_clockwise_page,
  bool toggle_pages,
  br_rotary_direction direction,
  char *output,
  size_t output_size
) {
  if (direction == BR_ROTARY_NONE) {
    return false;
  }
  if (toggle_pages) {
    const char *next_page = br_rotary_page_equals(current_page, clockwise_page)
      ? counter_clockwise_page
      : clockwise_page;
    return br_rotary_copy_page(next_page, output, output_size);
  }
  return br_rotary_copy_page(direction == BR_ROTARY_CLOCKWISE ? clockwise_page : counter_clockwise_page,
                             output,
                             output_size);
}
