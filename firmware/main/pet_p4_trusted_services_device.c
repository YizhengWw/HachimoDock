#include "pet_p4_trusted_services_device.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_input.h"
#include "pet_p4_audio.h"
#include "esp_timer.h"

static int64_t now_us(void *context) { (void)context; return esp_timer_get_time(); }
static pet_trusted_sample_result_t joystick(void *context, pet_trusted_joystick_t *out) {
  (void)context;
  pet_p4_input_joystick_snapshot_t snapshot = {0};
  pet_p4_input_get_joystick_snapshot(&snapshot);
  *out = (pet_trusted_joystick_t){snapshot.ready,
    snapshot.center_x, snapshot.center_y, snapshot.current_x, snapshot.current_y,
    snapshot.minimum_x, snapshot.maximum_x, snapshot.minimum_y, snapshot.maximum_y};
  return snapshot.ready ? PET_TRUSTED_SAMPLE_READY : PET_TRUSTED_SAMPLE_NOT_READY;
}
static bool audio_busy(void *context) {
  (void)context;
  bool capture = pet_p4_audio_active(), playback = pet_p4_audio_playback_active();
  bool conversation = pet_p4_audio_conversation_active(), music = pet_p4_audio_music_active();
  return capture || playback || conversation || music;
}
static pet_resource_result_t acquire(void *context, pet_resource_owner_t owner,
    pet_resource_kind_t kind, uint32_t units, pet_resource_lease_t *out) {
  (void)context;
  return pet_resources_device_acquire(owner, kind, units, out);
}
static bool current(void *context, pet_resource_lease_t lease) {
  (void)context; return pet_resources_device_current(lease);
}
static void revoke(void *context, pet_resource_owner_t owner) {
  (void)context; pet_resources_device_revoke(owner);
}
static pet_resource_finish_t finish(void *context, pet_resource_lease_t lease) {
  (void)context; return pet_resources_device_finish(lease);
}
pet_trusted_service_result_t pet_trusted_service_device_read(pet_trusted_service_authority_t *authority,
    uint32_t service, pet_trusted_service_snapshot_t *out, pet_trusted_service_diagnostic_t *diagnostic) {
  static const pet_trusted_service_backend_t backend = {
    NULL, now_us, joystick, audio_busy, acquire, current, revoke, finish
  };
  return pet_trusted_service_read(authority, service, &backend, out, diagnostic);
}
