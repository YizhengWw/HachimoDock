#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "pet_p4_audio_patch.h"
static uint64_t checksum(const uint8_t *p,size_t n){uint64_t h=UINT64_C(14695981039346656037);while(n--){h^=*p++;h*=UINT64_C(1099511628211);}return h;}
static bool fail_sync;
int patch_test_fsync(int fd){if(fail_sync){fail_sync=false;errno=EIO;return -1;}return fsync(fd);}
static int index_fault;static FILE *fault_stream;static bool read_failed;static unsigned removes;
static const char *fault_path="spiffs/ov0_0.idx";
FILE *patch_index_open(const char *path,const char *mode){
 if(index_fault==1&&!strcmp(path,fault_path)){errno=EIO;return NULL;}
 FILE *f=fopen(path,mode);if(f&&index_fault&&!strcmp(path,fault_path))fault_stream=f;return f;
}
size_t patch_index_read(void *out,size_t size,size_t count,FILE *f){if(index_fault==2&&f==fault_stream){read_failed=true;errno=EIO;return 0;}return fread(out,size,count,f);}
int patch_index_error(FILE *f){return read_failed&&f==fault_stream?1:ferror(f);}
int patch_index_close(FILE *f){bool fail=index_fault==3&&f==fault_stream;int result=fclose(f);if(f==fault_stream){fault_stream=NULL;read_failed=false;}if(fail){errno=EIO;return EOF;}return result;}
int patch_remove(const char *p){removes++;return remove(p);}
static const char pack0[]="0000000000000000000000000000000000000000000000000000000000000000";
static const char pack1[]="1111111111111111111111111111111111111111111111111111111111111111";
static uint8_t wave[46]={'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',2,0,0,0,1,0};
static cJSON *files(void){char hash[17];snprintf(hash,sizeof(hash),"%016llx",(unsigned long long)checksum(wave,sizeof(wave)));cJSON *a=cJSON_CreateArray(),*f=cJSON_CreateObject();assert(a&&f);cJSON_AddStringToObject(f,"family","done");cJSON_AddStringToObject(f,"path","p4/audio/done.wav");cJSON_AddNumberToObject(f,"size",sizeof(wave));cJSON_AddStringToObject(f,"checksum",hash);cJSON_AddItemToArray(a,f);return a;}
static void read_exact(const char *path,uint8_t *out){FILE *f=fopen(path,"rb");assert(f);assert(fread(out,1,sizeof(wave),f)==sizeof(wave));assert(fgetc(f)==EOF);assert(fclose(f)==0);}

