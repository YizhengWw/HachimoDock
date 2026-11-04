/* Actual factory/broker/session/group/scheduler. No synthetic PROGRAM grant. */
#include "pet_p4_component_program_session.h"
#include "pet_p4_program_host_device.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_program_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int64_t clock_us;
static bool audio;
static bool audio_after_yield;
static unsigned delays;
void vTaskDelay(uint32_t ticks){assert(ticks==1);clock_us+=10000;delays++;if(audio_after_yield)audio=true;}
unsigned uxTaskGetStackHighWaterMark(void *task){assert(!task);return 2048;}
int64_t esp_timer_get_time(void){return ++clock_us;}
bool pet_p4_audio_active(void){return audio;}
bool pet_p4_audio_playback_active(void){return false;}
bool pet_p4_audio_conversation_active(void){return false;}
bool pet_p4_audio_music_active(void){return false;}
static pet_component_program_session_t session;
static const pet_program_insn_t code[]={
 {PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
 {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}};
static pet_component_program_config_t config;
static void start(const pet_program_snapshot_t *seed,uint64_t revision){
  assert(pet_component_program_session_start(&session,&config,seed,revision,(revision-1)*100)==PET_RUNTIME_OK);
  assert(session.host.admission_kind==PET_RESOURCE_NATIVE_PROBE&&session.host.lease.kind==PET_RESOURCE_NATIVE_PROBE);
  assert(!pet_program_host_init(&session.host,&session.host.backend));
  assert(!pet_program_host_init_native_probe(&session.host,&session.host.backend));
}
static void stopped(void){
  pet_resource_usage_t usage;pet_resources_device_usage(&usage);
  assert(!usage.live_owners&&!usage.revoked_owners&&!usage.leases[PET_RESOURCE_NATIVE_PROBE]);
  assert(!usage.leases[PET_RESOURCE_PROGRAM]&&!session.live&&!session.scheduled);
}
static bool committed(uint64_t revision,int32_t base[8],int32_t out[8]){
  for(unsigned turn=0;turn<16;turn++)
    if(pet_component_program_session_step(&session,revision,(revision-1)*100+turn,base,out))return true;
  return false;
}
static char reply[1024];
static void receive(const char*line,void*context){assert(context==reply&&strlen(line)<sizeof(reply));strcpy(reply,line);}
int main(void){
  assert(!pet_resources_limit(PET_RESOURCE_PROGRAM).leases);
  assert(pet_program_host_device_init_native_probe(&session.host));
  config.enabled=true;config.image=(pet_program_image_t){.schema=1,.count=7,.fuel=64,.cells=1,.valid=true};
  memcpy(config.image.code,code,sizeof(code));
  config.projection=(pet_program_ui_projection_t){.schema=1,.variables=1,.minimum={0},.maximum={100}};
  memset(config.projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(config.projection.target));config.projection.target[0]=0;
  pet_program_snapshot_t seed={.schema=1,.cells=1};int32_t base[8]={0},out[8];
  start(&seed,1);pet_program_event_t event={1,{3}};
  assert(pet_component_program_session_enqueue(&session,1,&event)==PET_RUNTIME_OK);
  assert(committed(1,base,out)&&out[0]==3);
  assert(pet_component_program_session_snapshot(&session,&seed)&&seed.values[0]==3);
  pet_resource_realtime_t realtime={0};assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
  assert(!pet_component_program_session_step(&session,1,1,base,out));stopped();
  pet_resources_device_realtime_end(&realtime);assert(!realtime.serial);
  assert(!pet_component_program_session_step(&session,1,2,base,out));stopped();
  assert(!pet_component_program_session_can_enqueue(&session,1));
  uint64_t old_generation=session.generation;
  config.timer_count=1;config.timers[0]=(pet_program_timer_spec_t){.id=1,.event={1,{2}}};
  start(&seed,2);assert(session.generation>old_generation);
  assert(committed(2,base,out)&&out[0]==5);
  assert(session.timers.accepted==1&&session.last_report.result==PET_PROGRAM_COMMITTED);
  assert(pet_component_program_session_snapshot(&session,&seed)&&seed.values[0]==5);
  assert(pet_component_program_session_stop(&session)==PET_RUNTIME_OK);stopped();
  audio=true;assert(pet_component_program_session_start(&session,&config,&seed,3,0)==PET_RUNTIME_DENIED);stopped();
  audio=false;assert(!pet_component_program_session_step(&session,3,1,base,out));stopped();
  pet_program_host_t ordinary={0};assert(pet_program_host_device_init(&ordinary));uint64_t generation=777;
  assert(pet_program_host_start_image(&ordinary,&config.image,64,&seed,&generation)==PET_RUNTIME_DENIED&&generation==777);
  assert(!pet_resources_limit(PET_RESOURCE_PROGRAM).leases);
  pet_p4_program_session_probe(receive,reply);
  assert(strstr(reply,"\"ok\":true")&&strstr(reply,"\"samples\":8")&&strstr(reply,"\"timerEvents\":1"));
  assert(strstr(reply,"\"retired\":true")&&strstr(reply,"\"guestAdmission\":false")&&delays>=18);
  assert(strstr(reply,"\"sharedDispatcher\":true"));
  pet_p4_program_session_probe(receive,reply);assert(strstr(reply,"rate_limited"));
  clock_us+=6000000;audio_after_yield=true;
  pet_p4_program_session_probe(receive,reply);assert(strstr(reply,"audio_busy")&&strstr(reply,"\"retired\":true"));
  stopped();audio_after_yield=audio=false;
  clock_us+=6000000;pet_p4_program_session_probe(receive,reply);assert(strstr(reply,"\"ok\":true"));stopped();
  puts("PASS real native probe session; default PROGRAM quota remains closed");return 0;
}
