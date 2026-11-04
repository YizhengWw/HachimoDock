#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_program_persistence.h"
static pet_program_snapshot_t sample,decoded,old;
static uint8_t wire[PET_PROGRAM_STORE_MAX_BYTES+1],saved[PET_PROGRAM_STORE_MAX_BYTES+1];
static char text[PET_PROGRAM_STORE_MAX_TEXT_CAPACITY+1],durable[PET_APP_SAVE_BYTES];
static pet_app_store_status_t status=PET_APP_STORE_VALUE;
static pet_app_checkpoint_status_t checkpoint=PET_APP_CHECKPOINT_DURABLE;
static bool update=true,live=true;
void persistence_alloc_fail(bool);
unsigned persistence_live_alloc(void);
static unsigned writes;
/* The codec adapter borrows the existing atomic store; never fragments a model
 * into independently published keys. FS durability remains the real worker's job. */
pet_app_store_status_t pet_app_store_load_scoped(uint32_t domain,const char *id,char out[PET_APP_SAVE_BYTES]){assert(domain==1&&!strcmp(id,"probe"));if(status==PET_APP_STORE_VALUE)memcpy(out,durable,sizeof durable);return status;}
pet_app_store_status_t pet_app_store_read_scoped_cached(uint32_t d,const char *id,char out[PET_APP_SAVE_BYTES]){return pet_app_store_load_scoped(d,id,out);}
pet_app_store_status_t pet_app_store_load_scoped_with_lease(uint32_t d,const char *id,char out[PET_APP_SAVE_BYTES],pet_resource_lease_t lease){(void)lease;return pet_app_store_load_scoped(d,id,out);}
bool pet_app_store_update_scoped(uint32_t d,const char *id,const char *json){assert(d==1&&!strcmp(id,"probe"));writes++;if(!update)return false;assert(strlen(json)<sizeof durable);strcpy(durable,json);return true;}
pet_app_checkpoint_status_t pet_app_store_checkpoint_scoped(uint32_t d,const char *id,const char *json){assert(d==1&&!strcmp(id,"probe")&&!strcmp(json,durable));return checkpoint;}
static bool current(void *c,uint64_t generation){(void)c;return live&&generation==1;}
static void fixture(unsigned cells){memset(&sample,0,sizeof sample);sample.schema=17;sample.cells=cells;sample.revision=UINT32_MAX-1;for(unsigned i=0;i<cells;i++)sample.values[i]=i%2?INT32_MIN+(int32_t)i:INT32_MAX-(int32_t)i;for(unsigned i=0;i<8;i++)sample.outputs[i]=(int32_t)i-4;}
static void same(void){assert(!memcmp(&sample,&decoded,sizeof sample));}
static void rejection(void){assert(!memcmp(&old,&decoded,sizeof old));}
static void crc_fix(uint8_t *b,size_t n){uint32_t crc=UINT32_MAX;for(size_t i=0;i<n;i++){if(i>=28&&i<32)continue;crc^=b[i];for(int j=0;j<8;j++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}crc=~crc;for(unsigned i=0;i<4;i++)b[28+i]=(uint8_t)(crc>>(8*i));}
static void roundtrips(void){
 for(unsigned cells=0;cells<=256;cells++){
  fixture(cells);size_t n=pet_program_store_size(&sample);assert(n==(cells>64?1088U:320U));
  memset(wire,0xa5,sizeof wire);assert(pet_program_store_encode(wire,n,&sample)==PET_PROGRAM_STORE_OK);assert(wire[n]==0xa5);
  assert(wire[3]==(cells>64?'2':'1')&&wire[4]==(cells>64?2:1));
  assert(pet_program_store_decode(&decoded,17,cells,wire,n)==PET_PROGRAM_STORE_OK);same();
  assert(pet_program_store_encode_text(text,sizeof text,&sample)==PET_PROGRAM_STORE_OK);assert(strlen(text)==n*2+11);
  assert(pet_program_store_decode_text(&decoded,17,cells,text,strlen(text))==PET_PROGRAM_STORE_OK);same();
 }
}
static void rejects(void){
 fixture(255);size_t n=pet_program_store_size(&sample);assert(pet_program_store_encode(wire,n,&sample)==PET_PROGRAM_STORE_OK);memcpy(saved,wire,sizeof wire);
 memset(&decoded,0xa5,sizeof decoded);old=decoded;
 for(size_t i=0;i<n;i++){wire[i]^=1;assert(pet_program_store_decode(&decoded,17,255,wire,n)!=PET_PROGRAM_STORE_OK);rejection();wire[i]^=1;}
 for(size_t i=0;i<n;i++){assert(pet_program_store_decode(&decoded,17,255,wire,i)!=PET_PROGRAM_STORE_OK);rejection();}
 assert(pet_program_store_decode(&decoded,18,255,wire,n)==PET_PROGRAM_STORE_LAYOUT_MISMATCH);rejection();
 assert(pet_program_store_decode(&decoded,17,256,wire,n)==PET_PROGRAM_STORE_LAYOUT_MISMATCH);rejection();
 wire[32+255*4]=1;crc_fix(wire,n);assert(pet_program_store_decode(&decoded,17,255,wire,n)==PET_PROGRAM_STORE_BAD_STATE);rejection();memcpy(wire,saved,sizeof wire);
 wire[24]=1;crc_fix(wire,n);assert(pet_program_store_decode(&decoded,17,255,wire,n)==PET_PROGRAM_STORE_BAD_HEADER);rejection();memcpy(wire,saved,sizeof wire);
 for(size_t c=0;c<n;c++){assert(pet_program_store_encode(wire,c,&sample)==PET_PROGRAM_STORE_BAD_LENGTH);assert(!memcmp(wire,saved,sizeof wire));}
 assert(pet_program_store_encode((uint8_t*)&sample,sizeof sample,&sample)==PET_PROGRAM_STORE_INVALID_ARGUMENT);
 assert(pet_program_store_decode(&decoded,17,255,(const uint8_t*)&decoded,n)==PET_PROGRAM_STORE_INVALID_ARGUMENT);rejection();
 assert(pet_program_store_encode_text(text,sizeof text,&sample)==PET_PROGRAM_STORE_OK);size_t len=strlen(text);char kept[sizeof text];memcpy(kept,text,sizeof text);for(size_t cap=0;cap<=len;cap++){assert(pet_program_store_encode_text(text,cap,&sample)==PET_PROGRAM_STORE_BAD_LENGTH);assert(!memcmp(kept,text,sizeof text));}text[5]='1';assert(pet_program_store_decode_text(&decoded,17,255,text,len)==PET_PROGRAM_STORE_BAD_ENCODING);rejection();text[5]='2';text[9]='G';assert(pet_program_store_decode_text(&decoded,17,255,text,len)==PET_PROGRAM_STORE_BAD_ENCODING);rejection();
}
static void restore_commit(void){
 fixture(256);sample.revision=10;assert(pet_program_persistence_enqueue("probe",&sample));assert(writes==1&&!strncmp(durable,"{\"p4s2\":",8));
 static pet_program_image_t image;memset(&image,0,sizeof image);image.valid=true;image.schema=17;image.cells=256;image.fuel=64;image.count=5;
 image.code[0]=(pet_program_insn_t){PET_OP_CONST,0,0,0,255};image.code[1]=(pet_program_insn_t){PET_OP_CONST,1,0,0,123};image.code[2]=(pet_program_insn_t){PET_OP_STORE,0,1,0,0};image.code[3]=(pet_program_insn_t){PET_OP_OUTPUT,0,1,0,0};image.code[4]=(pet_program_insn_t){PET_OP_HALT,0,0,0,0};
 persistence_alloc_fail(true);old=decoded;unsigned old_writes=writes;assert(pet_program_persistence_load("probe",&image,&decoded)==PET_PROGRAM_SAVE_UNAVAILABLE);rejection();assert(!pet_program_persistence_enqueue("probe",&sample)&&writes==old_writes);assert(pet_program_persistence_checkpoint("probe",&sample)==PET_APP_CHECKPOINT_BLOCKED);assert(!persistence_live_alloc());persistence_alloc_fail(false);
 assert(pet_program_persistence_load("probe",&image,&decoded)==PET_PROGRAM_SAVE_VALUE);same();
 static pet_program_runtime_t runtime;pet_program_runtime_init(&runtime);assert(pet_program_runtime_start(&runtime,&image,1,current,NULL,1,&decoded)==PET_RUNTIME_OK);
 pet_program_event_t event={0};assert(pet_program_runtime_enqueue(&runtime,1,1,&event)==PET_RUNTIME_OK);
 pet_program_runtime_result_t r=PET_RUNTIME_YIELDED;
 for(unsigned i=0;i<20&&r!=PET_RUNTIME_COMPLETE;i++){
  r=pet_program_runtime_pump(&runtime,1);assert(r==PET_RUNTIME_YIELDED||r==PET_RUNTIME_COMPLETE);
  assert(pet_program_runtime_snapshot(&runtime,1,&decoded)==PET_RUNTIME_OK);
  if(r!=PET_RUNTIME_COMPLETE)same();
 }
 assert(r==PET_RUNTIME_COMPLETE&&decoded.revision==11&&decoded.values[255]==123&&decoded.outputs[0]==123);
 assert(pet_program_persistence_enqueue("probe",&decoded));sample=decoded;
 assert(pet_program_persistence_load_cached("probe",&image,&decoded)==PET_PROGRAM_SAVE_VALUE);same();
 for(int s=PET_APP_STORE_EMPTY;s<=PET_APP_STORE_INVALID;s++){if(s==PET_APP_STORE_VALUE)continue;status=(pet_app_store_status_t)s;old=decoded;assert(pet_program_persistence_load("probe",&image,&decoded)!=PET_PROGRAM_SAVE_VALUE);rejection();}status=PET_APP_STORE_VALUE;
 old=decoded;durable[20]=durable[20]=='0'?'1':'0';assert(pet_program_persistence_load("probe",&image,&decoded)==PET_PROGRAM_SAVE_CORRUPT);rejection();
 update=false;assert(!pet_program_persistence_enqueue("probe",&sample));
 /* A trap after STORE must publish neither the high cell nor the output. */
 assert(pet_program_runtime_cancel(&runtime,1)==PET_RUNTIME_OK);pet_program_report_t report;assert(pet_program_runtime_take_report(&runtime,1,&report)==PET_RUNTIME_OK);
 image.code[3]=(pet_program_insn_t){PET_OP_DIV,2,1,3,0};pet_program_runtime_init(&runtime);assert(pet_program_runtime_start(&runtime,&image,1,current,NULL,64,&sample)==PET_RUNTIME_OK);assert(pet_program_runtime_enqueue(&runtime,1,1,&event)==PET_RUNTIME_OK);
 for(unsigned i=0;i<10;i++){r=pet_program_runtime_pump(&runtime,1);if(r==PET_RUNTIME_COMPLETE)break;}
 assert(r==PET_RUNTIME_COMPLETE);assert(pet_program_runtime_take_report(&runtime,1,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_DIV_ZERO);
 assert(pet_program_runtime_snapshot(&runtime,1,&decoded)==PET_RUNTIME_OK);same();
}
int main(int argc,char **argv){if(argc>1){fixture(!strcmp(argv[1],"v1")?64:256);size_t n=pet_program_store_size(&sample);assert(pet_program_store_encode(wire,n,&sample)==PET_PROGRAM_STORE_OK);assert(fwrite(wire,1,n,stdout)==n);return 0;}roundtrips();rejects();restore_commit();assert(!persistence_live_alloc());puts("P4S1 compatibility; P4S2 0..256 roundtrips; strict rejection; restore and atomic commit/rollback OK");return 0;}
