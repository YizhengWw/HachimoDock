/* Real passive observer + JSON adapter, synthetic platform, no device/IO. */
#define main existing_trace_test_main
#include "p4_sched_trace_test.c"
#undef main
#include "pet_p4_sched_trace_rpc.h"
#include "cJSON.h"
#include <stdio.h>
static char reply[768];static unsigned sends;static bool audio;
bool pet_p4_audio_active(void){return audio;}
bool pet_p4_audio_playback_active(void){return false;}
bool pet_p4_audio_conversation_active(void){return false;}
bool pet_p4_audio_music_active(void){return false;}
static void send_reply(const char *line,void *context){
  assert(context==&sends&&strlen(line)<sizeof(reply));strcpy(reply,line);sends++;
}
static cJSON *request(const char *topic,const char *payload){
  char line[512];snprintf(line,sizeof(line),"{\"topic\":\"%s\",\"payload\":%s}",topic,payload);
  unsigned before=sends;assert(pet_p4_sched_trace_rpc_handle(line,send_reply,&sends)&&sends==before+1);
  cJSON *root=cJSON_Parse(reply);assert(root);cJSON *p=cJSON_DetachItemFromObject(root,"payload");cJSON_Delete(root);return p;
}
static void expect_error(const char *topic,const char *payload,const char *code){
  cJSON *p=request(topic,payload);assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"ok")));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"error")->valuestring,code));cJSON_Delete(p);
}
int main(void){
  setup();
  assert(!pet_p4_sched_trace_rpc_handle("{\"topic\":\"unrelated\",\"payload\":{}}",send_reply,&sends)&&!sends);
  assert(!pet_p4_sched_trace_rpc_handle("{\"topic\":\"debug/sched_unknown\",\"payload\":{}}",send_reply,&sends)&&!sends);
  const char *invalid[]={"{}","{\"durationUs\":0}","{\"durationUs\":600001}","{\"durationUs\":true}","{\"durationUs\":\"10\"}","{\"durationUs\":1.5}","{\"durationUs\":10,\"extra\":1}","{\"durationUs\":10,\"durationUs\":20}","{\"durationUs\\u0000\":10}"};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)expect_error("debug/sched_start",invalid[i],"invalid_request");
  audio=true;expect_error("debug/sched_start","{\"durationUs\":1000}","audio_busy");audio=false;
  cJSON *p=request("debug/sched_start","{\"durationUs\":1000}");
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"ok"))&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"active")));
  unsigned g=(unsigned)cJSON_GetObjectItemCaseSensitive(p,"generation")->valuedouble;assert(g);cJSON_Delete(p);
  expect_error("debug/sched_start","{\"durationUs\":1000}","unavailable");
  char data[96];snprintf(data,sizeof(data),"{\"generation\":%u,\"index\":0}",g);
  expect_error("debug/sched_read",data,"unavailable_record");
  snprintf(data,sizeof(data),"{\"generation\":%u,\"actor\":1}",g);
  expect_error("debug/sched_summary",data,"unavailable_record");
  pet_sched_trace_mark(PET_SCHED_MARK_PPA_BEGIN,123);atomic_fetch_add(&clock_us,10);
  snprintf(data,sizeof(data),"{\"generation\":%u}",g);p=request("debug/sched_stop",data);
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"ok"))&&cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"active")));
  assert(cJSON_GetObjectItemCaseSensitive(p,"count")->valuedouble==1);cJSON_Delete(p);
  p=request("debug/sched_status",data);assert(cJSON_GetObjectItemCaseSensitive(p,"flags")->valuedouble==PET_SCHED_FLAG_PARTIAL_WRAP_COVERAGE);cJSON_Delete(p);
  snprintf(data,sizeof(data),"{\"generation\":%u,\"index\":0}",g);p=request("debug/sched_read",data);
  assert(cJSON_GetObjectItemCaseSensitive(p,"value")->valuedouble==123&&cJSON_GetObjectItemCaseSensitive(p,"actor")->valuedouble==PET_SCHED_TASK_MAIN);cJSON_Delete(p);
  expect_error("debug/sched_read","{\"generation\":1,\"index\":1024}","invalid_request");
  snprintf(data,sizeof(data),"{\"generation\":%u,\"actor\":1}",g);p=request("debug/sched_summary",data);
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"ok")));
  assert(cJSON_GetObjectItemCaseSensitive(p,"actor")->valuedouble==1);
  assert(cJSON_GetObjectItemCaseSensitive(p,"waitPairs")->valuedouble==0);
  assert(cJSON_GetObjectItemCaseSensitive(p,"disinheritZero")->valuedouble==0);cJSON_Delete(p);
  expect_error("debug/sched_summary","{\"generation\":1,\"actor\":0}","invalid_request");
  expect_error("debug/sched_summary","{\"generation\":1,\"actor\":4}","invalid_request");
  expect_error("debug/sched_summary","{\"generation\":1,\"actor\":true}","invalid_request");
  expect_error("debug/sched_summary","{\"generation\":1,\"actor\":1,\"index\":0}","invalid_request");
  snprintf(data,sizeof(data),"{\"generation\":%u}",g+1);expect_error("debug/sched_status",data,"stale_generation");
  puts("PASS numeric opt-in scheduler diagnostic boundaries");return 0;
}
