/* Deferred, checksummed A/B snapshots. No filesystem work in runtime critical sections. */
#include "pet_p4_app_store.h"
#include "pet_p4_audio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#define SLOTS 32
#ifndef PET_APP_SAVE_ROOT
#define PET_APP_SAVE_ROOT "/spiffs"
#endif
typedef struct {char id[48],json[PET_APP_SAVE_BYTES];uint32_t seq;uint64_t dirty_since;uint8_t file_slot;bool dirty,loaded,deleted;} save_t;
typedef struct {uint32_t magic,seq,size,checksum;char id[48];} header_t;
static EXT_RAM_BSS_ATTR save_t saves[SLOTS];
static SemaphoreHandle_t lock;
static TaskHandle_t worker;
static uint64_t now_ms(void){return (uint64_t)esp_timer_get_time()/1000;}
static uint32_t checksum(const char *p,size_t n){uint32_t h=2166136261u;for(size_t i=0;i<n;i++){h^=(unsigned char)p[i];h*=16777619u;}return h;}
static bool valid_id(const char *id){
  if(!id||!id[0]||strlen(id)>47)return false;
  for(const char *p=id;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'))return false;
  return true;
}
static void path_for(const char *id,unsigned slot,char out[64]){
  unsigned char hash[32];mbedtls_sha256((const unsigned char*)id,strlen(id),hash,0);
  snprintf(out,64,PET_APP_SAVE_ROOT "/as-%02x%02x%02x%02x%02x%02x%02x%02x-%u",hash[0],hash[1],hash[2],hash[3],hash[4],hash[5],hash[6],hash[7],slot);
}
static bool read_slot(const char *id,unsigned slot,save_t *out){
  char path[64];path_for(id,slot,path);FILE *f=fopen(path,"rb");if(!f)return false;
  header_t h={0};bool ok=fread(&h,1,sizeof(h),f)==sizeof(h)&&h.magic==0x41535631&&h.size>0&&h.size<PET_APP_SAVE_BYTES&&memchr(h.id,0,sizeof(h.id))&&!strcmp(h.id,id);
  if(ok){ok=fread(out->json,1,h.size,f)==h.size&&fgetc(f)==EOF;out->json[h.size]=0;ok=ok&&checksum(out->json,h.size)==h.checksum&&!memchr(out->json,0,h.size);}
  fclose(f);if(ok){snprintf(out->id,sizeof(out->id),"%s",id);out->seq=h.seq;out->file_slot=slot;out->loaded=true;}return ok;
}
static bool write_slot(const save_t *save){
  char path[64];path_for(save->id,save->file_slot,path);
  if(save->deleted){bool ok=true;for(unsigned i=0;i<2;i++){path_for(save->id,i,path);if(unlink(path)!=0&&errno!=ENOENT)ok=false;}return ok;}
  header_t h={.magic=0x41535631,.seq=save->seq,.size=(uint32_t)strlen(save->json)};
  memcpy(h.id,save->id,sizeof(h.id));h.checksum=checksum(save->json,h.size);
  FILE *f=fopen(path,"wb");if(!f)return false;
  bool ok=fwrite(&h,1,sizeof(h),f)==sizeof(h)&&fwrite(save->json,1,h.size,f)==h.size&&fflush(f)==0;
  if(ok)ok=fsync(fileno(f))==0;
  if(fclose(f)!=0)ok=false;
  return ok; // prior generation is never overwritten until a new full snapshot exists
}
static int find_slot(const char *id){for(int i=0;i<SLOTS;i++)if(!strcmp(saves[i].id,id))return i;for(int i=0;i<SLOTS;i++)if(!saves[i].id[0])return i;return -1;}
static void flush_one(void){
    save_t pending;
    if(pet_p4_audio_stream_playing()||pet_p4_audio_music_active()||pet_p4_audio_conversation_active()||pet_p4_audio_active())return;
    for(int i=0;i<SLOTS;i++){
      xSemaphoreTake(lock,portMAX_DELAY);
      bool ready=saves[i].dirty&&now_ms()-saves[i].dirty_since>=30000;
      if(ready){pending=saves[i];pending.file_slot^=1;}xSemaphoreGive(lock);
      if(!ready)continue;
      bool ok=write_slot(&pending);
      xSemaphoreTake(lock,portMAX_DELAY);
      if(ok&&!strcmp(saves[i].id,pending.id)){
        saves[i].file_slot=pending.file_slot;saves[i].dirty_since=now_ms();
        if(saves[i].seq==pending.seq){saves[i].dirty=false;if(pending.deleted)memset(&saves[i],0,sizeof(save_t));}
      }
      xSemaphoreGive(lock);
      break; // at most one bounded save per second
    }
}
static void save_worker(void *arg){
  (void)arg;for(;;){vTaskDelay(pdMS_TO_TICKS(1000));flush_one();}
}
bool pet_app_store_init(void){
  if (worker) return true;
  if (!lock) lock = xSemaphoreCreateMutex();
  if (!lock) return false;
  return xTaskCreate(save_worker,"pet_app_save",4096,NULL,1,&worker)==pdPASS;
}
bool pet_app_store_read(const char *id,char out[PET_APP_SAVE_BYTES]){
  if (!lock || !valid_id(id)) return false;
  out[0] = 0;
  xSemaphoreTake(lock,portMAX_DELAY);int i=find_slot(id);
  if(i<0){xSemaphoreGive(lock);return false;}
  if(saves[i].loaded){bool ok=!saves[i].deleted&&saves[i].json[0];if(ok)memcpy(out,saves[i].json,PET_APP_SAVE_BYTES);xSemaphoreGive(lock);return ok;}
  /* Activation serializes loads. Worker only writes loaded slots. */
  save_t a={0},b={0};bool av=read_slot(id,0,&a),bv=read_slot(id,1,&b);
  if(av||bv)saves[i]=bv&&(!av||(int32_t)(b.seq-a.seq)>0)?b:a;
  else{snprintf(saves[i].id,48,"%s",id);saves[i].loaded=true;}
  if (av || bv) memcpy(out, saves[i].json, PET_APP_SAVE_BYTES);
  xSemaphoreGive(lock);
  return av || bv;
}
bool pet_app_store_update(const char *id,const char *json){
  if(!lock||!valid_id(id)||!json||!json[0]||strlen(json)>=PET_APP_SAVE_BYTES)return false;
  xSemaphoreTake(lock,portMAX_DELAY);int i=find_slot(id);bool ok=i>=0;
  if(ok&&strcmp(saves[i].json,json)){
    save_t *s=&saves[i];snprintf(s->id,48,"%s",id);snprintf(s->json,sizeof(s->json),"%s",json);s->seq++;if(!s->seq)s->seq=1;
    if (!s->dirty) s->dirty_since = now_ms();
    s->loaded=true;s->dirty=true;s->deleted=false;
  }xSemaphoreGive(lock);return ok;
}
void pet_app_store_forget(const char *id){
  if (!lock || !valid_id(id)) return;
  xSemaphoreTake(lock,portMAX_DELAY);int i=find_slot(id);
  if(i>=0){save_t *s=&saves[i];snprintf(s->id,48,"%s",id);s->json[0]=0;s->seq++;s->loaded=true;s->dirty=true;s->deleted=true;s->dirty_since=0;}
  xSemaphoreGive(lock);
}
