/* Fixed native session measurement, never caller code/state, storage or audio.
 * Runs on the protocol worker, NOT proof of main-parent/mixed-load timing. */
#include "pet_p4_program_probe.h"
#include "pet_p4_component_features.h"
#if PET_COMPONENT_PROGRAM_ENABLED
#include "pet_p4_component_program_session.h"
#include "pet_p4_program_host_device.h"
#include "pet_p4_audio.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static bool active,attempted,initialized,started;
static int64_t last_start;
static EXT_RAM_BSS_ATTR pet_component_program_session_t session;
static EXT_RAM_BSS_ATTR pet_program_scheduler_t scheduler;
static EXT_RAM_BSS_ATTR pet_component_program_config_t config;
static EXT_RAM_BSS_ATTR pet_program_snapshot_t snapshot;
static EXT_RAM_BSS_ATTR char reply[768];
static const pet_program_insn_t code[]={
 {PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
 {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}};
static bool audio_busy(void){
  bool a=pet_p4_audio_active(),b=pet_p4_audio_playback_active();
  bool c=pet_p4_audio_conversation_active(),d=pet_p4_audio_music_active();return a||b||c||d;
}
static void failure(pet_program_probe_send_fn send,void*context,const char*error){
  char line[192];snprintf(line,sizeof(line),"{\"topic\":\"debug/program_session_result\",\"payload\":{\"ok\":false,\"error\":\"%s\",\"samples\":0}}",error);send(line,context);
}
typedef struct {uint64_t maximum,start,end;unsigned turns,stack;} measurement_t;
static const char *commit(uint64_t revision,int32_t base[8],measurement_t*m){
  int32_t outputs[8];
  for(unsigned turn=0;turn<16;turn++){
    if(audio_busy())return "audio_busy";
    int64_t before=esp_timer_get_time();if(before<0)return "clock_error";
    pet_program_schedule_diagnostic_t turn={0};
    (void)pet_program_scheduler_step(&scheduler,(uint64_t)before/1000,&turn);
    bool committed=pet_component_program_session_collect(&session,revision,base,outputs);
    int64_t after=esp_timer_get_time();
    if(after<before)return "clock_error";
    uint64_t elapsed=(uint64_t)(after-before);m->turns++;
    if(elapsed>m->maximum){m->maximum=elapsed;m->start=(uint64_t)before;m->end=(uint64_t)after;}
    unsigned stack=uxTaskGetStackHighWaterMark(NULL);if(stack<m->stack)m->stack=stack;
    /* Every quantum, including begin/terminal publication, returns and yields.
     * No catch-up loop is hidden inside the measured session turn. */
    vTaskDelay(1);
    if(committed){memcpy(base,outputs,sizeof(outputs));return NULL;}
    if(!session.live||session.retiring||session.stop_requested)return "stopped";
  }
  return "turn_limit";
}
void pet_p4_program_session_probe(pet_program_probe_send_fn send,void*context){
  if(!send)return;
  const char*error=NULL;int64_t now=esp_timer_get_time();
  portENTER_CRITICAL(&mux);
  if(active)error="busy";
  else if(now<0||(started&&now<last_start))error="clock_error";
  else if(started&&now-last_start<INT64_C(5000000))error="rate_limited";
  else{active=true;started=true;last_start=now;}
  portEXIT_CRITICAL(&mux);
  if(error){failure(send,context,error);return;}
  measurement_t measured={.stack=UINT32_MAX};unsigned samples=0,timer_events=0;
  int32_t base[8]={0};
  if(audio_busy())error="audio_busy";
  if(!error&&!attempted){
    attempted=true;
    initialized=pet_program_scheduler_init(&scheduler)
      &&pet_program_host_device_init_native_probe(&session.host)
      &&pet_component_program_session_bind_scheduler(&session,&scheduler);
    config.enabled=true;config.image=(pet_program_image_t){.schema=1,.count=7,.fuel=64,.cells=1,.valid=true};
    memcpy(config.image.code,code,sizeof(code));
    config.projection=(pet_program_ui_projection_t){.schema=1,.variables=1,.minimum={0},.maximum={100}};
    memset(config.projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(config.projection.target));config.projection.target[0]=0;
  }
  if(!error&&!initialized)error="initialization_failed";
  if(!error&&(session.live||session.retiring||session.host.bound))error="retirement_pending";
  if(!error&&scheduler.stopped&&pet_program_scheduler_rearm(&scheduler)!=PET_SCHED_OK)error="parent_stopped";
  uint64_t revision=session.revision;
  if(!error&&revision>UINT64_MAX-2)error="generation_exhausted";
  if(!error){
    memset(&snapshot,0,sizeof(snapshot));snapshot.schema=1;snapshot.cells=1;
    config.timer_count=0;now=esp_timer_get_time();
    if(now<0)error="clock_error";
    else if(pet_component_program_session_start(&session,&config,&snapshot,++revision,(uint64_t)now/1000)!=PET_RUNTIME_OK)error="admission_denied";
  }
  for(unsigned i=0;i<8&&!error;i++){
    const pet_program_event_t event={1,{1}};
    if(pet_component_program_session_enqueue(&session,revision,&event)!=PET_RUNTIME_OK)error="enqueue_failed";
    if(!error)error=commit(revision,base,&measured);
    if(!error&&(base[0]!=(int32_t)(i+1)||session.last_report.result!=PET_PROGRAM_COMMITTED))error="result_mismatch";
    if(!error)samples++;
  }
  if(!error&&!pet_component_program_session_snapshot(&session,&snapshot))error="snapshot_failed";
  if(initialized&&session.generation&&pet_component_program_session_stop(&session)!=PET_RUNTIME_OK&&!error)error="retirement_pending";
  if(!error){
    config.timer_count=1;config.timers[0]=(pet_program_timer_spec_t){.id=1,.event={1,{2}}};
    now=esp_timer_get_time();
    if(now<0)error="clock_error";
    else if(pet_component_program_session_start(&session,&config,&snapshot,++revision,(uint64_t)now/1000)!=PET_RUNTIME_OK)error="timer_admission_denied";
    if(!error)error=commit(revision,base,&measured);
    if(!error&&(base[0]!=10||session.timers.accepted!=1))error="timer_result_mismatch";
    if(!error)timer_events=1;
  }
  if(initialized&&session.generation&&pet_component_program_session_stop(&session)!=PET_RUNTIME_OK&&!error)error="retirement_pending";
  bool retired=!session.live&&!session.retiring&&!session.scheduled&&!session.host.bound;
  int n=snprintf(reply,sizeof(reply),"{\"topic\":\"debug/program_session_result\",\"payload\":{\"engine\":\"p4-native-session-probe-v1\",\"ok\":%s,\"error\":\"%s\",\"samples\":%u,\"timerEvents\":%u,\"turns\":%u,\"stepMaxUs\":%" PRIu64 ",\"peakStartUs\":%" PRIu64 ",\"peakEndUs\":%" PRIu64 ",\"stackFreeBytes\":%u,\"retired\":%s,\"executionTask\":\"protocol-worker\",\"sharedDispatcher\":true,\"guestAdmission\":false}}",
    error?"false":"true",error?error:"",samples,timer_events,measured.turns,measured.maximum,measured.start,measured.end,
    measured.stack==UINT32_MAX?0:measured.stack,retired?"true":"false");
  if(n>0&&(size_t)n<sizeof(reply))send(reply,context);else failure(send,context,"reply_overflow");
  portENTER_CRITICAL(&mux);active=false;portEXIT_CRITICAL(&mux);
}
#else
void pet_p4_program_session_probe(pet_program_probe_send_fn send,void*context){
  if(send)send("{\"topic\":\"debug/program_session_result\",\"payload\":{\"ok\":false,\"error\":\"unavailable\",\"samples\":0}}",context);
}
#endif
