/*
 * [Input] Deterministic centered and directional joystick samples.
 * [Output] Assertions for center-aware four-way hysteresis, limited-travel
 *          direction recognition, dominant-axis resolution, and held repeat.
 * [Pos] Native C regression executable for P4 input logic.
 * [Sync] If this file changes, update `p4_input_logic_test.py` and the decoder.
 */

#include "pet_p4_input_core.h"

#include <assert.h>

static void release_to_center(pet_p4_joystick_decoder_t *decoder) {
  assert(pet_p4_joystick_decoder_update(decoder, 2048, 2048, 5) == PET_P4_JOYSTICK_CENTER);
}

int main(void) {
  pet_p4_joystick_decoder_t joystick;
  pet_p4_joystick_decoder_init(&joystick, 2048, 2048, 900, 500, 350, 140);

  assert(pet_p4_joystick_decoder_update(&joystick, 2048, 2048, 5) == PET_P4_JOYSTICK_CENTER);
  assert(pet_p4_joystick_decoder_update(&joystick, 400, 2048, 5) == PET_P4_JOYSTICK_LEFT);
  assert(pet_p4_joystick_decoder_update(&joystick, 1600, 2048, 5) == PET_P4_JOYSTICK_CENTER);
  release_to_center(&joystick);
  assert(pet_p4_joystick_decoder_update(&joystick, 3700, 2048, 5) == PET_P4_JOYSTICK_RIGHT);
  release_to_center(&joystick);
  assert(pet_p4_joystick_decoder_update(&joystick, 2048, 3700, 5) == PET_P4_JOYSTICK_UP);
  release_to_center(&joystick);
  assert(pet_p4_joystick_decoder_update(&joystick, 2048, 400, 5) == PET_P4_JOYSTICK_DOWN);
  release_to_center(&joystick);

  // Limited-travel samples from production joystick batches must cross the
  // center-aware threshold even though they do not reach the ADC rails.
  assert(pet_p4_joystick_decoder_update(&joystick, 1500, 2048, 5) == PET_P4_JOYSTICK_LEFT);
  release_to_center(&joystick);
  assert(pet_p4_joystick_decoder_update(&joystick, 2600, 2048, 5) == PET_P4_JOYSTICK_RIGHT);
  release_to_center(&joystick);
  assert(pet_p4_joystick_decoder_update(&joystick, 2048, 2600, 5) == PET_P4_JOYSTICK_UP);
  release_to_center(&joystick);
  assert(pet_p4_joystick_decoder_update(&joystick, 2048, 1500, 5) == PET_P4_JOYSTICK_DOWN);
  release_to_center(&joystick);

  // A diagonal sample resolves to the dominant axis and emits only once.
  assert(pet_p4_joystick_decoder_update(&joystick, 3500, 3100, 5) == PET_P4_JOYSTICK_RIGHT);
  assert(pet_p4_joystick_decoder_update(&joystick, 3500, 3100, 340) == PET_P4_JOYSTICK_CENTER);
  assert(pet_p4_joystick_decoder_update(&joystick, 3500, 3100, 10) == PET_P4_JOYSTICK_RIGHT);
  assert(pet_p4_joystick_decoder_update(&joystick, 3500, 3100, 140) == PET_P4_JOYSTICK_RIGHT);
  release_to_center(&joystick);

  return 0;
}
