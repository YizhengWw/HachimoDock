#include "pet_p4_storage_io_trace_rpc.h"
#if PET_P4_STORAGE_IO_TRACE_ENABLE
#include "pet_p4_audio.h"
#include "pet_p4_json_source.h"
#include "cJSON.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static bool io_integer(const cJSON *p,const char *key,uint32_t lo,uint32_t hi,uint32_t *out){
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,key);
  if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<lo||v->valuedouble>hi)return false;
  uint32_t n=(uint32_t)v->valuedouble;if((double)n!=v->valuedouble)return false;*out=n;return true;
}
static const char *io_request_id(const cJSON *p){
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,"requestId");
  if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>64)return "";
  for(const unsigned char *s=(const unsigned char*)v->valuestring;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'||*s=='.'))return "";
  return v->valuestring;
}
static bool io_fields(const cJSON *p,const char *const *keys,unsigned count){
  if(!cJSON_IsObject(p)||cJSON_GetArraySize(p)!=(int)count)return false;
  for(unsigned i=0;i<count;i++){unsigned seen=0;const cJSON *item;cJSON_ArrayForEach(item,p)if(item->string&&!strcmp(item->string,keys[i]))seen++;if(seen!=1)return false;}
  return true;
}
static bool io_audio_busy(void){return pet_p4_audio_active()||pet_p4_audio_playback_active()||pet_p4_audio_conversation_active()||pet_p4_audio_music_active();}
static bool io_boot_current(const cJSON *p){
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,"bootId");const char *current=pet_p4_boot_identity();
  if(!cJSON_IsString(v)||strlen(v->valuestring)!=16||!current||strlen(current)!=16)return false;
  return !strcmp(current,v->valuestring);
}
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
/* One numeric reply owns its immutable completed generation. No physical IO,
 * cache reset, admission, filenames, task pointers or private payloads. */
