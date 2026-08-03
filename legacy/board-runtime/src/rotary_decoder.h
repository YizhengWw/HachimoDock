#ifndef BOARD_ROTARY_DECODER_H
#define BOARD_ROTARY_DECODER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  BR_ROTARY_COUNTER_CLOCKWISE = -1,
  BR_ROTARY_NONE = 0,
  BR_ROTARY_CLOCKWISE = 1
} br_rotary_direction;

typedef struct {
  int previous_state;
  int accumulated_steps;
  int trigger_steps;
} br_rotary_decoder;

void br_rotary_decoder_init(br_rotary_decoder *decoder, int a_level, int b_level, int trigger_steps);
br_rotary_direction br_rotary_decoder_update(br_rotary_decoder *decoder, int a_level, int b_level);
bool br_rotary_select_page(
  const char *current_page,
  const char *clockwise_page,
  const char *counter_clockwise_page,
  bool toggle_pages,
  br_rotary_direction direction,
  char *output,
  size_t output_size
);

#endif
