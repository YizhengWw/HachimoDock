#include "pet_p4_trusted_services_device.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned clock_calls, input_calls, flag_calls[4];
static int busy_flag = -1;
int64_t esp_timer_get_time(void) { clock_calls++; return 1000 + clock_calls * 10; }
void pet_p4_input_get_joystick_snapshot(pet_p4_input_joystick_snapshot_t *out) {
  input_calls++;
  *out = (pet_p4_input_joystick_snapshot_t){true, 100, 200, 300, 400, 0, 4000, 0, 4000};
}
bool pet_p4_audio_active(void) { flag_calls[0]++; return busy_flag == 0; }
bool pet_p4_audio_playback_active(void) { flag_calls[1]++; return busy_flag == 1; }
bool pet_p4_audio_conversation_active(void) { flag_calls[2]++; return busy_flag == 2; }
bool pet_p4_audio_music_active(void) { flag_calls[3]++; return busy_flag == 3; }

int main(void) {
  pet_resource_owner_t owner = {0};
  pet_trusted_service_authority_t a = {0};
  pet_trusted_service_snapshot_t out, before;
  pet_trusted_service_diagnostic_t d;
  pet_resource_usage_t used;
  memset(&out, 0xab, sizeof(out)); before = out;
  assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_PERIPHERAL), &owner) == PET_RESOURCE_OK);
  assert(pet_trusted_service_authority_init(&a, owner, PET_TRUSTED_SERVICE_ALL_MASK, 16));
  /* Production remains fail closed until the resource owner enables a quota.
   * When enabled this same harness tests real SDK/provider wiring instead. */
  if (!pet_resources_limit(PET_RESOURCE_PERIPHERAL).leases) {
    assert(pet_trusted_service_device_read(&a, 1, &out, &d) == PET_TRUSTED_UNAVAILABLE);
    assert(pet_trusted_service_device_read(&a, 2, &out, &d) == PET_TRUSTED_UNAVAILABLE);
    assert(!clock_calls && !input_calls && !memcmp(&out, &before, sizeof(out)));
    puts("PASS real broker zero quota rejects both providers without sampling");
  } else {
    assert(pet_trusted_service_device_read(&a, 1, &out, &d) == PET_TRUSTED_OK);
    assert(out.value.monotonic_us == 1010 && d.elapsed_us == 10 && input_calls == 0);
    assert(pet_trusted_service_device_read(&a, 2, &out, &d) == PET_TRUSTED_OK);
    assert(out.value.joystick.current_x == 300 && out.value.joystick.center_y == 200);
    assert(input_calls == 1 && clock_calls == 4);
    puts("PASS real broker and P4 provider adapters read existing clock/ADC snapshot");
  }
  pet_resources_device_revoke(owner);
  pet_resources_device_usage(&used);
  assert(!used.leases[PET_RESOURCE_PERIPHERAL]);
  for (int i = 0; i < 4; i++) {
    owner = (pet_resource_owner_t){0};
    assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_PERIPHERAL), &owner) == PET_RESOURCE_OK);
    a = (pet_trusted_service_authority_t){0};
    assert(pet_trusted_service_authority_init(&a, owner, PET_TRUSTED_SERVICE_ALL_MASK, 16));
    memset(flag_calls, 0, sizeof(flag_calls)); busy_flag = i;
    assert(pet_trusted_service_device_read(&a, 2, &out, &d) == PET_TRUSTED_AUDIO_PROTECTED);
    for (unsigned flag = 0; flag < 4; flag++) assert(flag_calls[flag] == 1);
    assert(a.cancelled);
    busy_flag = -1;
    unsigned previous_clocks = clock_calls, previous_inputs = input_calls;
    assert(!pet_trusted_service_authority_init(&a, owner, PET_TRUSTED_SERVICE_ALL_MASK, 16));
    assert(pet_trusted_service_device_read(&a, 2, &out, &d) == PET_TRUSTED_CANCELLED);
    /* A different authority cannot revive the permanently revoked owner. */
    pet_trusted_service_authority_t attempted_revival = {0};
    assert(pet_trusted_service_authority_init(&attempted_revival, owner, PET_TRUSTED_SERVICE_ALL_MASK, 16));
    assert(pet_trusted_service_device_read(&attempted_revival, 2, &out, &d) == PET_TRUSTED_CANCELLED);
    assert(clock_calls == previous_clocks && input_calls == previous_inputs);
  }
  pet_resources_device_usage(&used);
  assert(!used.live_owners && !used.revoked_owners && !used.leases[PET_RESOURCE_PERIPHERAL]);
  puts("PASS all four audio flags are observed without audio operations or auto-resume");
  return 0;
}
