/* Actual patched vendor implementations, public constructors, fake bus only. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "audio_codec_sw_vol.h"
#include "driver/i2c_master.h"
#include "freertos/task.h"
#include "pet_p4_audio_volume_deadline.h"
#include "pet_p4_audio_volume.h"
#include "pet_p4_resources_device.h"
#include "nvs.h"

static int64_t now;
static void *task = (void *)1;
static uint8_t regs[256];
static int fail_read=-1, fail_write=-1, reads,writes, last_timeout, cost_us;
static int ignore_write=-1, lock_busy, lock_depth, nvs_opened, nvs_reads, nvs_writes, nvs_commits;
static int nvs_fail_open, nvs_fail_get=ESP_ERR_NVS_NOT_FOUND, nvs_fail_set, nvs_fail_commit;
static bool audio_idle=true,revoke_on_set,queue_on_open;
static unsigned stored_value;
static pet_resource_broker_t broker;
static pet_resource_owner_t storage_owner;
static pet_resource_realtime_t raced_audio;
int64_t esp_timer_get_time(void){return now;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return task;}
void vTaskDelay(TickType_t ticks){now+=(int64_t)ticks*1000;}
void esp_codec_dev_sleep(int ms){now+=(int64_t)ms*1000;}
esp_err_t i2c_master_bus_add_device(void *bus,const i2c_device_config_t *cfg,void **out){
  assert(bus && cfg->device_address==0x18 && cfg->scl_speed_hz==100000);*out=regs;return 0;
}
esp_err_t i2c_master_bus_rm_device(void *dev){assert(dev==regs);return 0;}
esp_err_t i2c_master_transmit_receive(void *dev,const uint8_t *tx,size_t tn,uint8_t *rx,size_t rn,int ms){
  assert(dev==regs && tn==1 && rn==1 && ms>0);reads++;last_timeout=ms;
  if(cost_us>ms*1000){now+=ms*1000;return ESP_ERR_TIMEOUT;}now+=cost_us;
  if(tx[0]==fail_read)return ESP_FAIL;
  *rx=regs[tx[0]];return 0;
}
esp_err_t i2c_master_transmit(void *dev,const uint8_t *tx,size_t n,int ms){
  assert(dev==regs && n==2 && ms>0);writes++;last_timeout=ms;
  if(cost_us>ms*1000){now+=ms*1000;return ESP_ERR_TIMEOUT;}now+=cost_us;
  if(tx[0]==fail_write)return ESP_FAIL;
  if(tx[0]!=ignore_write)regs[tx[0]]=tx[1];
  return 0;
}
const audio_codec_vol_if_t *audio_codec_new_sw_vol(void){assert(0);return NULL;}
static bool data_open(const audio_codec_data_if_t *h){(void)h;return true;}
static int data_write(const audio_codec_data_if_t *h,uint8_t *data,int n){(void)h;(void)data;(void)n;return 0;}
int xSemaphoreTakeRecursive(void *lock,TickType_t ticks){assert(lock && ticks<=5);if(lock_busy)return pdFALSE;lock_depth++;return pdTRUE;}
int xSemaphoreGiveRecursive(void *lock){assert(lock && lock_depth>0);lock_depth--;return pdTRUE;}
void pet_resources_device_usage(pet_resource_usage_t *out){pet_resources_usage(&broker,out);}
pet_resource_result_t pet_resources_device_owner_open(uint32_t mask,pet_resource_owner_t *out){
  pet_resource_result_t r=pet_resources_owner_open(&broker,mask,out);if(r==PET_RESOURCE_OK)storage_owner=*out;return r;
}
pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t owner,pet_resource_kind_t kind,uint32_t units,pet_resource_lease_t *out){return pet_resources_try_acquire(&broker,owner,kind,units,out);}
bool pet_resources_device_current(pet_resource_lease_t lease){return pet_resources_current(&broker,lease);}
pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t lease){assert(!nvs_opened);return pet_resources_finish(&broker,lease);}
void pet_resources_device_revoke(pet_resource_owner_t owner){pet_resources_owner_revoke(&broker,owner);}
static bool idle(void){return audio_idle;}
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out){
  assert(!strcmp(name,"p4_audio") && lock_depth==0 && !nvs_opened);
  if(nvs_fail_open)return nvs_fail_open;
  if(mode==NVS_READWRITE){assert(pet_resources_owner_current(&broker,storage_owner));if(queue_on_open)audio_idle=false;}
  nvs_opened=1;*out=1;return 0;
}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *value){assert(h==1 && !strcmp(key,"master_v1"));nvs_reads++;*value=(uint8_t)stored_value;return nvs_fail_get;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t value){
  assert(h==1 && !strcmp(key,"master_v1") && lock_depth==0 && value<=100);nvs_writes++;
  if(revoke_on_set)assert(pet_resources_realtime_begin(&broker,&raced_audio)==PET_RESOURCE_OK);
  stored_value=value;return nvs_fail_set;
}
esp_err_t nvs_commit(nvs_handle_t h){assert(h==1 && lock_depth==0);nvs_commits++;return nvs_fail_commit;}
void nvs_close(nvs_handle_t h){assert(h==1 && nvs_opened && lock_depth==0);nvs_opened=0;}
static void no_resources(void){pet_resource_usage_t u;pet_resources_usage(&broker,&u);assert(!u.live_owners && !u.revoked_owners && !u.leases[PET_RESOURCE_STORAGE_WORK]);}
static cJSON *wire(const char *input,bool want_ok){
  cJSON *request=cJSON_Parse(input);assert(request);bool ok;
  char *line=pet_audio_volume_reply(request,"board-1",true,&ok);assert(line && ok==want_ok);
  cJSON *response=cJSON_Parse(line);assert(response);cJSON_free(line);cJSON_Delete(request);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(response,"topic")->valuestring,"audio/volume-state"));
  cJSON *p=cJSON_DetachItemFromObjectCaseSensitive(response,"payload");cJSON_Delete(response);
  assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(p,"ok")));
  assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(p,"persisted")));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"boardDeviceId")->valuestring,"board-1"));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"engine")->valuestring,PET_AUDIO_MASTER_VOLUME_ENGINE));
  return p;
}
static void runtime_tests(esp_codec_dev_handle_t dev,const char *scenario){
  bool corrupt=!strcmp(scenario,"corrupt");
  if(corrupt)nvs_fail_get=ESP_ERR_NVS_TYPE_MISMATCH;
  bool range=!strcmp(scenario,"out-of-range");
  if(range){nvs_fail_get=0;stored_value=250;}
  bool boot_mute=!strcmp(scenario,"boot-mute");
  bool boot_low=!strcmp(scenario,"boot-low");
  if(boot_mute || boot_low){nvs_fail_get=0;stored_value=boot_low?35:0;}
  unsigned expected_boot=boot_mute?0:boot_low?35:100;
  assert(pet_audio_volume_boot_load()==expected_boot && nvs_reads==1);
  cost_us=0;now=0;
  assert(pet_audio_volume_attach(dev,(void *)1,(void *)1)==0 && !lock_depth);
  pet_audio_volume_status_t s;
  assert(pet_audio_volume_get(&s)==0 && s.volume==(int)expected_boot && s.persisted==!(corrupt || range));
  assert((regs[0x31]&0x60)==(boot_mute?0x60:0));
  assert(!nvs_writes && !pet_audio_volume_pending());
  if(boot_mute || boot_low) return;
  const char *invalid[]={
    "{\"operation\":\"set\",\"volume\":1.1}","{\"operation\":\"set\",\"volume\":-1}",
    "{\"operation\":\"set\",\"volume\":101}","{\"operation\":\"set\",\"volume\":true}",
    "{\"operation\":\"set\",\"volume\":\"35\"}","{\"operation\":\"set\",\"volume\":null}",
    "{\"operation\":\"set\",\"volume\":1e999}","{\"operation\":\"set\"}",
    "{\"operation\":\"query\",\"volume\":35}","{\"operation\":\"SET\",\"volume\":35}",
    "{\"operation\":\"set\",\"volume\":35,\"volume\":50}",
    "{\"operation\":\"set\",\"volume\":35,\"extra\":1}",
    "{\"operation\":\"query\",\"operation\":\"set\"}",
    "{\"operation\":\"query\",\"requestId\":\"duplicate\"}",
    "{\"operation\":\"query\",\"expectedBoardDeviceId\":\"duplicate\"}"};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){
    char input[512];snprintf(input,sizeof(input),"{\"topic\":\"audio/volume\",\"payload\":{\"requestId\":\"r\",\"expectedBoardDeviceId\":\"board-1\",%s",invalid[i]+1);
    size_t n=strlen(input);input[n]='}';input[n+1]=0;
    int before=reads+writes;cJSON *p=wire(input,false);assert(reads+writes==before);cJSON_Delete(p);
  }
  cJSON *p=wire("{\"topic\":\"audio/volume\",\"payload\":{\"operation\":\"set\",\"volume\":35,\"requestId\":\"wrong\",\"expectedBoardDeviceId\":\"other\"}}",false);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"error")->valuestring,"board_mismatch"));cJSON_Delete(p);
  int untouched=reads+writes;
  p=wire("{\"topic\":\"audio/volume\",\"payload\":{\"operation\":\"set\",\"volume\":35,\"requestId\":\"missing-board\"}}",false);cJSON_Delete(p);
  p=wire("{\"topic\":\"audio/volume\",\"topic\":\"audio/volume\",\"payload\":{\"operation\":\"query\",\"requestId\":\"duplicate-root\",\"expectedBoardDeviceId\":\"board-1\"}}",false);cJSON_Delete(p);
  assert(reads+writes==untouched);
  char id_input[512],long_id[97];memset(long_id,'x',96);long_id[96]=0;
  snprintf(id_input,sizeof(id_input),"{\"topic\":\"audio/volume\",\"payload\":{\"operation\":\"query\",\"requestId\":\"%s\",\"expectedBoardDeviceId\":\"board-1\"}}",long_id);
  p=wire(id_input,false);assert(!cJSON_GetObjectItemCaseSensitive(p,"requestId")->valuestring[0]);cJSON_Delete(p);
  assert(reads+writes==untouched);
  p=wire("{\"topic\":\"audio/volume\",\"payload\":{\"operation\":\"set\",\"volume\":35.0,\"requestId\":\"set-35\",\"expectedBoardDeviceId\":\"board-1\"}}",true);
  assert(cJSON_GetObjectItemCaseSensitive(p,"volume")->valueint==35 && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"persisted")));cJSON_Delete(p);
  int saved=nvs_writes;pet_audio_volume_idle_commit(999,true,idle);assert(nvs_writes==saved);
  audio_idle=false;pet_audio_volume_idle_commit(1000,true,idle);audio_idle=true;assert(nvs_writes==saved);
  pet_resource_realtime_t rt={0};assert(pet_resources_realtime_begin(&broker,&rt)==PET_RESOURCE_OK);
  pet_audio_volume_idle_commit(1000,true,idle);assert(nvs_writes==saved);pet_resources_realtime_finish(&broker,rt);
  pet_audio_volume_idle_commit(1000,true,idle);assert(nvs_writes==saved+1 && stored_value==35);no_resources();
  assert(pet_audio_volume_get(&s)==0 && s.persisted && !s.save_pending);
  int same_writes=writes;assert(pet_audio_volume_set(35,1050,&s)==0 && writes==same_writes && !s.save_pending);
  assert(pet_audio_volume_set(0,1100,&s)==0 && s.volume==0 && (regs[0x31]&0x60)==0x60);
  assert(pet_audio_volume_set(1,1200,&s)==0 && !(regs[0x31]&0x60));
  assert(pet_audio_volume_set(45,1300,&s)==0);
  pet_audio_volume_idle_commit(10999,true,idle);assert(nvs_writes==saved+1);
  pet_audio_volume_idle_commit(11000,true,idle);assert(nvs_writes==saved+2 && stored_value==45);
  no_resources();
  /* Query must observe hardware rather than reload the saved value. */
  assert(pet_audio_volume_set(60,12000,&s)==0);
  int before_writes=nvs_writes,before_reads=nvs_reads,before_codec=writes;
  p=wire("{\"topic\":\"audio/volume\",\"payload\":{\"operation\":\"query\",\"requestId\":\"q\",\"expectedBoardDeviceId\":\"board-1\"}}",true);
  assert(cJSON_GetObjectItemCaseSensitive(p,"volume")->valueint==60);
  assert(nvs_writes==before_writes && nvs_reads==before_reads && writes==before_codec);cJSON_Delete(p);
  /* Actual vendor reopen at 48/16 kHz; query still checks real registers. */
  esp_codec_dev_sample_info_t fmt={.sample_rate=48000,.channel=1,.bits_per_sample=16};
  assert(esp_codec_dev_close(dev)==0 && esp_codec_dev_open(dev,&fmt)==0);
  assert(pet_audio_volume_get(&s)==0 && s.volume==60);
  fmt.sample_rate=16000;assert(esp_codec_dev_close(dev)==0 && esp_codec_dev_open(dev,&fmt)==0);
  assert(pet_audio_volume_get(&s)==0 && s.volume==60);
  fail_write=0x32;assert(pet_audio_volume_set(70,13000,&s)!=0 && !s.confirmed && !pet_audio_volume_pending());fail_write=-1;
  assert(pet_audio_volume_get(&s)==0 && s.volume==60); // SDK cache is not the observation.
  ignore_write=0x32;assert(pet_audio_volume_set(80,14000,&s)!=0 && s.volume==-1);ignore_write=-1;
  assert(pet_audio_volume_get(&s)==0 && s.volume==60);
  lock_busy=1;assert(pet_audio_volume_set(80,15000,&s)==ESP_ERR_TIMEOUT && !s.confirmed);lock_busy=0;
  /* Fail mute after level write: failed/null result, no compensating replay. */
  fail_write=0x31;int partial_writes=writes;
  p=wire("{\"topic\":\"audio/volume\",\"payload\":{\"operation\":\"set\",\"volume\":65,\"requestId\":\"partial\",\"expectedBoardDeviceId\":\"board-1\"}}",false);
  assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(p,"volume")) && writes==partial_writes+2);
  assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"persisted")));cJSON_Delete(p);fail_write=-1;
  assert(pet_audio_volume_get(&s)==0 && s.volume==65 && !s.save_pending);
  assert(pet_audio_volume_set(70,16000,&s)==0);
  nvs_fail_commit=ESP_FAIL;
  pet_audio_volume_idle_commit(22000,true,idle);assert(pet_audio_volume_pending());no_resources();
  assert(pet_audio_volume_get(&s)==0 && !s.persisted && s.save_error);
  saved=nvs_writes;nvs_fail_commit=0;
  pet_audio_volume_idle_commit(51999,true,idle);assert(nvs_writes==saved);
  pet_audio_volume_idle_commit(52000,true,idle);assert(nvs_writes==saved+1);no_resources();
  assert(pet_audio_volume_get(&s)==0 && s.persisted);
  assert(pet_audio_volume_set(0,53000,&s)==0);
  revoke_on_set=true;pet_audio_volume_idle_commit(62000,true,idle);revoke_on_set=false;
  assert(pet_audio_volume_pending() && !nvs_opened);no_resources();
  pet_resources_realtime_finish(&broker,raced_audio);memset(&raced_audio,0,sizeof(raced_audio));
  pet_audio_volume_idle_commit(92000,true,idle);assert(stored_value==0);no_resources();
  assert(pet_audio_volume_get(&s)==0 && s.persisted && s.volume==0);
  /* NVS open/set failures and queue loss cannot leak a physical lease. */
  assert(pet_audio_volume_set(20,93000,&s)==0);
  queue_on_open=true;saved=nvs_writes;
  pet_audio_volume_idle_commit(102000,true,idle);queue_on_open=false;audio_idle=true;
  assert(nvs_writes==saved && pet_audio_volume_pending());no_resources();
  nvs_fail_open=ESP_FAIL;pet_audio_volume_idle_commit(112000,true,idle);nvs_fail_open=0;no_resources();
  assert(pet_audio_volume_get(&s)==0 && s.save_error && s.save_pending);
  nvs_fail_set=ESP_FAIL;pet_audio_volume_idle_commit(142000,true,idle);nvs_fail_set=0;no_resources();
  assert(pet_audio_volume_get(&s)==0 && !s.persisted && s.save_pending);
  pet_audio_volume_idle_commit(172000,true,idle);no_resources();
  assert(pet_audio_volume_get(&s)==0 && s.persisted && s.volume==20);
  regs[0x32]=255;assert(pet_audio_volume_get(&s)!=0 && !s.confirmed && s.volume==-1);
  puts("master actual-readback/protocol/NVS/admission/cleanup tests passed");
}

