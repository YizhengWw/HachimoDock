/* Incremental FIXED native diagnostic. No installable code, user state or IO.
 * Request runs on the protocol worker; process runs once per MAIN turn outside
 * UI locks. Each turn performs at most one lifecycle operation or host quantum.
 * Caller guarantees reply callback/context lifetime until terminal delivery. */
#include "pet_p4_program_probe.h"
#include "pet_p4_component_features.h"
#if PET_COMPONENT_PROGRAM_ENABLED
#include "pet_p4_component_program_session.h"
#include "pet_p4_program_host_device.h"
#include "pet_p4_audio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
enum {SETUP,START,ENQUEUE,PUMP,SNAPSHOT,STOP_FIRST,TIMER_START,TIMER_PUMP,STOP_FINAL,CASE_SETUP,DONE};
static portMUX_TYPE main_mux=portMUX_INITIALIZER_UNLOCKED;
static atomic_bool main_active,main_ready;
static bool main_inside,main_attempted,main_initialized,main_started;
static TaskHandle_t main_owner;
static int64_t main_last_start;
static unsigned main_phase;
static pet_program_probe_send_fn main_send;
static void *main_context;
static EXT_RAM_BSS_ATTR pet_component_program_session_t main_session;
static EXT_RAM_BSS_ATTR pet_program_scheduler_t main_scheduler;
static EXT_RAM_BSS_ATTR pet_component_program_config_t main_config;
static EXT_RAM_BSS_ATTR pet_program_snapshot_t main_snapshot;
static EXT_RAM_BSS_ATTR pet_program_snapshot_t main_verification;
static EXT_RAM_BSS_ATTR char main_reply[1024];
static EXT_RAM_BSS_ATTR struct {
  uint64_t revision,maximum,start,end;
  uint32_t turns,pumps,samples,timers,event_turns,stack,peak_phase,kind,max_images,fuel_cases;
  int32_t base[8];const char *error;
} main_job;
static const pet_program_insn_t main_code[]={
 {PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
 {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}};
static bool main_audio_busy(void){
  bool a=pet_p4_audio_active(),b=pet_p4_audio_playback_active();
  bool c=pet_p4_audio_conversation_active(),d=pet_p4_audio_music_active();return a||b||c||d;
}
static void main_fail(const char*error){if(!main_job.error)main_job.error=error;main_phase=STOP_FINAL;}
static void main_early(pet_program_probe_send_fn send,void*context,const char*error){
  char reply[192];snprintf(reply,sizeof(reply),"{\"topic\":\"debug/program_main_result\",\"payload\":{\"ok\":false,\"error\":\"%s\",\"samples\":0}}",error);send(reply,context);
}
bool pet_p4_program_main_probe_bind_owner(void){
  TaskHandle_t current=xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&main_mux);
  bool fresh=current&&!main_owner&&!main_started&&!atomic_load_explicit(&main_active,memory_order_acquire);
  if(fresh)main_owner=current;
  portEXIT_CRITICAL(&main_mux);return fresh;
}
void pet_p4_program_main_probe(pet_program_probe_send_fn send,void*context){
  if(!send)return;
  const char*error=NULL;int64_t now=esp_timer_get_time();
  portENTER_CRITICAL(&main_mux);
  if(!main_owner)error="unavailable";
  else if(atomic_load_explicit(&main_active,memory_order_acquire))error="busy";
  else if(now<0||(main_started&&now<main_last_start))error="clock_error";
  else if(main_started&&now-main_last_start<INT64_C(5000000))error="rate_limited";
  else{
    main_started=true;main_last_start=now;main_send=send;main_context=context;main_phase=SETUP;
    atomic_store_explicit(&main_active,true,memory_order_release);
  }
  portEXIT_CRITICAL(&main_mux);
  if(error)main_early(send,context,error);
}
static bool main_stop(void){
  if(main_initialized&&main_session.generation){
    pet_program_runtime_result_t stopped=pet_component_program_session_stop(&main_session);
    if(stopped!=PET_RUNTIME_OK&&stopped!=PET_RUNTIME_IDLE){main_fail("retirement_pending");return false;}
  }
  return !main_session.live&&!main_session.retiring&&!main_session.scheduled&&!main_session.host.bound;
}
static void main_pump(uint64_t now,bool timer){
  pet_program_schedule_diagnostic_t turn={0};int32_t outputs[8];
  (void)pet_program_scheduler_step(&main_scheduler,now,&turn);main_job.pumps++;
  bool committed=pet_component_program_session_collect(&main_session,main_job.revision,main_job.base,outputs);
  if(main_job.kind){
    bool terminal=main_session.report_valid&&main_session.last_report.generation==main_session.generation;
    if((main_job.kind==1&&committed)||(main_job.kind==2&&terminal&&main_session.last_report.result==PET_PROGRAM_FUEL_EXHAUSTED)){
      if(!pet_component_program_session_snapshot(&main_session,&main_verification)){
        main_fail("case_snapshot_failed");return;
      }
      pet_program_snapshot_t expected;memset(&expected,0,sizeof(expected));
      expected.schema=1;expected.cells=PET_PROGRAM_CELLS;expected.revision=main_job.kind==1?1:0;
      if(memcmp(&main_verification,&expected,sizeof(expected))
          ||main_session.last_report.used_fuel!=(main_job.kind==1?1:PET_PROGRAM_FUEL)){
        main_fail("case_result_mismatch");return;
      }
      main_job.max_images++;if(main_job.kind==2)main_job.fuel_cases++;
      main_phase=STOP_FINAL;return;
    }
    if(committed||(terminal&&main_session.last_report.result!=PET_PROGRAM_COMMITTED)){
      main_fail("case_unexpected_result");return;
    }
  }
  if(committed){
    int32_t expected=timer?10:(int32_t)main_job.samples+1;
    if(outputs[0]!=expected||main_session.last_report.result!=PET_PROGRAM_COMMITTED){main_fail("result_mismatch");return;}
    memcpy(main_job.base,outputs,sizeof(outputs));
    if(timer){
      if(main_session.timers.accepted!=1){main_fail("timer_mismatch");return;}
      main_job.timers=1;main_phase=STOP_FINAL;
    }else{main_job.samples++;main_phase=main_job.samples==8?SNAPSHOT:ENQUEUE;}
    return;
  }
  if(!main_session.live||main_session.retiring||main_session.stop_requested){main_fail("stopped");return;}
  if(++main_job.event_turns>=(main_job.kind==2?512U:16U))main_fail("turn_limit");
}
/* Main-thread only. Never sleep/drain a program or wait for a reply here. */
void pet_p4_program_main_probe_process(void){
  if(!atomic_load_explicit(&main_active,memory_order_acquire)||main_phase==DONE||main_inside||xTaskGetCurrentTaskHandle()!=main_owner)return;
  main_inside=true;unsigned measured_phase=main_phase;int64_t before=esp_timer_get_time();
  if(main_phase==SETUP){memset(&main_job,0,sizeof(main_job));main_job.stack=UINT32_MAX;}
  if(before<0)main_fail("clock_error");
  else if(main_audio_busy())main_fail("audio_busy");
  if(main_job.error)main_phase=STOP_FINAL;
  switch(main_phase){
  case SETUP:
    if(!main_attempted){
      main_attempted=true;main_initialized=pet_program_scheduler_init(&main_scheduler)
        &&pet_program_host_device_init_native_probe(&main_session.host)
        &&pet_component_program_session_bind_scheduler(&main_session,&main_scheduler);
      main_config.enabled=true;main_config.image=(pet_program_image_t){.schema=1,.count=7,.fuel=64,.cells=1,.valid=true};
      memcpy(main_config.image.code,main_code,sizeof(main_code));
      main_config.projection=(pet_program_ui_projection_t){.schema=1,.variables=1,.minimum={0},.maximum={100}};
      memset(main_config.projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(main_config.projection.target));main_config.projection.target[0]=0;
    }
    if(!main_initialized){main_fail("initialization_failed");break;}
    if(main_session.live||main_session.retiring||main_session.host.bound){main_fail("retirement_pending");break;}
    if(main_scheduler.stopped&&pet_program_scheduler_rearm(&main_scheduler)!=PET_SCHED_OK){main_fail("parent_stopped");break;}
    main_job.revision=main_session.revision;
    if(main_job.revision>UINT64_MAX-4){main_fail("generation_exhausted");break;}
    main_config.image=(pet_program_image_t){.schema=1,.count=7,.fuel=64,.cells=1,.valid=true};
    memcpy(main_config.image.code,main_code,sizeof(main_code));
    main_config.timer_count=0;memset(&main_snapshot,0,sizeof(main_snapshot));main_snapshot.schema=1;main_snapshot.cells=1;
    main_phase=START;break;
  case START:case TIMER_START:{
    bool timer=main_phase==TIMER_START;
    if(timer){main_config.timer_count=1;main_config.timers[0]=(pet_program_timer_spec_t){.id=1,.event={1,{2}}};}
    pet_program_runtime_result_t result=pet_component_program_session_start(&main_session,&main_config,&main_snapshot,++main_job.revision,(uint64_t)before/1000);
    if(result!=PET_RUNTIME_OK){main_fail("admission_denied");break;}
    main_job.event_turns=0;main_phase=timer?TIMER_PUMP:ENQUEUE;break;
  }
  case ENQUEUE:{
    const pet_program_event_t event={1,{1}};
    if(pet_component_program_session_enqueue(&main_session,main_job.revision,&event)!=PET_RUNTIME_OK){main_fail("enqueue_failed");break;}
    main_job.event_turns=0;main_phase=PUMP;break;
  }
  case PUMP:main_pump((uint64_t)before/1000,false);break;
  case TIMER_PUMP:main_pump((uint64_t)before/1000,true);break;
  case SNAPSHOT:
    if(!pet_component_program_session_snapshot(&main_session,&main_snapshot)){main_fail("snapshot_failed");break;}
    main_phase=STOP_FIRST;break;
  case STOP_FIRST:if(main_stop())main_phase=TIMER_START;break;
  case STOP_FINAL:if(main_stop())main_phase=!main_job.error&&main_job.kind<2?CASE_SETUP:DONE;break;
  case CASE_SETUP:
    /* Fresh fixed diagnostic models only; never reinterpret or reset a user's
     * saved layout. All 256 instructions are validated on the real main path. */
    main_job.kind++;
    main_config.timer_count=0;
    main_config.image=(pet_program_image_t){.schema=1,.count=PET_PROGRAM_CODE,.fuel=PET_PROGRAM_FUEL,.cells=PET_PROGRAM_CELLS,.valid=true};
    if(main_job.kind==2)main_config.image.code[0].op=PET_OP_JUMP;
    memset(&main_snapshot,0,sizeof(main_snapshot));main_snapshot.schema=1;main_snapshot.cells=PET_PROGRAM_CELLS;
    memset(main_job.base,0,sizeof(main_job.base));main_phase=START;break;
  default:main_fail("bad_phase");break;
  }
  unsigned stack=uxTaskGetStackHighWaterMark(NULL);if(stack<main_job.stack)main_job.stack=stack;
  int64_t after=esp_timer_get_time();
  if(before<0||after<before){
    main_fail("clock_error");
    if(!main_session.live&&!main_session.retiring&&!main_session.scheduled&&!main_session.host.bound)main_phase=DONE;
  }
  else if((uint64_t)(after-before)>main_job.maximum){main_job.maximum=(uint64_t)(after-before);main_job.start=(uint64_t)before;main_job.end=(uint64_t)after;main_job.peak_phase=measured_phase;}
  if(main_job.turns!=UINT32_MAX)main_job.turns++;
  if(main_phase==DONE){
    /* Publication is memory-only. UART/USB serialization and drain belong to
     * the protocol worker; never block the renderer on diagnostic delivery. */
    atomic_store_explicit(&main_ready,true,memory_order_release);
  }
  main_inside=false;
}
bool pet_p4_program_main_probe_pending(void){
  return atomic_load_explicit(&main_active,memory_order_acquire);
}
void pet_p4_program_main_probe_reply(void){
  if(xTaskGetCurrentTaskHandle()==main_owner||!atomic_exchange_explicit(&main_ready,false,memory_order_acq_rel))return;
    int n=snprintf(main_reply,sizeof(main_reply),"{\"topic\":\"debug/program_main_result\",\"payload\":{\"engine\":\"p4-main-session-probe-v2\",\"ok\":%s,\"error\":\"%s\",\"samples\":%u,\"timerEvents\":%u,\"maxImageCases\":%u,\"fuelExhaustionCases\":%u,\"workTurns\":%u,\"pumps\":%u,\"workTurnMaxUs\":%" PRIu64 ",\"peakStartUs\":%" PRIu64 ",\"peakEndUs\":%" PRIu64 ",\"peakPhase\":%u,\"stackFreeBytes\":%u,\"retired\":true,\"executionTask\":\"main\",\"timingScope\":\"work-including-stack-excluding-reply\",\"sharedDispatcher\":true,\"guestAdmission\":false}}",
      main_job.error?"false":"true",main_job.error?main_job.error:"",main_job.samples,main_job.timers,main_job.max_images,main_job.fuel_cases,main_job.turns,main_job.pumps,main_job.maximum,main_job.start,main_job.end,main_job.peak_phase,main_job.stack);
    if(n>0&&(size_t)n<sizeof(main_reply))main_send(main_reply,main_context);else main_early(main_send,main_context,"reply_overflow");
    /* Callback has returned and physical work retired BEFORE another request
     * may replace routing/job fields. A callback reentry cannot process twice. */
    atomic_store_explicit(&main_ready,false,memory_order_release);
    atomic_store_explicit(&main_active,false,memory_order_release);
}
#else
bool pet_p4_program_main_probe_bind_owner(void){return false;}
void pet_p4_program_main_probe(pet_program_probe_send_fn send,void*context){
  if(send)send("{\"topic\":\"debug/program_main_result\",\"payload\":{\"ok\":false,\"error\":\"unavailable\",\"samples\":0}}",context);
}
void pet_p4_program_main_probe_process(void){}
bool pet_p4_program_main_probe_pending(void){return false;}
void pet_p4_program_main_probe_reply(void){}
#endif
