/*
 * [Input] Debounced button levels, rotary quadrature states, and calibrated
 *         two-axis joystick ADC samples.
 * [Output] Heap-free logical button, legacy rotary, and four-direction
 *          joystick events with center-aware per-direction hysteresis and
 *          held-direction repeat.
 * [Pos] Platform-independent physical-input decoder core.
 * [Sync] If this file changes, update `pet_p4_input_core.h`, host tests, and
 *        `firmware/.folder.md`.
 */

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

#define PET_P4_JOYSTICK_ADC_MAX 4095
#define PET_P4_JOYSTICK_THRESHOLD_DIVISOR 5
#define PET_P4_JOYSTICK_MIN_ACTIVATION_DELTA 240

static int bounded_direction_threshold(int available, int limit, int minimum) {
  int threshold = available / PET_P4_JOYSTICK_THRESHOLD_DIVISOR;
  if (threshold < minimum) threshold = minimum;
  if (limit > 0 && threshold > limit) threshold = limit;
  return threshold;
}

static int joystick_x_threshold(
  const pet_p4_joystick_decoder_t *decoder,
  int dx,
  bool release
) {
  if (dx < 0) return release ? decoder->release_left : decoder->activation_left;
  return release ? decoder->release_right : decoder->activation_right;
}

static int joystick_y_threshold(
  const pet_p4_joystick_decoder_t *decoder,
  int dy,
  bool release
) {
  if (dy < 0) return release ? decoder->release_down : decoder->activation_down;
  return release ? decoder->release_up : decoder->activation_up;
}

static pet_p4_joystick_direction_t joystick_direction_for_sample(
  const pet_p4_joystick_decoder_t *decoder,
  int x,
  int y,
  bool release
) {
  const int dx = x - decoder->center_x;
  const int dy = y - decoder->center_y;
  const int abs_x = abs(dx);
  const int abs_y = abs(dy);
  const int x_threshold = joystick_x_threshold(decoder, dx, release);
  const int y_threshold = joystick_y_threshold(decoder, dy, release);
  const bool x_active = abs_x >= x_threshold;
  const bool y_active = abs_y >= y_threshold;
  if (!x_active && !y_active) return PET_P4_JOYSTICK_CENTER;
  if (x_active && (!y_active
      || (int64_t) abs_x * y_threshold >= (int64_t) abs_y * x_threshold)) {
    return dx < 0 ? PET_P4_JOYSTICK_LEFT : PET_P4_JOYSTICK_RIGHT;
  }
  // The board's joystick is mounted with the positive Y rail facing upward.
  return dy < 0 ? PET_P4_JOYSTICK_DOWN : PET_P4_JOYSTICK_UP;
}

void pet_p4_joystick_decoder_init(
  pet_p4_joystick_decoder_t *decoder,
  int center_x,
  int center_y,
  int activation_delta,
  int release_delta,
  uint32_t repeat_delay_ms,
  uint32_t repeat_interval_ms
) {
  if (!decoder) return;
  memset(decoder, 0, sizeof(*decoder));
  decoder->center_x = center_x;
  decoder->center_y = center_y;
  const int activation_limit = activation_delta > 0 ? activation_delta : 900;
  const int release_limit = release_delta > 0 ? release_delta : 500;
  decoder->activation_left = bounded_direction_threshold(
    center_x,
    activation_limit,
    PET_P4_JOYSTICK_MIN_ACTIVATION_DELTA
  );
  decoder->activation_right = bounded_direction_threshold(
    PET_P4_JOYSTICK_ADC_MAX - center_x,
    activation_limit,
    PET_P4_JOYSTICK_MIN_ACTIVATION_DELTA
  );
  decoder->activation_down = bounded_direction_threshold(
    center_y,
    activation_limit,
    PET_P4_JOYSTICK_MIN_ACTIVATION_DELTA
  );
  decoder->activation_up = bounded_direction_threshold(
    PET_P4_JOYSTICK_ADC_MAX - center_y,
    activation_limit,
    PET_P4_JOYSTICK_MIN_ACTIVATION_DELTA
  );
  decoder->release_left = decoder->activation_left / 2;
  decoder->release_right = decoder->activation_right / 2;
  decoder->release_down = decoder->activation_down / 2;
  decoder->release_up = decoder->activation_up / 2;
  if (decoder->release_left > release_limit) decoder->release_left = release_limit;
  if (decoder->release_right > release_limit) decoder->release_right = release_limit;
  if (decoder->release_down > release_limit) decoder->release_down = release_limit;
  if (decoder->release_up > release_limit) decoder->release_up = release_limit;
  decoder->repeat_delay_ms = repeat_delay_ms > 0 ? repeat_delay_ms : 350;
  decoder->repeat_interval_ms = repeat_interval_ms > 0 ? repeat_interval_ms : 140;
  decoder->direction = PET_P4_JOYSTICK_CENTER;
}

pet_p4_joystick_direction_t pet_p4_joystick_decoder_update(
  pet_p4_joystick_decoder_t *decoder,
  int x,
  int y,
  uint32_t elapsed_ms
) {
  pet_p4_joystick_direction_t candidate;
  if (!decoder || elapsed_ms == 0) return PET_P4_JOYSTICK_CENTER;

  if (decoder->direction == PET_P4_JOYSTICK_CENTER) {
    candidate = joystick_direction_for_sample(decoder, x, y, false);
    if (candidate == PET_P4_JOYSTICK_CENTER) return PET_P4_JOYSTICK_CENTER;
    decoder->direction = candidate;
    decoder->held_ms = 0;
    decoder->next_repeat_ms = decoder->repeat_delay_ms;
    return candidate;
  }

  candidate = joystick_direction_for_sample(decoder, x, y, true);
  if (candidate == PET_P4_JOYSTICK_CENTER) {
    decoder->direction = PET_P4_JOYSTICK_CENTER;
    decoder->held_ms = 0;
    decoder->next_repeat_ms = 0;
    return PET_P4_JOYSTICK_CENTER;
  }
  if (candidate != decoder->direction) {
    const pet_p4_joystick_direction_t activated = joystick_direction_for_sample(
      decoder,
      x,
      y,
      false
    );
    if (activated != PET_P4_JOYSTICK_CENTER && activated != decoder->direction) {
      decoder->direction = activated;
      decoder->held_ms = 0;
      decoder->next_repeat_ms = decoder->repeat_delay_ms;
      return activated;
    }
  }

  decoder->held_ms = saturating_add_u32(decoder->held_ms, elapsed_ms);
  if (decoder->held_ms < decoder->next_repeat_ms) return PET_P4_JOYSTICK_CENTER;
  decoder->next_repeat_ms = saturating_add_u32(
    decoder->next_repeat_ms,
    decoder->repeat_interval_ms
  );
  return decoder->direction;
}