static __attribute__((noinline)) void io_completed_reply(const cJSON *root,const cJSON *p,pet_storage_io_send_fn send,void *context){
  const char *const envelope[]={"topic","payload"},*const keys[]={"requestId","bootId","owner","generation","actor"};
  uint32_t owner=0,generation=0,actor=0;
  bool ok=io_fields(root,envelope,2)&&io_fields(p,keys,5)&&io_request_id(p)[0]
    &&io_integer(p,"owner",1,3,&owner)&&io_integer(p,"generation",0,UINT32_MAX,&generation)&&io_integer(p,"actor",1,4,&actor);
  const char *error=ok?"":"invalid_request";
  pet_storage_io_status_t status={0};pet_storage_io_counter_t values[3]={{0}};
  if(ok&&!io_boot_current(p)){ok=false;error="boot_changed";}
  if(ok&&!pet_storage_io_completed_read((pet_storage_io_actor_t)owner,generation,(pet_storage_io_actor_t)actor,&status,values)){
    ok=false;error="completed_generation_unavailable";
  }
  char response[1280];
  int n=snprintf(response,sizeof(response),"{\"topic\":\"debug/storage_io_result\",\"payload\":{\"requestId\":\"%s\",\"bootId\":\"%s\",\"engine\":\"p4-storage-completed-v1\",\"ok\":%s,\"error\":\"%s\",\"owner\":%u,\"actor\":%u,\"generation\":%"PRIu32",\"complete\":%s,\"flags\":%"PRIu32",\"startedUs\":\"%"PRIu64"\",\"stoppedUs\":\"%"PRIu64"\",\"operations\":[",
    io_request_id(p),pet_p4_boot_identity(),ok?"true":"false",error,(unsigned)owner,(unsigned)actor,status.generation,status.complete?"true":"false",status.flags,status.started_us,status.stop_requested_us);
  if(n<0||(size_t)n>=sizeof(response))return;
  for(unsigned i=0;i<3;i++){
    const pet_storage_io_counter_t *v=&values[i];
    int added=snprintf(response+n,sizeof(response)-(size_t)n,"%s{\"operation\":%u,\"calls\":%"PRIu32",\"errors\":%"PRIu32",\"cacheHits\":%"PRIu32",\"requestedBytes\":\"%"PRIu64"\",\"wallUs\":\"%"PRIu64"\",\"maxWallUs\":%"PRIu32",\"sizeBuckets\":[%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32"]}",
      i?",":"",i,v->calls,v->errors,v->cache_hits,v->bytes,v->wall_us,v->max_wall_us,v->buckets[0],v->buckets[1],v->buckets[2],v->buckets[3]);
    if(added<0||(size_t)added>=sizeof(response)-(size_t)n)return;n+=added;
  }
  if((size_t)n+4<=sizeof(response)){memcpy(response+n,"]}}",4);send(response,context);}
}
#endif
bool pet_storage_io_rpc_handle(const char *line,pet_storage_io_send_fn send,void *context){
  if(!line||!send||!strstr(line,"debug/storage_io_"))return false;
  if(!pet_json_source_bounded_text(line))return false; /* Ordinary invalid-json path, no parser repair. */
  cJSON *root=cJSON_ParseWithOpts(line,NULL,true);const cJSON *t=cJSON_GetObjectItemCaseSensitive(root,"topic"),*p=cJSON_GetObjectItemCaseSensitive(root,"payload");
  const char *topic=cJSON_IsString(t)?t->valuestring:"";enum {BEGIN,STOP,STATUS,READ,LATEST,UNKNOWN} action=UNKNOWN;
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
  if(!strcmp(topic,"debug/storage_io_completed")){io_completed_reply(root,p,send,context);cJSON_Delete(root);return true;}
#endif
  if(!strcmp(topic,"debug/storage_io_begin"))action=BEGIN;else if(!strcmp(topic,"debug/storage_io_stop"))action=STOP;
  else if(!strcmp(topic,"debug/storage_io_status"))action=STATUS;else if(!strcmp(topic,"debug/storage_io_read"))action=READ;
  else if(!strcmp(topic,"debug/storage_io_latest"))action=LATEST;
  if(action==UNKNOWN){cJSON_Delete(root);return false;}
  const char *const envelope_keys[]={"topic","payload"},*const begin_keys[]={"requestId","bootId","durationUs"},*const state_keys[]={"requestId","bootId","generation"},*const read_keys[]={"requestId","bootId","generation","actor","operation"},*const latest_keys[]={"requestId","bootId"};
  uint32_t generation=0,duration=0,actor=0,operation=0;
  bool valid=io_fields(root,envelope_keys,2)&&io_request_id(p)[0]&&io_fields(p,action==BEGIN?begin_keys:action==READ?read_keys:action==LATEST?latest_keys:state_keys,action==READ?5:action==LATEST?2:3);
  if(valid&&action!=LATEST)valid=action==BEGIN?io_integer(p,"durationUs",1,PET_STORAGE_IO_MAX_WINDOW_US,&duration):io_integer(p,"generation",1,UINT32_MAX,&generation);
  if(valid&&action==READ)valid=io_integer(p,"actor",1,4,&actor)&&io_integer(p,"operation",0,2,&operation);
  const char *error=valid?"":"invalid_request";pet_storage_io_status_t status={0};pet_storage_io_counter_t counter={0};bool ok=valid;
  if(ok&&!io_boot_current(p)){ok=false;error="boot_changed";}
  if(ok&&action==BEGIN){if(io_audio_busy()){ok=false;error="audio_busy";}else{generation=pet_storage_io_begin(duration);if(!generation){ok=false;error="window_unavailable";}}}
  if(ok&&action==STOP&&!pet_storage_io_stop(generation)){ok=false;error="stale_generation";}
  if(ok&&action==LATEST){if(pet_storage_io_latest(&status))generation=status.generation;else{ok=false;error="window_unavailable";}}
  if(ok&&action!=LATEST&&!pet_storage_io_status(generation,&status)){ok=false;error="stale_generation";}
  if(ok&&action==READ&&!pet_storage_io_read(generation,(pet_storage_io_actor_t)actor,(pet_storage_io_operation_t)operation,&counter)){ok=false;error="window_not_readable";}
  char response[1024];
  int n=snprintf(response,sizeof(response),"{\"topic\":\"debug/storage_io_result\",\"payload\":{\"requestId\":\"%s\",\"bootId\":\"%s\",\"engine\":\"p4-storage-io-trace-v1\",\"ok\":%s,\"error\":\"%s\",\"generation\":%"PRIu32",\"durationUs\":%"PRIu32",\"active\":%s,\"complete\":%s,\"inflight\":%"PRIu32",\"flags\":%"PRIu32,
    io_request_id(p),pet_p4_boot_identity(),ok?"true":"false",error,generation,status.duration_us,status.active?"true":"false",status.complete?"true":"false",status.inflight,status.flags);
  if(n<0||(size_t)n>=sizeof(response)){cJSON_Delete(root);return true;}
  if(ok&&action==READ){
    int added=snprintf(response+n,sizeof(response)-(size_t)n,",\"actor\":%"PRIu32",\"operation\":%"PRIu32",\"calls\":%"PRIu32",\"errors\":%"PRIu32",\"cacheHits\":%"PRIu32",\"requestedBytes\":\"%"PRIu64"\",\"wallUs\":\"%"PRIu64"\",\"maxWallUs\":%"PRIu32",\"sizeBuckets\":[%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32"]",
      actor,operation,counter.calls,counter.errors,counter.cache_hits,counter.bytes,counter.wall_us,counter.max_wall_us,counter.buckets[0],counter.buckets[1],counter.buckets[2],counter.buckets[3]);
    if(added<0||(size_t)added>=sizeof(response)-(size_t)n){cJSON_Delete(root);return true;}n+=added;
  }
  if((size_t)n+3<=sizeof(response)){memcpy(response+n,"}}",3);send(response,context);}
  cJSON_Delete(root);return true;
}
#endif
