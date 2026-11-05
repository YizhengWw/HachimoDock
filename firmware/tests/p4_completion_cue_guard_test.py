"""Actual cue admission/process and guard. Stub speaker has no audio/IO; no cooldown oracle."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1];S=(ROOT/'main/pet_p4_audio.c').read_text()
def fn(name):
 at=S.index(name);start=S.index('{',at);depth=0
 for i in range(start,len(S)):
  if S[i]=='{':depth+=1
  if S[i]=='}':
   depth-=1
   if depth==0:return S[at:i+1]
parts=[fn('void pet_p4_audio_status_cues_defer('),fn('static bool status_cue_deferred('),fn('static pet_resource_result_t status_cue_realtime_begin('),fn('static void completion_cue_cancel_pending('),fn('static bool status_cue_enqueue('),fn('static bool completion_active('),fn('static const char *completion_agent('),fn('static void completion_cue_observe('),fn('void pet_p4_audio_process(')]
prefix=r'''#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PET_P4_SESSION_QUEUE_MAX 8
#define PET_P4_ASSET_FAMILY_MAX 32
#define PET_P4_SESSION_QUEUE_ID_MAX 96
#define pdPASS 1
#define PET_RESOURCE_OK 0
#define PET_RESOURCE_PROTECTED 1
#define portENTER_CRITICAL(p) ((void)(p))
static void test_after_unlock(void);
#define portEXIT_CRITICAL(p) ((void)(p),test_after_unlock())
#include "pet_p4_completion_cue_guard.h"
typedef int pet_resource_result_t;
typedef struct{unsigned serial;}pet_resource_realtime_t;
typedef struct{char id[96],state[32];unsigned long long transition_revision;}pet_p4_session_queue_item_t;
typedef struct{char family[32],audio_path[80];bool audio_custom;}pet_p4_asset_entry_t;
typedef struct{uint16_t count;pet_p4_asset_entry_t entries[1];}catalog_t;
typedef struct{bool asset_transfer_active;unsigned session_queue_count,current_session_index,asset_revision;char current_session_id[96],current_session_agent[48],active_agent[48];pet_p4_session_queue_item_t session_queue[8];catalog_t asset_catalog;}pet_p4_runtime_state_t;
typedef struct{char family[32],logical_path[80];bool audio_custom,completion_guarded;unsigned generation;}pet_p4_audio_playback_request_t;
static atomic_bool g_status_cue_deferred,g_status_cue_recheck,g_instrument_enabled,g_stream_active;
static atomic_uint g_playback_generation,g_completion_cue_queued,g_completion_cue_started,g_completion_cue_suppressed;
static pet_completion_cue_guard_t g_completion_cues;
static int g_status_cue_mux,g_playback_behavior;static void *g_playback_queue=(void*)1;
static char g_playback_family[32],g_playback_session_key[96];static unsigned g_playback_asset_revision;static unsigned long long g_playback_session_revision;
static unsigned enqueued,started,attempts;static bool queue_ok=true,broker_ok=true;static pet_p4_audio_playback_request_t pending;static const char *lifecycle="working";
static bool pet_p4_appearance_active(void){return false;}
static bool pet_p4_audio_playback_ready(void){return true;}
static bool pet_p4_audio_conversation_active(void){return false;}
static bool pet_p4_audio_playback_active(void){return false;}
static const char *pet_p4_state_effective_lifecycle(const pet_p4_runtime_state_t*s,unsigned long long n){(void)s;(void)n;return lifecycle;}
static const char *pet_p4_canonical_lifecycle(const char*s){return s;}
static int pet_p4_behavior_select(int*b,const catalog_t*c,const char*l,unsigned rev,unsigned long long n){(void)b;(void)c;(void)rev;(void)n;return !strcmp(l,"done")?0:-1;}
static bool pet_p4_system_cue_family(const char*s){return !strcmp(s,"done");}
static bool global_working_cue;
static bool pet_audio_patch_global_enabled(const char*s){return global_working_cue&&!strcmp(s,"working");}
static int xQueueOverwrite(void*q,const void*r){(void)q;attempts++;if(!queue_ok)return 0;pending=*(const pet_p4_audio_playback_request_t*)r;enqueued++;return pdPASS;}
static pet_resource_result_t pet_resources_device_realtime_begin(pet_resource_realtime_t*t){t->serial=broker_ok?1:0;return broker_ok?PET_RESOURCE_OK:PET_RESOURCE_PROTECTED;}
'''
tail=r'''
static unsigned race_unlocks;
static void test_after_unlock(void){if(race_unlocks&&!--race_unlocks)completion_cue_cancel_pending(&pending,true);}
static void reset(void){
 global_working_cue=false;race_unlocks=0;
 memset(&g_completion_cues,0,sizeof(g_completion_cues));memset(&pending,0,sizeof(pending));atomic_store(&g_status_cue_deferred,false);atomic_store(&g_status_cue_recheck,false);atomic_store(&g_playback_generation,0);atomic_store(&g_completion_cue_queued,0);atomic_store(&g_completion_cue_started,0);atomic_store(&g_completion_cue_suppressed,0);g_playback_family[0]=g_playback_session_key[0]=0;g_playback_asset_revision=0;g_playback_session_revision=0;enqueued=started=attempts=0;queue_ok=broker_ok=true;
}
static pet_p4_runtime_state_t fixture(void){
 reset();pet_p4_runtime_state_t s={0};s.session_queue_count=2;s.asset_revision=1;s.asset_catalog.count=1;strcpy(s.asset_catalog.entries[0].family,"done");strcpy(s.current_session_agent,"synthetic-agent");strcpy(s.active_agent,"synthetic-agent");strcpy(s.current_session_id,"synthetic-A");
 for(unsigned i=0;i<2;i++){strcpy(s.session_queue[i].id,i?"synthetic-B":"synthetic-A");strcpy(s.session_queue[i].state,"working");s.session_queue[i].transition_revision=i?190:90;}
 lifecycle="working";pet_p4_audio_process(&s,900);
 for(unsigned i=0;i<2;i++){strcpy(s.session_queue[i].state,"done");s.session_queue[i].transition_revision=i?200:100;}lifecycle="done";return s;
}
static void start_pending(bool expected){pet_resource_realtime_t t={0};bool ok=status_cue_realtime_begin(&t,&pending)==PET_RESOURCE_OK;assert(ok==expected);if(ok)started++;assert(started==atomic_load(&g_completion_cue_started));}
int main(void){
 pet_p4_runtime_state_t s=fixture();pet_p4_audio_process(&s,1000);start_pending(true);for(unsigned i=0;i<100;i++)pet_p4_audio_process(&s,1001+i);assert(enqueued==1&&started==1);puts("same_transition_refresh_once");
 s=fixture();pet_p4_audio_process(&s,1000);start_pending(true);pet_p4_audio_status_cues_defer(true);pet_p4_audio_status_cues_defer(false);pet_p4_audio_process(&s,1100);assert(enqueued==1&&started==1);puts("defer_does_not_replay_started_cue");
 s=fixture();pet_p4_audio_process(&s,1000);start_pending(true);s.asset_revision++;pet_p4_audio_process(&s,1100);assert(enqueued==1&&started==1);puts("asset_revision_does_not_replay");
 s=fixture();pet_p4_audio_process(&s,1000);start_pending(true);strcpy(s.current_session_id,"synthetic-B");pet_p4_audio_process(&s,1100);start_pending(true);strcpy(s.current_session_id,"synthetic-A");pet_p4_audio_process(&s,1200);assert(enqueued==2&&started==2);puts("A_B_A_once_per_task");
 s=fixture();pet_p4_audio_process(&s,1000);start_pending(true);lifecycle="idle";pet_p4_audio_process(&s,1100);lifecycle="done";pet_p4_audio_process(&s,1200);assert(enqueued==1&&started==1);puts("done_idle_done_does_not_replay");
 s=fixture();pet_p4_audio_process(&s,1000);start_pending(true);strcpy(s.session_queue[0].state,"working");s.session_queue[0].transition_revision=300;lifecycle="working";pet_p4_audio_process(&s,1001);strcpy(s.session_queue[0].state,"done");s.session_queue[0].transition_revision=301;lifecycle="done";pet_p4_audio_process(&s,1002);start_pending(true);assert(enqueued==2&&started==2);puts("next_real_transition_without_cooldown");
 s=fixture();pet_p4_audio_status_cues_defer(true);pet_p4_audio_process(&s,1000);assert(enqueued==0);pet_p4_audio_status_cues_defer(false);pet_p4_audio_process(&s,1100);start_pending(true);assert(enqueued==1&&started==1);puts("new_transition_while_busy_is_not_lost");
 s=fixture();pet_p4_audio_process(&s,1000);pet_p4_audio_status_cues_defer(true);start_pending(false);assert(started==0);pet_p4_audio_status_cues_defer(false);pet_p4_audio_process(&s,1100);start_pending(true);pet_p4_audio_process(&s,1200);assert(enqueued==2&&started==1);puts("queued_then_prestart_defer_retries_only_unstarted");
 s=fixture();queue_ok=false;pet_p4_audio_process(&s,1000);assert(enqueued==0);queue_ok=true;pet_p4_audio_process(&s,1100);start_pending(true);assert(enqueued==1&&started==1);puts("failed_queue_admission_not_consumed");
 s=fixture();pet_p4_audio_process(&s,1000);broker_ok=false;start_pending(false);broker_ok=true;pet_p4_audio_process(&s,1100);start_pending(true);assert(enqueued==2&&started==1);puts("known_prestart_resource_denial_not_consumed");
 s=fixture();pet_p4_audio_process(&s,1000);strcpy(s.session_queue[0].state,"working");s.session_queue[0].transition_revision=300;lifecycle="working";pet_p4_audio_process(&s,1100);start_pending(false);assert(started==0);puts("late_old_pending_cue_cannot_start_in_next_task");
 s=fixture();pet_p4_audio_process(&s,1000);start_pending(true);s.session_queue[0].transition_revision=90;strcpy(s.session_queue[0].state,"working");lifecycle="working";pet_p4_audio_process(&s,1100);strcpy(s.session_queue[0].state,"done");s.session_queue[0].transition_revision=100;lifecycle="done";pet_p4_audio_process(&s,1200);assert(enqueued==1);puts("stale_active_revision_cannot_rearm");
 s=fixture();s.session_queue_count=0;strcpy(s.current_session_id,"");lifecycle="done";pet_p4_audio_process(&s,1000);assert(enqueued==0);puts("unobserved_historical_done_silent");
 reset();memset(&s,0,sizeof(s));s.session_queue_count=1;s.asset_revision=1;s.asset_catalog.count=1;strcpy(s.asset_catalog.entries[0].family,"done");strcpy(s.current_session_agent,"synthetic-agent");
 for(unsigned i=1;i<=64;i++){snprintf(s.session_queue[0].id,sizeof(s.session_queue[0].id),"synthetic-%u",i);strcpy(s.current_session_id,s.session_queue[0].id);strcpy(s.session_queue[0].state,"working");s.session_queue[0].transition_revision=i*2;lifecycle="working";pet_p4_audio_process(&s,i*10);strcpy(s.session_queue[0].state,"done");s.session_queue[0].transition_revision=i*2+1;lifecycle="done";pet_p4_audio_process(&s,i*10+1);start_pending(true);}assert(started==64);
 for(unsigned i=1;i<=64;i++){snprintf(s.session_queue[0].id,sizeof(s.session_queue[0].id),"synthetic-%u",i);strcpy(s.current_session_id,s.session_queue[0].id);s.session_queue[0].transition_revision=i*2+1;pet_p4_audio_process(&s,1000+i);}assert(started==64&&enqueued==64);puts("bounded_history_64_distinct_tasks_no_terminal_replay");
 s=fixture();pet_p4_audio_process(&s,1000);race_unlocks=2;pet_p4_audio_process(&s,1100);assert(race_unlocks==0);
 if(!atomic_load(&g_status_cue_recheck)){puts("PEER171_LOST_RECHECK_RUNTIME_REPRO");return 171;}
 pet_p4_audio_process(&s,1200);start_pending(true);assert(enqueued==2&&started==1);puts("worker_cancel_after_due_unlock_preserves_recheck");
 s=fixture();global_working_cue=true;lifecycle="working";pet_p4_audio_process(&s,1000);
 assert(enqueued==1&&!pending.completion_guarded);lifecycle="done";pet_p4_audio_process(&s,1100);start_pending(true);
 pet_p4_audio_process(&s,1200);assert(enqueued==2&&started==1&&atomic_load(&g_completion_cue_queued)==1);
 puts("configured_global_working_cue_preserves_once_per_task_done");
 printf("guard_bytes=%zu\n",sizeof(g_completion_cues));puts("PASS_16_PRODUCTION_PROCESS_CASES");return 0;
}
'''
def test_actual_status_emission_per_task_and_prestart_admission(tmp_path):
 source=tmp_path/'process.c';source.write_text(prefix+'\n'+'\n'.join(parts)+tail)
 exe=tmp_path/'process'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(source),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True,check=True)
 assert 'PASS_16_PRODUCTION_PROCESS_CASES' in result.stdout
