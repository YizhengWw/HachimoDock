#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PET_P4_BUTTON_EVENT_NONE = 0,
  PET_P4_BUTTON_EVENT_DOWN = 1 << 0,
  PET_P4_BUTTON_EVENT_UP = 1 << 1,
  PET_P4_BUTTON_EVENT_SHORT_PRESS = 1 << 2,
  PET_P4_BUTTON_EVENT_LONG_PRESS = 1 << 3,
  PET_P4_BUTTON_EVENT_LONG_RELEASE = 1 << 4,
};

typedef struct {
  bool raw_pressed;
  bool stable_pressed;
  bool long_press_emitted;
  uint32_t raw_stable_ms;
  uint32_t held_ms;
  uint32_t debounce_ms;
  uint32_t long_press_ms;
} pet_p4_button_decoder_t;

typedef enum {
  PET_P4_ROTARY_COUNTER_CLOCKWISE = -1,
  PET_P4_ROTARY_NONE = 0,
  PET_P4_ROTARY_CLOCKWISE = 1,
} pet_p4_rotary_direction_t;

typedef struct {
  int previous_state;
  int accumulated_steps;
  int trigger_steps;
} pet_p4_rotary_decoder_t;

void pet_p4_button_decoder_init(
  pet_p4_button_decoder_t *decoder,
  bool pressed,
  uint32_t debounce_ms,
  uint32_t long_press_ms
);
uint8_t pet_p4_button_decoder_update(
  pet_p4_button_decoder_t *decoder,
  bool pressed,
  uint32_t elapsed_ms
);
void pet_p4_rotary_decoder_init(
  pet_p4_rotary_decoder_t *decoder,
  int a_level,
  int b_level,
  int trigger_steps
);
pet_p4_rotary_direction_t pet_p4_rotary_decoder_update(
  pet_p4_rotary_decoder_t *decoder,
  int a_level,
  int b_level
);

#ifdef __cplusplus
}
#endif
