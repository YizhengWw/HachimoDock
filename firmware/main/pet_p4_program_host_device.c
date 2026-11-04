#include "pet_p4_program_host_device.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_audio.h"
#include "esp_timer.h"
static int64_t slice_clock(void *context){(void)context;return esp_timer_get_time();}

static bool audio_busy(void *context) {
  (void)context;
  bool capture = pet_p4_audio_active(), playback = pet_p4_audio_playback_active();
  bool conversation = pet_p4_audio_conversation_active(), music = pet_p4_audio_music_active();
  return capture || playback || conversation || music;
}
static pet_resource_result_t owner_open(void *context, uint32_t mask, pet_resource_owner_t *out) {
  (void)context; return pet_resources_device_owner_open(mask,out);
}
static pet_resource_result_t acquire(void *context, pet_resource_owner_t owner,
    pet_resource_kind_t kind, uint32_t units, pet_resource_lease_t *out) {
  (void)context; return pet_resources_device_acquire(owner,kind,units,out);
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
static bool publish(void *context,pet_resource_lease_t lease,pet_program_state_t *target,const pet_program_state_t *candidate){
  (void)context;return pet_resources_device_program_publish(lease,target,candidate);
}
static bool publish_probe(void *context,pet_resource_lease_t lease,pet_program_state_t *target,const pet_program_state_t *candidate){
  (void)context;return pet_resources_device_probe_program_publish(lease,target,candidate);
}
static const pet_program_host_backend_t backend = {NULL,audio_busy,owner_open,acquire,current,revoke,finish};
bool pet_program_host_device_init(pet_program_host_t *fresh_host) {
  return pet_program_host_init(fresh_host,&backend)&&pet_program_host_bind_publication(fresh_host,publish,NULL)
    &&pet_program_host_bind_clock(fresh_host,slice_clock,NULL,750);
}
bool pet_program_host_device_init_native_probe(pet_program_host_t *fresh_host){
  return pet_program_host_init_native_probe(fresh_host,&backend)
    &&pet_program_host_bind_publication(fresh_host,publish_probe,NULL)
    &&pet_program_host_bind_clock(fresh_host,slice_clock,NULL,750);
}
