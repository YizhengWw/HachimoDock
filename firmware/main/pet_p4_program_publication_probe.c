/* Explicit fixed-vector measurement, never caller code/state or user saves. */
#include "pet_p4_program_probe.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_audio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
static portMUX_TYPE publication_probe_mux=portMUX_INITIALIZER_UNLOCKED;
static bool active,started;
static int64_t last_start;
static EXT_RAM_BSS_ATTR pet_program_state_t source,target;
#if PET_P4_PROGRAM_V2
static EXT_RAM_BSS_ATTR pet_program_publication_t selector;
static EXT_RAM_BSS_ATTR pet_program_state_t slots[2];
#endif
static EXT_RAM_BSS_ATTR char reply[512];
static bool audio_busy(void){
  bool a=pet_p4_audio_active(),b=pet_p4_audio_playback_active();
  bool c=pet_p4_audio_conversation_active(),d=pet_p4_audio_music_active();return a||b||c||d;
}
static void early(pet_program_probe_send_fn send,void *context,const char *error){
  char message[160];snprintf(message,sizeof(message),"{\"topic\":\"debug/program_publication_result\",\"payload\":{\"ok\":false,\"error\":\"%s\",\"samples\":0}}",error);send(message,context);
}
void pet_p4_program_publication_probe(pet_program_probe_send_fn send,void *context){
  if(!send)return;
  const char *error=NULL;int64_t now=esp_timer_get_time();
  portENTER_CRITICAL(&publication_probe_mux);
  if(active)error="busy";
  else if(now<0||(started&&now<last_start))error="clock_error";
  else if(started&&now-last_start<INT64_C(5000000))error="rate_limited";
  else{active=true;started=true;last_start=now;}
  portEXIT_CRITICAL(&publication_probe_mux);
  if(error){early(send,context,error);return;}
  pet_resource_owner_t owner={0};pet_resource_lease_t lease={0};
  unsigned samples=0;uint64_t minimum=UINT64_MAX,maximum=0,sum=0;
  unsigned critical_samples=0;uint32_t critical_maximum=0;uint64_t critical_sum=0;
  if(audio_busy())error="audio_busy";
  if(!error&&pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_NATIVE_PROBE),&owner)!=PET_RESOURCE_OK)error="resource_busy";
  if(!error&&pet_resources_device_acquire(owner,PET_RESOURCE_NATIVE_PROBE,PET_PROGRAM_V1_FUEL,&lease)!=PET_RESOURCE_OK)error="resource_busy";
  for(unsigned i=0;i<128&&!error;i++){
    if(audio_busy()||!pet_resources_device_current(lease)){error="cancelled";break;}
    (void)pet_program_init(&target,owner.generation);source=target;source.revision=1;
    for(unsigned cell=0;cell<PET_PROGRAM_CELLS;cell++)source.cells[cell]=(int32_t)(i*64+cell);
    for(unsigned output=0;output<PET_PROGRAM_OUTPUTS;output++)source.outputs[output]=(int32_t)(i+output);
#if PET_P4_PROGRAM_V2
    memset(&selector,0,sizeof(selector));
    if(!pet_program_state_bind_publication(&target,&selector,slots)){error="publication_failed";break;}
#endif
    int64_t before=esp_timer_get_time();
    pet_program_publication_observation_t observed={0};
    bool copied=pet_resources_device_probe_program_publish_observed(lease,&target,&source,&observed);
    int64_t after=esp_timer_get_time();
    if(!copied){error="cancelled";break;}
    if(before<0||after<before){error="clock_error";break;}
    uint64_t elapsed=(uint64_t)(after-before);
    if(UINT64_MAX-sum<elapsed){error="clock_error";break;}
    if(memcmp(pet_program_state_read(&target),&source,sizeof(target))){error="publication_failed";break;}
    if(elapsed<minimum)minimum=elapsed;if(elapsed>maximum)maximum=elapsed;sum+=elapsed;samples++;
    if(observed.cycle_measured){critical_samples++;critical_sum+=observed.critical_cycles;
      if(observed.critical_cycles>critical_maximum)critical_maximum=observed.critical_cycles;}
    /* Physical work has completed; one scheduler yield, never a second copy
     * hidden in this measured interval. Real audio may revoke before next turn. */
    vTaskDelay(1);
  }
  if(owner.generation)pet_resources_device_revoke(owner);
  if(lease.serial&&(pet_resources_device_finish(lease)!=PET_RESOURCE_FINISH_REVOKED)&&!error)error="release_failed";
#ifdef ESP_PLATFORM
  const char *storage="psram";
#else
  const char *storage="host-stub";
#endif
#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
  unsigned cpu_hz=CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ*1000000U;
#else
  unsigned cpu_hz=0;
#endif
  int n=snprintf(reply,sizeof(reply),"{\"topic\":\"debug/program_publication_result\",\"payload\":{\"engine\":\"p4-program-publication-probe-v1\",\"ok\":%s,\"error\":\"%s\",\"samples\":%u,\"minUs\":%" PRIu64 ",\"maxUs\":%" PRIu64 ",\"sumUs\":%" PRIu64 ",\"stateBytes\":%u,\"storage\":\"%s\",\"criticalSamples\":%u,\"criticalMaxCycles\":%u,\"criticalSumCycles\":%" PRIu64 ",\"configuredCpuHz\":%u}}",
    error?"false":"true",error?error:"",samples,samples?minimum:0,maximum,sum,(unsigned)sizeof(target),storage,critical_samples,(unsigned)critical_maximum,critical_sum,cpu_hz);
  if(n>0&&(size_t)n<sizeof(reply))send(reply,context);else early(send,context,"reply_overflow");
  portENTER_CRITICAL(&publication_probe_mux);active=false;portEXIT_CRITICAL(&publication_probe_mux);
}