int main(int argc,char **argv){
  audio_codec_i2c_cfg_t bus={.addr=0x30,.bus_handle=(void *)1};
  const audio_codec_ctrl_if_t *ctrl=audio_codec_new_i2c_ctrl(&bus);assert(ctrl);
  es8311_codec_cfg_t cfg={.ctrl_if=ctrl,.codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC,.pa_pin=-1,
    .hw_gain={.pa_voltage=5.0,.codec_dac_voltage=3.3}};
  const audio_codec_if_t *codec=es8311_codec_new(&cfg);assert(codec);
  audio_codec_data_if_t data={.is_open=data_open,.write=data_write};
  esp_codec_dev_cfg_t dc={.dev_type=ESP_CODEC_DEV_TYPE_OUT,.codec_if=codec,.data_if=&data};
  esp_codec_dev_handle_t dev=esp_codec_dev_new(&dc);assert(dev);
  int volume;bool muted;
  assert(esp_codec_dev_set_out_vol(dev,35)==0);
  assert(esp_codec_dev_get_out_vol(dev,&volume)==0 && volume==35);
  fail_write=0x32;
  assert(esp_codec_dev_set_out_vol(dev,70)==ESP_CODEC_DEV_WRITE_FAIL);
  assert(esp_codec_dev_get_out_vol(dev,&volume)==0 && volume==35);
  fail_write=-1;assert(esp_codec_dev_set_out_mute(dev,false)==0);
  fail_read=0x31;int before=writes;
  assert(esp_codec_dev_set_out_mute(dev,true)==ESP_CODEC_DEV_READ_FAIL && writes==before);
  assert(esp_codec_dev_get_out_mute(dev,&muted)==0 && !muted);
  fail_read=-1;fail_write=0x31;
  assert(esp_codec_dev_set_out_mute(dev,true)==ESP_CODEC_DEV_WRITE_FAIL);
  assert(esp_codec_dev_get_out_mute(dev,&muted)==0 && !muted);
  fail_write=-1;assert(esp_codec_dev_set_out_mute(dev,true)==0);
  assert(esp_codec_dev_get_out_mute(dev,&muted)==0 && muted);
  assert(esp_codec_dev_set_out_mute(dev,false)==0 && last_timeout==100);

  now=0;cost_us=7000;assert(pet_audio_volume_deadline_begin(20000));
  assert(!pet_audio_volume_deadline_begin(20000));
  assert(esp_codec_dev_set_out_mute(dev,true)==0 && now==14000 && last_timeout==13);
  assert(esp_codec_dev_set_out_vol(dev,80)==ESP_CODEC_DEV_WRITE_FAIL && now==20000 && last_timeout==6);
  before=reads+writes;
  assert(esp_codec_dev_set_out_mute(dev,false)==ESP_CODEC_DEV_READ_FAIL && before==reads+writes);
  assert(esp_codec_dev_get_out_vol(dev,&volume)==0 && volume==35);
  task=(void *)2; // Simulated capture task cannot inherit the master's deadline.
  assert(!pet_audio_volume_deadline_active());
  assert(!pet_audio_volume_deadline_begin(20000));
  assert(esp_codec_dev_set_out_vol(dev,40)==0 && last_timeout==100);
  pet_audio_volume_deadline_end(); // A different task cannot clear its owner.
  task=(void *)1;assert(pet_audio_volume_deadline_active());
  pet_audio_volume_deadline_end();assert(esp_codec_dev_set_out_vol(dev,50)==0 && last_timeout==100);
  runtime_tests(dev,argc>1?argv[1]:"default");
  esp_codec_dev_delete(dev);audio_codec_delete_codec_if(codec);audio_codec_delete_ctrl_if(ctrl);
  puts("actual vendor error/cache/mute/deadline/task-isolation tests passed");
}
