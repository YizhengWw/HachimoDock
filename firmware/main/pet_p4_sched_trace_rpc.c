#include "pet_p4_sched_trace_rpc.h"
#if PET_P4_SCHED_TRACE_ENABLE
#include "pet_p4_audio.h"
#include "pet_p4_json_source.h"
#include "cJSON.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool integer(const cJSON *p,const char *key,uint32_t max,uint32_t *out){
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,key);
  if(!cJSON_IsNumber(v)||v->valuedouble<0||v->valuedouble>max
      ||v->valuedouble!=(double)(uint32_t)v->valuedouble)return false;
  *out=(uint32_t)v->valuedouble;return true;
}
static void error(pet_sched_probe_send_fn send,void *ctx,const char *code){
  char out[160];
  snprintf(out,sizeof(out),"{\"topic\":\"debug/sched_result\",\"payload\":{\"ok\":false,\"error\":\"%s\"}}",code);
  send(out,ctx);
}
static bool audio_busy(void){
  bool a=pet_p4_audio_active(),b=pet_p4_audio_playback_active();
  bool c=pet_p4_audio_conversation_active(),d=pet_p4_audio_music_active();return a||b||c||d;
}
bool pet_p4_sched_trace_rpc_handle(const char *line,pet_sched_probe_send_fn send,void *ctx){
  if(!line||!send||!strstr(line,"debug/sched_"))return false;
  if(!pet_json_source_bounded_text(line)){error(send,ctx,"invalid_request");return true;}
  cJSON *root=cJSON_ParseWithOpts(line,NULL,true);
  const cJSON *topic=cJSON_GetObjectItemCaseSensitive(root,"topic");
  const cJSON *p=cJSON_GetObjectItemCaseSensitive(root,"payload");
  const char *name=cJSON_IsString(topic)?topic->valuestring:"";
  enum {START,STOP,STATUS,READ,SUMMARY,UNKNOWN} action=UNKNOWN;
  if(!strcmp(name,"debug/sched_start"))action=START;
  else if(!strcmp(name,"debug/sched_stop"))action=STOP;
  else if(!strcmp(name,"debug/sched_status"))action=STATUS;
  else if(!strcmp(name,"debug/sched_read"))action=READ;
  else if(!strcmp(name,"debug/sched_summary"))action=SUMMARY;
  if(action==UNKNOWN){cJSON_Delete(root);return false;}
  uint32_t generation=0,duration=0,index=0,actor=0;
  bool valid=cJSON_IsObject(root)&&cJSON_GetArraySize(root)==2&&cJSON_IsObject(p)
    &&cJSON_GetArraySize(p)==((action==READ||action==SUMMARY)?2:1);
  if(action==START)valid=valid&&integer(p,"durationUs",PET_SCHED_TRACE_MAX_WINDOW_US,&duration)&&duration;
  else valid=valid&&integer(p,"generation",UINT32_MAX,&generation)&&generation;
  if(action==READ)valid=valid&&integer(p,"index",PET_SCHED_TRACE_CAPACITY-1,&index);
  if(action==SUMMARY)valid=valid&&integer(p,"actor",PET_SCHED_TASK_PROTOCOL,&actor)&&actor>=PET_SCHED_TASK_MAIN;
  cJSON_Delete(root);
  if(!valid){error(send,ctx,"invalid_request");return true;}
  if(action==START){
    if(audio_busy()){error(send,ctx,"audio_busy");return true;}
    generation=pet_sched_trace_begin(duration);
    if(!generation){error(send,ctx,"unavailable");return true;}
  }else if(action==STOP&&!pet_sched_trace_stop(generation)){
    error(send,ctx,"stale_generation");return true;
  }
  pet_sched_trace_status_t status;
  if(!pet_sched_trace_status(generation,&status)){error(send,ctx,"stale_generation");return true;}
  char reply[640];int n;
  if(action==SUMMARY){
    pet_sched_actor_summary_t s;
    if(!pet_sched_trace_summary(generation,(pet_sched_task_t)actor,&s)){error(send,ctx,"unavailable_record");return true;}
    n=snprintf(reply,sizeof(reply),"{\"topic\":\"debug/sched_result\",\"payload\":{\"ok\":true,\"generation\":%u,\"actor\":%u,\"waitPairs\":%u,\"waitTotalUs\":%" PRIu64 ",\"waitMaxUs\":%u,\"holdPairs\":%u,\"holdBeforeReleaseTotalUs\":%" PRIu64 ",\"holdBeforeReleaseMaxUs\":%u,\"releasePairs\":%u,\"releaseTotalUs\":%" PRIu64 ",\"releaseMaxUs\":%u,\"unpaired\":%u,\"inheritZero\":%u,\"inheritNonzero\":%u,\"disinheritZero\":%u,\"disinheritNonzero\":%u,\"phase\":%u}}",
      generation,actor,s.wait_pairs,s.wait_total_us,s.wait_max_us,s.hold_pairs,s.hold_before_release_total_us,s.hold_before_release_max_us,
      s.release_pairs,s.release_total_us,s.release_max_us,s.unpaired,s.inherit_zero,s.inherit_nonzero,s.disinherit_zero,s.disinherit_nonzero,s.phase);
  }else if(action==READ){
    pet_sched_event_t e;
    if(!pet_sched_trace_read(generation,index,&e)){error(send,ctx,"unavailable_record");return true;}
    n=snprintf(reply,sizeof(reply),"{\"topic\":\"debug/sched_result\",\"payload\":{\"ok\":true,\"generation\":%u,\"index\":%u,\"elapsedUs\":%u,\"kind\":%u,\"core\":%u,\"actor\":%u,\"peer\":%u,\"marker\":%u,\"value\":%u}}",
      generation,index,e.elapsed_us,e.kind,e.core,e.actor,e.peer,e.marker,e.value);
  }else n=snprintf(reply,sizeof(reply),"{\"topic\":\"debug/sched_result\",\"payload\":{\"ok\":true,\"engine\":\"p4-partial-scheduler-evidence-v1\",\"generation\":%u,\"active\":%s,\"durationUs\":%u,\"count\":%u,\"overwritten\":%u,\"flags\":%u,\"startedUs\":%" PRIu64 ",\"endedUs\":%" PRIu64 "}}",
    generation,status.active?"true":"false",status.duration_us,status.count,status.overwritten,status.flags,status.started_us,status.ended_us);
  if(n<0||(size_t)n>=sizeof(reply))error(send,ctx,"reply_overflow");else send(reply,ctx);
  return true;
}
#endif
