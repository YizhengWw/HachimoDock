#include "pet_p4_program_host_device.h"
#include "pet_p4_resources_device.h"
#include <assert.h>
#include <stdio.h>

static int busy=-1;
static unsigned flags[4];
static unsigned clock_calls;
int64_t esp_timer_get_time(void){clock_calls++;return 0;}
bool pet_p4_audio_active(void) {flags[0]++;return busy==0;}
bool pet_p4_audio_playback_active(void) {flags[1]++;return busy==1;}
bool pet_p4_audio_conversation_active(void) {flags[2]++;return busy==2;}
bool pet_p4_audio_music_active(void) {flags[3]++;return busy==3;}
static pet_program_host_t host;
int main(void) {
  assert(pet_resources_limit(PET_RESOURCE_PROGRAM).leases==0 && pet_resources_limit(PET_RESOURCE_PROGRAM).units==0);
  assert(pet_resources_limit(PET_RESOURCE_PERIPHERAL).leases==0 && pet_resources_limit(PET_RESOURCE_PERIPHERAL).units==0);
  assert(pet_program_host_device_init(&host));
  assert(host.clock&&host.slice_budget_us==750);
  pet_program_image_t image={.schema=1,.count=1,.fuel=1,.valid=true};
  assert(pet_program_host_stage(&host,&image)==PET_RUNTIME_OK);
  for(unsigned i=0;i<32;i++) {
    uint64_t g=777; assert(pet_program_host_start(&host,1,NULL,&g)==PET_RUNTIME_DENIED && g==777);
    assert(pet_program_host_start_image(&host,&image,1,NULL,&g)==PET_RUNTIME_DENIED&&g==777);
    assert(host.last_admission==PET_RESOURCE_UNAVAILABLE && host.runtime.generation==0 && !host.bound);
    pet_resource_usage_t usage; pet_resources_device_usage(&usage);
    assert(!usage.live_owners && !usage.revoked_owners && !usage.leases[PET_RESOURCE_PROGRAM]);
  }
  assert(host.greatest_generation==64);
  for(busy=0;busy<4;busy++) {
    for(unsigned j=0;j<4;j++) flags[j]=0;
    uint64_t g=777; assert(pet_program_host_start(&host,1,NULL,&g)==PET_RUNTIME_DENIED && g==777);
    assert(host.last_admission==PET_RESOURCE_PROTECTED && host.greatest_generation==64);
    for(unsigned j=0;j<4;j++) assert(flags[j]==1);
  }
  pet_resource_usage_t usage; pet_resources_device_usage(&usage);
  assert(!usage.live_owners && !usage.revoked_owners && !host.runtime.active && !host.runtime.report_pending);
  assert(!clock_calls); /* No program admission: no clock/VM work starts. */
  puts("PASS actual device factory + real production broker: quota closed, no execution/leaks; four audio flags read only");
  return 0;
}