static const char *originals[]={"spiffs/s0_ready","spiffs/s1_ready","spiffs/s0_manifest","spiffs/s1_custom_video","spiffs/default29.json","spiffs/raw-partition.bin","spiffs/s0_original.wav","spiffs/s1_original.wav"};
static void original_files(bool create){for(unsigned i=0;i<sizeof(originals)/sizeof(originals[0]);i++){if(create){FILE *f=fopen(originals[i],"wb");assert(f);assert(fwrite("ORIGINAL-KEEP",1,13,f)==13);assert(!fclose(f));}else{char b[14]={0};FILE *f=fopen(originals[i],"rb");assert(f);assert(fread(b,1,14,f)==13&&!memcmp(b,"ORIGINAL-KEEP",13));assert(!fclose(f));}}}
int main(void){
 bool attempted=false;cJSON *a=files();uint32_t generation=0;char path[64],old[64];uint8_t original[46];
 original_files(true);
 assert(pet_audio_patch_begin(0,pack0,a,sizeof(wave),&attempted));assert(attempted);
 attempted=false;assert(!pet_audio_patch_chunk("p4/audio/done.wav",1,0,wave,sizeof(wave),&attempted)&&!attempted);
 attempted=false;assert(!pet_audio_patch_chunk("p4/audio/done.wav",0,2,wave,sizeof(wave),&attempted)&&!attempted);
 assert(pet_audio_patch_chunk("p4/audio/done.wav",0,0,wave,sizeof(wave),&attempted));
 assert(pet_audio_patch_commit(&attempted,&generation)&&generation==1);pet_audio_patch_retire();
 assert(pet_audio_patch_resolve(0,pack0,"done",path,sizeof(path)));read_exact(path,original);assert(!memcmp(original,wave,sizeof(wave)));strcpy(old,path);
 assert(!pet_audio_patch_resolve(0,pack1,"done",path,sizeof(path)));original_files(false);
 puts("PASS builtin overlay persists; exact counters and wrong pack rejected; originals intact");
 cJSON *bad_size=files();cJSON_ReplaceItemInObject(cJSON_GetArrayItem(bad_size,0),"size",cJSON_CreateNumber(43));
 attempted=false;removes=0;assert(!pet_audio_patch_begin(0,pack0,bad_size,43,&attempted)&&!attempted&&removes==0);cJSON_Delete(bad_size);
 attempted=false;removes=0;assert(!pet_audio_patch_begin(0,pack0,a,sizeof(wave)+1,&attempted)&&!attempted&&removes==0);
 puts("PASS invalid declared size/snapshot total fail before deletion");
 for(int failure=1;failure<=3;failure++){
  index_fault=failure;attempted=false;removes=0;
  assert(!pet_audio_patch_begin(0,pack0,a,sizeof(wave),&attempted));
  assert(!attempted&&removes==0);index_fault=0;fault_stream=NULL;read_failed=false;
  assert(pet_audio_patch_resolve(0,pack0,"done",path,sizeof(path))&&!strcmp(path,old));
  read_exact(path,original);assert(!memcmp(original,wave,sizeof(wave)));original_files(false);
 }
 puts("PASS current-bank open/read/close IO_UNKNOWN each rejects begin with ZERO remove calls");
 assert(pet_audio_patch_begin(0,pack0,a,sizeof(wave),&attempted));assert(pet_audio_patch_chunk("p4/audio/done.wav",0,0,wave,20,&attempted));
 attempted=false;assert(!pet_audio_patch_commit(&attempted,&generation)&&!attempted);assert(pet_audio_patch_abort(&attempted));pet_audio_patch_retire();
 assert(pet_audio_patch_resolve(0,pack0,"done",path,sizeof(path))&&!strcmp(old,path));read_exact(path,original);assert(!memcmp(original,wave,sizeof(wave)));original_files(false);
 puts("PASS partial staging and known abort retain active committed bank");
 assert(pet_audio_patch_begin(0,pack0,a,sizeof(wave),&attempted));wave[45]=1;assert(pet_audio_patch_chunk("p4/audio/done.wav",0,0,wave,sizeof(wave),&attempted));wave[45]=0;
 attempted=false;assert(!pet_audio_patch_commit(&attempted,&generation)&&!attempted);assert(pet_audio_patch_abort(&attempted));pet_audio_patch_retire();original_files(false);
 puts("PASS checksum mismatch cannot publish or affect original files");
 cJSON_Delete(a);wave[24]=0;a=files();assert(pet_audio_patch_begin(0,pack0,a,sizeof(wave),&attempted));assert(pet_audio_patch_chunk("p4/audio/done.wav",0,0,wave,sizeof(wave),&attempted));
 attempted=false;assert(!pet_audio_patch_commit(&attempted,&generation)&&!attempted);assert(pet_audio_patch_abort(&attempted));pet_audio_patch_retire();cJSON_Delete(a);wave[24]=0x80;a=files();original_files(false);
 puts("PASS checksum-correct invalid WAV profile cannot publish");
 assert(pet_audio_patch_begin(0,pack0,a,sizeof(wave),&attempted));assert(pet_audio_patch_chunk("p4/audio/done.wav",0,0,wave,20,&attempted));pet_audio_patch_retire();
 assert(pet_audio_patch_resolve(0,pack0,"done",path,sizeof(path))&&!strcmp(old,path));original_files(false);
 puts("PASS cold restart after incomplete staging selects old valid bank");
 cJSON_Delete(a);wave[44]=2;a=files();assert(pet_audio_patch_begin(0,pack0,a,sizeof(wave),&attempted));assert(pet_audio_patch_chunk("p4/audio/done.wav",0,0,wave,sizeof(wave),&attempted));
 fail_sync=true;attempted=false;assert(!pet_audio_patch_commit(&attempted,&generation)&&attempted);
 attempted=false;assert(!pet_audio_patch_abort(&attempted)&&!attempted);assert(!pet_audio_patch_chunk("p4/audio/done.wav",1,sizeof(wave),wave,1,&attempted)&&!attempted);
 read_exact(old,original);assert(original[44]==1);original_files(false);
 // A read-only cold observation may see a durable index despite unknown fsync.
 // This is not an ACK or replay: the attempted bank must never be erased.
 pet_audio_patch_retire();assert(pet_audio_patch_resolve(0,pack0,"done",path,sizeof(path)));read_exact(path,original);assert(original[44]==2);
 puts("PASS unknown publication retains both banks; abort and chunk cannot clear/rewrite");
 FILE *f=fopen("spiffs/ov0_1.idx","r+b");assert(f);assert(!fseek(f,-1,SEEK_END));int ch=fgetc(f);assert(ch!=EOF);assert(!fseek(f,-1,SEEK_END));assert(fputc(ch^1,f)!=EOF);assert(!fclose(f));
 assert(pet_audio_patch_resolve(0,pack0,"done",path,sizeof(path))&&!strcmp(old,path));read_exact(path,original);assert(original[44]==1);original_files(false);
 puts("PASS corrupted newer index falls back to prior fully valid bank");
 assert(pet_audio_patch_begin(1,pack1,a,sizeof(wave),&attempted));assert(pet_audio_patch_chunk("p4/audio/done.wav",0,0,wave,sizeof(wave),&attempted));assert(pet_audio_patch_commit(&attempted,&generation));pet_audio_patch_retire();
 assert(pet_audio_patch_resolve(0,pack0,"done",path,sizeof(path)));assert(pet_audio_patch_resolve(1,pack1,"done",path,sizeof(path)));original_files(false);
 puts("PASS custom slot independent of builtin and default29/raw/video/WAV originals");

 // An explicit empty snapshot removes only overrides and persists across restart.
 cJSON *empty=cJSON_CreateArray();assert(empty);
 for(int slot=0;slot<2;slot++){
  const char *identity=slot?pack1:pack0;
  assert(pet_audio_patch_begin(slot,identity,empty,0,&attempted));
  assert(pet_audio_patch_commit(&attempted,&generation));pet_audio_patch_retire();
  assert(!pet_audio_patch_resolve(slot,identity,"done",path,sizeof(path)));original_files(false);
 }
 cJSON_Delete(empty);puts("PASS builtin/custom empty remove snapshots persist; original WAV/video/raw/default29 unchanged");

 /* Global audio has a separate durable namespace, independent of both video packs. */
 pet_audio_patch_retire();
 cJSON *global_files=files();
 assert(pet_audio_patch_begin(PET_AUDIO_PATCH_GLOBAL_SLOT,PET_AUDIO_PATCH_GLOBAL_PACK,global_files,sizeof(wave),&attempted));
 assert(pet_audio_patch_chunk("p4/audio/done.wav",0,0,wave,sizeof(wave),&attempted));
 assert(pet_audio_patch_commit(&attempted,&generation));pet_audio_patch_retire();
 assert(pet_audio_patch_resolve(PET_AUDIO_PATCH_GLOBAL_SLOT,PET_AUDIO_PATCH_GLOBAL_PACK,"done",path,sizeof(path)));
 assert(pet_audio_patch_global_enabled("done"));
 pet_audio_patch_global_refresh();assert(pet_audio_patch_global_enabled("done"));
 assert(!pet_audio_patch_global_enabled("working"));
 assert(strstr(path,"ov2_")!=NULL);read_exact(path,original);assert(!memcmp(original,wave,sizeof(wave)));
 assert(!pet_audio_patch_resolve(PET_AUDIO_PATCH_GLOBAL_SLOT,pack1,"done",path,sizeof(path)));
 cJSON *empty_global=cJSON_CreateArray();
 assert(pet_audio_patch_begin(PET_AUDIO_PATCH_GLOBAL_SLOT,PET_AUDIO_PATCH_GLOBAL_PACK,empty_global,0,&attempted));
 assert(pet_audio_patch_commit(&attempted,&generation));pet_audio_patch_retire();
 assert(!pet_audio_patch_resolve(PET_AUDIO_PATCH_GLOBAL_SLOT,PET_AUDIO_PATCH_GLOBAL_PACK,"done",path,sizeof(path)));
 assert(!pet_audio_patch_global_enabled("done"));
 pet_audio_patch_global_refresh();assert(!pet_audio_patch_global_enabled("done"));
 original_files(false);cJSON_Delete(global_files);cJSON_Delete(empty_global);
 puts("PASS global cue storage and empty restore independent of both video slots");
 cJSON_Delete(a);return 0;
}
