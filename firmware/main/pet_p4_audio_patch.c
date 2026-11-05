/* Non-destructive SPIFFS-only WAV overlay. Neither video/raw nor pack files
 * are named by this module. Each slot has two bounded override banks. A new
 * bank's checksummed index is written LAST; incomplete staging is never live.
 * No replace-rename assumption: SPIFFS index generations select a valid bank. */
#include "pet_p4_audio_patch.h"
#include "esp_attr.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdatomic.h>
#ifndef PET_AUDIO_PATCH_FS_PREFIX
#define PET_AUDIO_PATCH_FS_PREFIX "/spiffs"
#endif
#ifndef PET_AUDIO_PATCH_SYNC
#define PET_AUDIO_PATCH_SYNC fsync
#endif
/* Compile-time host fault hooks; production remains direct stdio. */
#ifndef PET_AUDIO_PATCH_INDEX_OPEN
#define PET_AUDIO_PATCH_INDEX_OPEN fopen
#endif
#ifndef PET_AUDIO_PATCH_INDEX_READ
#define PET_AUDIO_PATCH_INDEX_READ fread
#endif
#ifndef PET_AUDIO_PATCH_INDEX_ERROR
#define PET_AUDIO_PATCH_INDEX_ERROR ferror
#endif
#ifndef PET_AUDIO_PATCH_INDEX_CLOSE
#define PET_AUDIO_PATCH_INDEX_CLOSE fclose
#endif
#ifndef PET_AUDIO_PATCH_REMOVE
#define PET_AUDIO_PATCH_REMOVE remove
#endif
typedef enum { INDEX_ABSENT, INDEX_INVALID, INDEX_VALID, INDEX_IO_UNKNOWN } index_result_t;
#define ENTRY_BYTES 188U
#define HEADER_BYTES 92U
#define INDEX_BYTES (HEADER_BYTES+PET_AUDIO_PATCH_MAX_FILES*ENTRY_BYTES+8U)
typedef struct {
  char family[48],path[128];
  uint32_t size,received,index;
  uint64_t checksum,received_hash;
} patch_file_t;
typedef struct {
  patch_file_t files[PET_AUDIO_PATCH_MAX_FILES];
  char pack[65];
  int slot,bank;
  uint32_t generation,count,total;
  bool active,published,publication_attempted;
} patch_plan_t;
static EXT_RAM_BSS_ATTR patch_plan_t pending,scratch;
static EXT_RAM_BSS_ATTR uint8_t index_bytes[INDEX_BYTES+1U];
static uint64_t hash_update(uint64_t h,const uint8_t *p,size_t n){for(size_t i=0;i<n;i++){h^=p[i];h*=UINT64_C(1099511628211);}return h;}
static uint64_t hash(const uint8_t *p,size_t n){return hash_update(UINT64_C(14695981039346656037),p,n);}
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t *p){return (uint64_t)le32(p)|(uint64_t)le32(p+4)<<32;}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static void put64(uint8_t *p,uint64_t n){put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static bool hex(const char *p,size_t n){if(!p||strlen(p)!=n)return false;for(size_t i=0;i<n;i++)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')))return false;return true;}
static _Atomic uint32_t global_family_mask;
static uint32_t global_family_bit(const char *family){
  static const char *const names[]={"welcome","working","idle","done","error","waiting_user",
    "permission_request","permission_approved","permission_denied","permission_expired","session_terminated",
    "idle.playing","idle.wandering","idle.default","idle.jumping","working.thinking","working.typing","working.browsing",
    "touch.lick","touch.what","touch.left","touch.right","idle.begging","idle.daydreaming","idle.eating","idle.reading","idle.traveling"};
  _Static_assert(sizeof(names)/sizeof(names[0])<=32,"global cue mask bound");
  if(family)for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)if(!strcmp(family,names[i]))return UINT32_C(1)<<i;
  return 0;
}
bool pet_audio_patch_global_family_known(const char *family){return global_family_bit(family)!=0;}
bool pet_audio_patch_global_enabled(const char *family){return (atomic_load_explicit(&global_family_mask,memory_order_acquire)&global_family_bit(family))!=0;}
static uint32_t global_plan_mask(const patch_plan_t *plan){
  uint32_t mask=0;
  for(unsigned i=0;i<plan->count;i++)mask|=global_family_bit(plan->files[i].family);
  return mask;
}
static bool family_valid(const char *p){size_t n=p?strlen(p):0;if(!n||n>=48)return false;for(size_t i=0;i<n;i++)if(!((p[i]>='a'&&p[i]<='z')||(p[i]>='0'&&p[i]<='9')||p[i]=='_'||p[i]=='-'||p[i]=='.'))return false;return true;}
static bool path_valid(const char *p){size_t n=p?strlen(p):0;return n>7&&n<128&&!strncmp(p,"p4/",3)&&!strcmp(p+n-4,".wav")&&!strstr(p,"..")&&!strchr(p,'\\')&&!strchr(p,':');}
static bool number(const cJSON *o,const char *key,uint32_t max,uint32_t *out){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<0||v->valuedouble>max||floor(v->valuedouble)!=v->valuedouble)return false;*out=(uint32_t)v->valuedouble;return true;}
static const char *string(const cJSON *o,const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
static bool file_path(int slot,int bank,unsigned file,char *out,size_t n){if(slot<0||slot>PET_AUDIO_PATCH_GLOBAL_SLOT||bank<0||bank>1||file>=PET_AUDIO_PATCH_MAX_FILES)return false;int used=snprintf(out,n,PET_AUDIO_PATCH_FS_PREFIX "/ov%d_%d_%u.wav",slot,bank,file);return used>0&&(size_t)used<n;}
static bool index_path(int slot,int bank,char *out,size_t n){int used=snprintf(out,n,PET_AUDIO_PATCH_FS_PREFIX "/ov%d_%d.idx",slot,bank);return used>0&&(size_t)used<n;}
static bool close_written(FILE *f){bool ok=fflush(f)==0;if(ok&&PET_AUDIO_PATCH_SYNC(fileno(f))!=0)ok=false;if(fclose(f)!=0)ok=false;return ok;}
static bool remove_one(const char *p,bool *attempted){*attempted=true;errno=0;return PET_AUDIO_PATCH_REMOVE(p)==0||errno==ENOENT;}
static bool clear_bank(int slot,int bank,bool *attempted){char p[64];if(!index_path(slot,bank,p,sizeof(p)))return false;if(!remove_one(p,attempted))return false;for(unsigned i=0;i<PET_AUDIO_PATCH_MAX_FILES;i++){if(!file_path(slot,bank,i,p,sizeof(p))||!remove_one(p,attempted))return false;}return true;}
static index_result_t read_index(int slot,int bank,patch_plan_t *out){
  char p[64];if(!index_path(slot,bank,p,sizeof(p)))return INDEX_INVALID;
  errno=0;FILE *f=PET_AUDIO_PATCH_INDEX_OPEN(p,"rb");
  if(!f)return errno==ENOENT?INDEX_ABSENT:INDEX_IO_UNKNOWN;
  size_t n=PET_AUDIO_PATCH_INDEX_READ(index_bytes,1,sizeof(index_bytes),f);
  bool io_ok=!PET_AUDIO_PATCH_INDEX_ERROR(f);if(PET_AUDIO_PATCH_INDEX_CLOSE(f)!=0)io_ok=false;
  /* Unreadable is not absent/corrupt. It may be the newest committed bank. */
  if(!io_ok)return INDEX_IO_UNKNOWN;
  if(n<HEADER_BYTES+8||n>INDEX_BYTES||memcmp(index_bytes,"p4audv1",8)||index_bytes[8]!=slot||index_bytes[9]!=bank)return INDEX_INVALID;
  for(unsigned i=10;i<16;i++)if(index_bytes[i])return INDEX_INVALID;
  uint64_t generation=le64(index_bytes+16);uint32_t count=le32(index_bytes+88);
  if(!generation||generation>UINT32_MAX||count>PET_AUDIO_PATCH_MAX_FILES||n!=HEADER_BYTES+count*ENTRY_BYTES+8||hash(index_bytes,n-8)!=le64(index_bytes+n-8))return INDEX_INVALID;
  memset(out,0,sizeof(*out));out->slot=slot;out->bank=bank;out->generation=(uint32_t)generation;out->count=count;
  memcpy(out->pack,index_bytes+24,64);out->pack[64]=0;if(!hex(out->pack,64))return INDEX_INVALID;
  for(unsigned i=0;i<count;i++){
    const uint8_t *row=index_bytes+HEADER_BYTES+i*ENTRY_BYTES;patch_file_t *entry=&out->files[i];
    if(!memchr(row,0,48)||!memchr(row+48,0,128))return INDEX_INVALID;
    memcpy(entry->family,row,48);memcpy(entry->path,row+48,128);entry->size=le32(row+176);entry->checksum=le64(row+180);
    if(!family_valid(entry->family)||!path_valid(entry->path)||entry->size<44||entry->size>PET_AUDIO_PATCH_MAX_FILE||entry->size>PET_AUDIO_PATCH_MAX_BYTES-out->total)return INDEX_INVALID;
    out->total+=entry->size;
    for(unsigned j=0;j<i;j++)if(!strcmp(entry->family,out->files[j].family)||!strcmp(entry->path,out->files[j].path))return INDEX_INVALID;
  }
  return INDEX_VALID;
}
static bool latest(int slot,int *bank,uint32_t *generation){
  bool found=false;uint32_t best=0;int selected=-1;
  for(int b=0;b<2;b++){
    index_result_t result=read_index(slot,b,&scratch);
    /* Do not infer an older bank is current after any IO-unknown result. */
    if(result==INDEX_IO_UNKNOWN)return false;
    if(result!=INDEX_VALID)continue;
    if(found&&scratch.generation==best)return false;
    if(!found||scratch.generation>best){found=true;best=scratch.generation;selected=b;}
  }
  *bank=selected;*generation=best;return true;
}
void pet_audio_patch_global_refresh(void){
  int bank;uint32_t generation,mask=0;
  if(latest(PET_AUDIO_PATCH_GLOBAL_SLOT,&bank,&generation)&&bank>=0
      &&read_index(PET_AUDIO_PATCH_GLOBAL_SLOT,bank,&scratch)==INDEX_VALID
      &&!strcmp(scratch.pack,PET_AUDIO_PATCH_GLOBAL_PACK))mask=global_plan_mask(&scratch);
  atomic_store_explicit(&global_family_mask,mask,memory_order_release);
}
bool pet_audio_patch_begin(int slot,const char *pack,const cJSON *files,uint32_t total,bool *attempted){
  if(pending.active||slot<0||slot>PET_AUDIO_PATCH_GLOBAL_SLOT||!hex(pack,64)||!cJSON_IsArray(files)||total>PET_AUDIO_PATCH_MAX_BYTES)return false;
  int count=cJSON_GetArraySize(files);if(count<0||count>(int)PET_AUDIO_PATCH_MAX_FILES)return false;
  memset(&pending,0,sizeof(pending));pending.slot=slot;pending.count=(uint32_t)count;memcpy(pending.pack,pack,65);
  for(unsigned i=0;i<pending.count;i++){
    const cJSON *item=cJSON_GetArrayItem(files,(int)i);patch_file_t *entry=&pending.files[i];
    const char *family=string(item,"family"),*path=string(item,"path"),*checksum=string(item,"checksum");
    if(!family_valid(family)||!path_valid(path)||!hex(checksum,16)||!number(item,"size",PET_AUDIO_PATCH_MAX_FILE,&entry->size)||entry->size<44||entry->size>PET_AUDIO_PATCH_MAX_BYTES-pending.total)return false;
    for(unsigned j=0;j<i;j++)if(!strcmp(family,pending.files[j].family)||!strcmp(path,pending.files[j].path))return false;
    memcpy(entry->family,family,strlen(family)+1);memcpy(entry->path,path,strlen(path)+1);
    for(unsigned j=0;j<16;j++)entry->checksum=(entry->checksum<<4)|(uint64_t)(checksum[j]<='9'?checksum[j]-'0':checksum[j]-'a'+10);
    entry->received_hash=UINT64_C(14695981039346656037);pending.total+=entry->size;
  }
  if(pending.total!=total)return false;
  int old;uint32_t generation;if(!latest(slot,&old,&generation)||generation==UINT32_MAX)return false;
  pending.bank=old<0?0:1-old;pending.generation=generation+1;
  /* Mark this exact private bank before IO so a failed phase retains its identity. */
  pending.active=true;
  return clear_bank(slot,pending.bank,attempted);
}
static patch_file_t *find_file(const char *path,unsigned *index){for(unsigned i=0;i<pending.count;i++)if(!strcmp(pending.files[i].path,path)){if(index)*index=i;return &pending.files[i];}return NULL;}
bool pet_audio_patch_chunk(const char *path,uint32_t index,uint32_t offset,const uint8_t *bytes,size_t n,bool *attempted){
  unsigned at=0;patch_file_t *entry=find_file(path,&at);
  if(!pending.active||pending.publication_attempted||!entry||!bytes||!n||n>PET_AUDIO_PATCH_CHUNK_BYTES||index!=entry->index||offset!=entry->received||n>entry->size-entry->received)return false;
  char p[64];if(!file_path(pending.slot,pending.bank,at,p,sizeof(p)))return false;
  *attempted=true;FILE *f=fopen(p,offset?"ab":"wb");if(!f)return false;
  bool ok=fseek(f,0,SEEK_END)==0&&ftell(f)==(long)offset;
  if(ok)ok=fwrite(bytes,1,n,f)==n;
  if(!close_written(f))ok=false;
  if(!ok)return false;
  f=fopen(p,"rb");if(!f)return false;
  uint8_t check[512];size_t read=0;ok=fseek(f,(long)offset,SEEK_SET)==0;
  while(ok&&read<n){size_t take=n-read<sizeof(check)?n-read:sizeof(check);ok=fread(check,1,take,f)==take&&!memcmp(check,bytes+read,take);read+=take;}
  if(ok)ok=fgetc(f)==EOF&&!ferror(f);
  if(fclose(f)!=0)ok=false;
  if(!ok)return false;
  entry->received_hash=hash_update(entry->received_hash,bytes,n);entry->received+=(uint32_t)n;entry->index++;return true;
}
static bool wav_exact(const char *path,const patch_file_t *entry){
  FILE *f=fopen(path,"rb");if(!f)return false;
  uint8_t block[512];uint64_t checksum=UINT64_C(14695981039346656037);uint32_t total=0;size_t n;
  while((n=fread(block,1,sizeof(block),f))){if(n>entry->size-total){fclose(f);return false;}checksum=hash_update(checksum,block,n);total+=(uint32_t)n;}
  bool ok=!ferror(f)&&total==entry->size&&checksum==entry->checksum&&fseek(f,0,SEEK_SET)==0;
  uint8_t riff[12];if(ok)ok=fread(riff,1,12,f)==12&&!memcmp(riff,"RIFF",4)&&!memcmp(riff+8,"WAVE",4)&&le32(riff+4)==entry->size-8;
  bool format=false,data=false;uint32_t offset=12;
  while(ok&&offset+8<=entry->size){
    uint8_t head[8];if(fread(head,1,8,f)!=8){ok=false;break;}uint32_t size=le32(head+4);offset+=8;
    if(size>entry->size-offset){ok=false;break;}
    if(!memcmp(head,"fmt ",4)){
      uint8_t fmt[16];if(size<16||fread(fmt,1,16,f)!=16){ok=false;break;}
      format=fmt[0]==1&&!fmt[1]&&fmt[2]==1&&!fmt[3]&&le32(fmt+4)==16000&&le32(fmt+8)==32000&&fmt[12]==2&&!fmt[13]&&fmt[14]==16&&!fmt[15];
    }else if(!memcmp(head,"data",4)){if(!format||!size||(size&1)||size>1024U*1024U){ok=false;break;}data=true;}
    uint32_t next=offset+size+(size&1U);if(next<offset||next>entry->size){ok=false;break;}offset=next;ok=fseek(f,(long)offset,SEEK_SET)==0;
  }
  ok=ok&&format&&data&&offset==entry->size;if(fclose(f)!=0)ok=false;return ok;
}
bool pet_audio_patch_commit(bool *attempted,uint32_t *generation){
  if(!pending.active||pending.publication_attempted)return false;
  char p[64];
  for(unsigned i=0;i<pending.count;i++){
    patch_file_t *entry=&pending.files[i];
    if(entry->received!=entry->size||entry->received_hash!=entry->checksum||!file_path(pending.slot,pending.bank,i,p,sizeof(p))||!wav_exact(p,entry))return false;
  }
  size_t n=HEADER_BYTES+pending.count*ENTRY_BYTES+8;memset(index_bytes,0,n);memcpy(index_bytes,"p4audv1",8);
  index_bytes[8]=(uint8_t)pending.slot;index_bytes[9]=(uint8_t)pending.bank;put64(index_bytes+16,pending.generation);memcpy(index_bytes+24,pending.pack,64);put32(index_bytes+88,pending.count);
  for(unsigned i=0;i<pending.count;i++){uint8_t *row=index_bytes+HEADER_BYTES+i*ENTRY_BYTES;patch_file_t *entry=&pending.files[i];memcpy(row,entry->family,strlen(entry->family)+1);memcpy(row+48,entry->path,strlen(entry->path)+1);put32(row+176,entry->size);put64(row+180,entry->checksum);}
  uint64_t expected_hash=hash(index_bytes,n-8);put64(index_bytes+n-8,expected_hash);if(!index_path(pending.slot,pending.bank,p,sizeof(p)))return false;
  pending.publication_attempted=true;
  *attempted=true;FILE *f=fopen(p,"wb");if(!f)return false;bool ok=fwrite(index_bytes,1,n,f)==n;if(!close_written(f))ok=false;
  if(!ok||read_index(pending.slot,pending.bank,&scratch)!=INDEX_VALID||scratch.generation!=pending.generation||strcmp(scratch.pack,pending.pack)||hash(index_bytes,n-8)!=expected_hash)return false;
  pending.published=true;*generation=pending.generation;
  if(pending.slot==PET_AUDIO_PATCH_GLOBAL_SLOT&&!strcmp(pending.pack,PET_AUDIO_PATCH_GLOBAL_PACK))
    atomic_store_explicit(&global_family_mask,global_plan_mask(&pending),memory_order_release);
  return true;
}
bool pet_audio_patch_abort(bool *attempted){if(!pending.active||pending.publication_attempted)return false;return clear_bank(pending.slot,pending.bank,attempted);}
void pet_audio_patch_retire(void){memset(&pending,0,sizeof(pending));}
bool pet_audio_patch_progress(const char *path,uint32_t *index,uint32_t *bytes){patch_file_t *entry=find_file(path,NULL);if(!pending.active||!entry)return false;*index=entry->index;*bytes=entry->received;return true;}
unsigned pet_audio_patch_file_count(void){return pending.count;}
uint32_t pet_audio_patch_total_bytes(void){return pending.total;}
bool pet_audio_patch_resolve(int slot,const char *pack,const char *family,char *out,size_t n){
  if(slot<0||slot>PET_AUDIO_PATCH_GLOBAL_SLOT||!hex(pack,64)||!family_valid(family)||!out||n<32)return false;
  int bank;uint32_t generation;if(!latest(slot,&bank,&generation)||bank<0||read_index(slot,bank,&scratch)!=INDEX_VALID||strcmp(scratch.pack,pack))return false;
  for(unsigned i=0;i<scratch.count;i++)if(!strcmp(scratch.files[i].family,family))return file_path(slot,bank,i,out,n);
  return false;
}
