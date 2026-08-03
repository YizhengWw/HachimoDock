#include "pet_p4_input_core.h"

#include <stdlib.h>
#include <string.h>

static uint32_t saturating_add_u32(uint32_t value, uint32_t increment) {
  return UINT32_MAX - value < increment ? UINT32_MAX : value + increment;
}

void pet_p4_button_decoder_init(
  pet_p4_button_decoder_t *decoder,
  bool pressed,
  uint32_t debounce_ms,
  uint32_t long_press_ms
) {
  if (!decoder) return;
  memset(decoder, 0, sizeof(*decoder));
  decoder->raw_pressed = pressed;
  decoder->stable_pressed = pressed;
  decoder->debounce_ms = debounce_ms > 0 ? debounce_ms : 25;
  decoder->long_press_ms = long_press_ms > 0 ? long_press_ms : 700;
}

uint8_t pet_p4_button_decoder_update(
  pet_p4_button_decoder_t *decoder,
  bool pressed,
  uint32_t elapsed_ms
) {
  uint8_t events = PET_P4_BUTTON_EVENT_NONE;
  bool was_stably_pressed;
  if (!decoder || elapsed_ms == 0) return events;

  was_stably_pressed = decoder->stable_pressed;

  if (pressed != decoder->raw_pressed) {
    decoder->raw_pressed = pressed;
    decoder->raw_stable_ms = 0;
  } else {
    decoder->raw_stable_ms = saturating_add_u32(decoder->raw_stable_ms, elapsed_ms);
  }

  if (decoder->raw_pressed != decoder->stable_pressed
      && decoder->raw_stable_ms >= decoder->debounce_ms) {
    bool was_long_press = decoder->long_press_emitted;
    decoder->stable_pressed = decoder->raw_pressed;
    decoder->held_ms = 0;
    if (decoder->stable_pressed) {
      decoder->long_press_emitted = false;
      events |= PET_P4_BUTTON_EVENT_DOWN;
    } else {
      events |= PET_P4_BUTTON_EVENT_UP;
      if (was_long_press) {
        events |= PET_P4_BUTTON_EVENT_LONG_RELEASE;
      } else {
        events |= PET_P4_BUTTON_EVENT_SHORT_PRESS;
      }
      decoder->long_press_emitted = false;
    }
  }

  if (decoder->stable_pressed && was_stably_pressed) {
    decoder->held_ms = saturating_add_u32(decoder->held_ms, elapsed_ms);
    if (!decoder->long_press_emitted && decoder->held_ms >= decoder->long_press_ms) {
      decoder->long_press_emitted = true;
      events |= PET_P4_BUTTON_EVENT_LONG_PRESS;
    }
  }

  return events;
}

static int rotary_state(int a_level, int b_level) {
  return ((a_level ? 1 : 0) << 1) | (b_level ? 1 : 0);
}

static int rotary_transition_delta(int previous_state, int current_state) {
  int transition = (previous_state << 2) | current_state;
  switch (transition) {
    case 0xD:
    case 0x4:
    case 0x2:
    case 0xB:
      return 1;
    case 0xE:
    case 0x8:
    case 0x1:
    case 0x7:
      return -1;
    default:
      return 0;
  }
}

void pet_p4_rotary_decoder_init(
  pet_p4_rotary_decoder_t *decoder,
  int a_level,
  int b_level,
  int trigger_steps
) {
  if (!decoder) return;
  decoder->previous_state = rotary_state(a_level, b_level);
  decoder->accumulated_steps = 0;
  decoder->trigger_steps = trigger_steps > 0 ? trigger_steps : 4;
}

pet_p4_rotary_direction_t pet_p4_rotary_decoder_update(
  pet_p4_rotary_decoder_t *decoder,
  int a_level,
  int b_level
) {
  int current_state;
  int delta;
  if (!decoder) return PET_P4_ROTARY_NONE;

  current_state = rotary_state(a_level, b_level);
  if (current_state == decoder->previous_state) return PET_P4_ROTARY_NONE;

  delta = rotary_transition_delta(decoder->previous_state, current_state);
  decoder->previous_state = current_state;
  if (delta == 0) {
    decoder->accumulated_steps = 0;
    return PET_P4_ROTARY_NONE;
  }
  if ((decoder->accumulated_steps > 0 && delta < 0)
      || (decoder->accumulated_steps < 0 && delta > 0)) {
    decoder->accumulated_steps = 0;
  }
  decoder->accumulated_steps += delta;

  if (decoder->accumulated_steps >= decoder->trigger_steps) {
    decoder->accumulated_steps = 0;
    return PET_P4_ROTARY_CLOCKWISE;
  }
  if (abs(decoder->accumulated_steps) >= decoder->trigger_steps) {
    decoder->accumulated_steps = 0;
    return PET_P4_ROTARY_COUNTER_CLOCKWISE;
  }
  return PET_P4_ROTARY_NONE;
}
