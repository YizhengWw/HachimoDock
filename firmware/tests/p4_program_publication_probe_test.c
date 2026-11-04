#include "pet_p4_program_probe.h"
#include "pet_p4_resources_device.h"
#include "freertos/FreeRTOS.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int64_t clock_us=1000;
static bool busy,reenter;
static unsigned delays,stop_after,reads[4],responses;
static char messages[2][512];
int64_t esp_timer_get_time(void){int64_t value=clock_us;clock_us+=10;return value;}
void vTaskDelay(TickType_t ticks){assert(ticks==1);delays++;if(stop_after&&delays==stop_after)busy=true;}
bool pet_p4_audio_active(void){reads[0]++;return busy;}
bool pet_p4_audio_playback_active(void){reads[1]++;return false;}
bool pet_p4_audio_conversation_active(void){reads[2]++;return false;}
bool pet_p4_audio_music_active(void){reads[3]++;return false;}
static void receive(const char *line,void *context){
  assert(context==&responses&&responses<2&&strlen(line)<512);strcpy(messages[responses++],line);
  if(reenter){reenter=false;pet_p4_program_publication_probe(receive,context);}
}
static cJSON *invoke(void){
  responses=delays=0;pet_p4_program_publication_probe(receive,&responses);assert(responses>=1);
  cJSON *root=cJSON_Parse(messages[0]);assert(root);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root,"topic")->valuestring,"debug/program_publication_result"));return root;
}
static void clean(void){
  pet_resource_usage_t used;pet_resources_device_usage(&used);assert(!used.live_owners&&!used.revoked_owners&&!used.realtime_reservations);
  for(unsigned i=0;i<PET_RESOURCE_KIND_COUNT;i++)assert(!used.leases[i]);
  for(unsigned i=1;i<4;i++)assert(reads[i]==reads[0]);
}
int main(void){
  reenter=true;cJSON *root=invoke(),*p=cJSON_GetObjectItemCaseSensitive(root,"payload");
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"ok"))&&responses==2);
  assert(cJSON_GetObjectItemCaseSensitive(p,"samples")->valueint==128);
  assert(cJSON_GetObjectItemCaseSensitive(p,"minUs")->valueint==10&&cJSON_GetObjectItemCaseSensitive(p,"maxUs")->valueint==10);
  assert(cJSON_GetObjectItemCaseSensitive(p,"sumUs")->valueint==1280);
  assert(cJSON_GetObjectItemCaseSensitive(p,"criticalSamples")->valueint==0);
  assert(cJSON_GetObjectItemCaseSensitive(p,"criticalMaxCycles")->valueint==0);
  assert(cJSON_GetObjectItemCaseSensitive(p,"configuredCpuHz")->valueint==0);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"storage")->valuestring,"host-stub"));cJSON_Delete(root);clean();
  assert(strstr(messages[1],"busy"));root=invoke();assert(strstr(messages[0],"rate_limited"));cJSON_Delete(root);clean();
  clock_us+=6000000;stop_after=3;root=invoke();p=cJSON_GetObjectItemCaseSensitive(root,"payload");
  assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"ok"))&&cJSON_GetObjectItemCaseSensitive(p,"samples")->valueint==3);
  assert(strstr(messages[0],"cancelled"));cJSON_Delete(root);clean();
  busy=false;stop_after=0;clock_us+=6000000;root=invoke();assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"payload"),"ok")));cJSON_Delete(root);clean();
  clock_us=-1;root=invoke();assert(strstr(messages[0],"clock_error"));cJSON_Delete(root);clean();
  puts("PASS fixed-vector publication probe: actual broker, precise phases, cancellation, rate/reentry, no program grant");return 0;
}
