/* Actual parser/serializer with counted observer/audio adapters only. */
#include "pet_p4_storage_io_trace_rpc.h"
#include "pet_p4_audio.h"
#include "cJSON.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned begins,stops,statuses,reads,sends,latests;
static bool audio,unavailable;
static cJSON *response;
static char boot[17]="0123456789abcdef";
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
static unsigned completed_reads;
bool pet_storage_io_completed_read(pet_storage_io_actor_t owner,uint32_t expected,pet_storage_io_actor_t actor,
    pet_storage_io_status_t *status,pet_storage_io_counter_t out[3]){
 completed_reads++;if(unavailable||owner!=3||actor!=3||(expected&&expected!=7))return false;
 *status=(pet_storage_io_status_t){.generation=7,.complete=true,.owner_actor=3,.started_us=UINT64_MAX,.stop_requested_us=UINT64_MAX};
 for(unsigned i=0;i<3;i++)out[i]=(pet_storage_io_counter_t){.bytes=UINT64_MAX,.wall_us=UINT64_MAX,.calls=UINT32_MAX,.errors=UINT32_MAX,.max_wall_us=UINT32_MAX,.cache_hits=UINT32_MAX,.buckets={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX}};
 return true;
}
#endif
const char *pet_p4_boot_identity(void){return boot;}
bool pet_p4_audio_active(void){return audio;}
bool pet_p4_audio_playback_active(void){return false;}
bool pet_p4_audio_conversation_active(void){return false;}
bool pet_p4_audio_music_active(void){return false;}
uint32_t pet_storage_io_begin(uint32_t duration){assert(duration&&duration<=5000000);begins++;return unavailable?0:7;}
bool pet_storage_io_stop(uint32_t generation){stops++;return generation==7;}
bool pet_storage_io_status(uint32_t generation,pet_storage_io_status_t *out){
 statuses++;if(generation!=7)return false;*out=(pet_storage_io_status_t){.generation=7,.duration_us=5000000,.complete=true};return true;
}
bool pet_storage_io_read(uint32_t generation,pet_storage_io_actor_t actor,pet_storage_io_operation_t op,pet_storage_io_counter_t *out){
 reads++;assert(generation==7&&actor>=1&&actor<=4&&op<=2);
 *out=(pet_storage_io_counter_t){.calls=UINT32_MAX,.errors=UINT32_MAX,.max_wall_us=UINT32_MAX,
   .bytes=UINT64_MAX,.wall_us=UINT64_MAX,.buckets={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX}};return true;
}
bool pet_storage_io_latest(pet_storage_io_status_t *out){
 latests++;if(unavailable)return false;*out=(pet_storage_io_status_t){.generation=7,.duration_us=5000000,.complete=true};return true;
}
static void send(const char *text,void *context){
 assert(context==(void*)1&&strlen(text)<(strstr(text,"p4-storage-completed-v1")?1280:1024)&&!strstr(text,"SECRET")&&!strstr(text,"/spiffs")&&!strstr(text,"address"));
 cJSON_Delete(response);response=cJSON_Parse(text);assert(response);sends++;
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(response,"topic")->valuestring,"debug/storage_io_result"));
}
static bool call(const char *topic,const char *payload){
 cJSON *input=cJSON_Parse(payload);assert(input);if(!cJSON_HasObjectItem(input,"bootId"))cJSON_AddStringToObject(input,"bootId",boot);
 char *encoded=cJSON_PrintUnformatted(input);assert(encoded);
 char line[1024];snprintf(line,sizeof(line),"{\"topic\":\"%s\",\"payload\":%s}",topic,encoded);cJSON_free(encoded);cJSON_Delete(input);
 assert(pet_storage_io_rpc_handle(line,send,(void*)1));
 const cJSON *p=cJSON_GetObjectItemCaseSensitive(response,"payload");assert(p);
 return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"ok"));
}
int main(void){
 assert(call("debug/storage_io_begin","{\"requestId\":\"test-1\",\"durationUs\":5000000}"));assert(begins==1&&statuses==1);
 assert(call("debug/storage_io_read","{\"requestId\":\"read\",\"generation\":7,\"actor\":3,\"operation\":0}"));assert(reads==1);
 const cJSON *p=cJSON_GetObjectItemCaseSensitive(response,"payload");
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"requestedBytes")->valuestring,"18446744073709551615"));
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"wallUs")->valuestring,"18446744073709551615"));
 const char *bad[]={
  "{\"requestId\":\"bad\",\"durationUs\":5000001}","{\"requestId\":\"bad\",\"durationUs\":0}",
  "{\"requestId\":\"bad\",\"durationUs\":true}","{\"requestId\":\"bad\",\"durationUs\":1.5}",
  "{\"requestId\":\"bad\",\"durationUs\":\"100\"}","{\"requestId\":\"bad\",\"requestId\":\"other\",\"durationUs\":100}",
  "{\"requestId\":\"bad\",\"durationUs\":100,\"path\":\"SECRET/path\"}","{\"requestId\":\"SECRET private request\",\"durationUs\":100}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!call("debug/storage_io_begin",bad[i]));assert(begins==1);
 assert(!call("debug/storage_io_read","{\"requestId\":\"bad\",\"generation\":7,\"actor\":0,\"operation\":0}"));
 assert(!call("debug/storage_io_read","{\"requestId\":\"bad\",\"generation\":7,\"actor\":1,\"operation\":-1}"));assert(reads==1);
 assert(!call("debug/storage_io_stop","{\"requestId\":\"bad\",\"generation\":4294967296}"));assert(!stops);
 unsigned before=sends;assert(pet_storage_io_rpc_handle("{\"topic\":\"debug/storage_io_begin\",\"topic\":\"debug/storage_io_begin\",\"payload\":{\"requestId\":\"bad\",\"durationUs\":100}}",send,(void*)1));assert(sends==before+1&&begins==1);
 audio=true;assert(!call("debug/storage_io_begin","{\"requestId\":\"audio\",\"durationUs\":100}"));assert(begins==1);audio=false;
 unavailable=true;assert(!call("debug/storage_io_begin","{\"requestId\":\"busy\",\"durationUs\":100}"));assert(begins==2);
 assert(!call("debug/storage_io_stop","{\"requestId\":\"stale\",\"generation\":8}"));assert(stops==1);
 assert(call("debug/storage_io_stop","{\"requestId\":\"stop\",\"generation\":7}"));assert(stops==2);
 strcpy(boot,"1111111111111111");
 assert(!call("debug/storage_io_stop","{\"requestId\":\"old\",\"bootId\":\"0123456789abcdef\",\"generation\":7}"));assert(stops==2);
 assert(!call("debug/storage_io_begin","{\"requestId\":\"old\",\"bootId\":\"0123456789abcdef\",\"durationUs\":100}"));assert(begins==2);
 before=sends;assert(!pet_storage_io_rpc_handle("{\"topic\":\"other\",\"payload\":{\"note\":\"debug/storage_io_begin\"}}",send,(void*)1));assert(before==sends);
 assert(!pet_storage_io_rpc_handle("{\"topic\":\"debug/storage_io_begin\",\"payload\":{\"requestId\":\"x\",\"durationUs\":100}} trailing",send,(void*)1));
 unavailable=false;unsigned old_begins=begins,old_stops=stops,old_statuses=statuses;
 assert(call("debug/storage_io_latest","{\"requestId\":\"latest\"}"));assert(latests==1&&begins==old_begins&&stops==old_stops&&statuses==old_statuses);
 assert(!call("debug/storage_io_latest","{\"requestId\":\"latest\",\"generation\":7}"));assert(latests==1);
 assert(!call("debug/storage_io_latest","{\"requestId\":\"old\",\"bootId\":\"0123456789abcdef\"}"));assert(latests==1);
 unavailable=true;assert(!call("debug/storage_io_latest","{\"requestId\":\"missing\"}"));assert(latests==2&&begins==old_begins&&stops==old_stops);
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
 unavailable=false;
 char payload[256];char id[65];memset(id,'c',64);id[64]=0;
 snprintf(payload,sizeof(payload),"{\"requestId\":\"%s\",\"owner\":3,\"generation\":0,\"actor\":3}",id);
 assert(call("debug/storage_io_completed",payload)&&completed_reads==1);
 p=cJSON_GetObjectItemCaseSensitive(response,"payload");const cJSON *ops=cJSON_GetObjectItemCaseSensitive(p,"operations");
 assert(cJSON_GetArraySize(ops)==3);
 for(unsigned i=0;i<3;i++)assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(ops,i),"requestedBytes")->valuestring,"18446744073709551615"));
 assert(!call("debug/storage_io_completed","{\"requestId\":\"bad\",\"owner\":4,\"generation\":0,\"actor\":3}")&&completed_reads==1);
 assert(!call("debug/storage_io_completed","{\"requestId\":\"old\",\"owner\":3,\"generation\":0,\"actor\":3,\"bootId\":\"0123456789abcdef\"}")&&completed_reads==1);
 assert(!call("debug/storage_io_completed","{\"requestId\":\"stale\",\"owner\":3,\"generation\":8,\"actor\":3}")&&completed_reads==2);
 assert(!call("debug/storage_io_completed","{\"requestId\":\"bad\",\"owner\":3,\"generation\":0,\"actor\":3,\"path\":\"SECRET\"}")&&completed_reads==2);
 assert(begins==old_begins&&stops==old_stops);
#endif
 cJSON_Delete(response);puts("PASS private HAL RPC strict schema, redaction, u64 exact metadata, no invalid begin");return 0;
}
