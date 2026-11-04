/* Deterministic interleavings in the actual broker adapter, no device/audio IO. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "pet_p4_program_runtime.h"
#include "pet_p4_resources_device.h"
static unsigned lock_depth,large_copies;
static int interleave;
static pet_program_state_t facade,slots[2],candidate,before;
static pet_program_publication_t selector;
static pet_resource_owner_t owner;
static pet_resource_lease_t lease;
static pet_resource_realtime_t realtime;
static void *checked_copy(void*,const void*,size_t);
void test_mux_enter(pthread_mutex_t *m){assert(!lock_depth);assert(!pthread_mutex_lock(m));lock_depth++;}
void test_mux_exit(pthread_mutex_t *m){assert(lock_depth==1);lock_depth--;assert(!pthread_mutex_unlock(m));}
/* Instrument exactly the production copy; no model or broker algorithm replaced. */
#define memcpy checked_copy
#include "pet_p4_resources_device.c"
#undef memcpy
static void *checked_copy(void *out,const void *in,size_t n){
 assert(!lock_depth);large_copies++;assert(n==sizeof(pet_program_state_t)&&n>512);
 assert(out!=pet_program_state_read(&facade));
 const unsigned char *src=in;unsigned char *dst=out;
 for(size_t i=0;i<n/2;i++)dst[i]=src[i];
 if(interleave){
  int mode=interleave;interleave=0;
  assert(!memcmp(pet_program_state_read(&facade),&before,sizeof before));
  if(mode==1)pet_resources_device_revoke(owner);
  if(mode==2)assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
  if(mode==3)assert(!pet_resources_device_program_publish(lease,&facade,&candidate));
  assert(!memcmp(pet_program_state_read(&facade),&before,sizeof before));
 }
 for(size_t i=n/2;i<n;i++)dst[i]=src[i];return out;
}
static void open_model(void){
 owner=(pet_resource_owner_t){0};lease=(pet_resource_lease_t){0};
 assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM),&owner)==PET_RESOURCE_OK);
 assert(pet_resources_device_acquire(owner,PET_RESOURCE_PROGRAM,64,&lease)==PET_RESOURCE_OK);
 assert(pet_program_init(&facade,owner.generation));memset(&selector,0,sizeof selector);
 facade.cells[255]=99;assert(pet_program_state_bind_publication(&facade,&selector,slots));
 before=*pet_program_state_read(&facade);candidate=before;candidate.revision++;candidate.cells[255]=123;candidate.outputs[7]=-8;
}
static void close_model(void){pet_resources_device_revoke(owner);assert(pet_resources_device_finish(lease)==PET_RESOURCE_FINISH_REVOKED);}
static bool authority(void *unused,uint64_t generation){(void)unused;return generation==owner.generation&&pet_resources_device_current(lease);}
static bool publish(void *unused,uint64_t generation,pet_program_state_t *target,const pet_program_state_t *next){(void)unused;assert(generation==owner.generation);return pet_resources_device_program_publish(lease,target,next);}
static void boundaries(void){
 for(int mode=0;mode<4;mode++){
  open_model();unsigned count=large_copies;interleave=mode;
  bool ok=pet_resources_device_program_publish(lease,&facade,&candidate);
  assert(ok==(mode==0||mode==3));assert(large_copies==count+1&&!selector.preparing);
  assert(!memcmp(pet_program_state_read(&facade),ok?&candidate:&before,sizeof candidate));
  assert(facade.cells[255]==99); /* facade is never the large-copy destination */
  if(mode==2)pet_resources_device_realtime_end(&realtime);
  close_model();const pet_program_state_t *kept=pet_program_state_read(&facade);
  assert(!pet_resources_device_program_publish(lease,&facade,&candidate));assert(pet_program_state_read(&facade)==kept);
 }
 open_model();candidate.revision=2;unsigned count=large_copies;
 assert(!pet_resources_device_program_publish(lease,&facade,&candidate)&&large_copies==count);
 candidate=before;candidate.revision=1;candidate.outbox_count=1;
 assert(!pet_resources_device_program_publish(lease,&facade,&candidate)&&large_copies==count);
 candidate.outbox_count=0;pet_program_state_t plain=before;
 assert(!pet_resources_device_program_publish(lease,&plain,&candidate));
 assert(!pet_resources_device_program_publish(lease,&facade,&slots[1]));close_model();
}
static void conflict(void){
 open_model();pet_program_insn_t code[]={{PET_OP_HALT,0,0,0,0}};
 pet_program_t p={.code=code,.count=1,.fuel=1,.cells=256};pet_program_event_t e={0};
 pet_program_authority_t a={.generation=owner.generation,.current=authority};static pet_program_workspace_t w;
 assert(pet_program_run(&p,&e,&facade,&a,&w)==PET_PROGRAM_READY);
 assert(pet_resources_device_program_publish(lease,&facade,&candidate));
 assert(pet_program_commit_with_publication(&facade,&w,&a,NULL,publish,NULL)==PET_PROGRAM_CONFLICT);
 assert(!memcmp(pet_program_state_read(&facade),&candidate,sizeof candidate));close_model();
}
static void runtime_snapshot(void){
 open_model();static pet_program_runtime_t r;pet_program_runtime_init(&r);
 static pet_program_image_t image;memset(&image,0,sizeof image);image.schema=7;image.cells=256;image.count=7;image.fuel=64;image.valid=true;
 image.code[0]=(pet_program_insn_t){PET_OP_CONST,0,0,0,255};image.code[1]=(pet_program_insn_t){PET_OP_EVENT,1,0,0,0};
 image.code[2]=(pet_program_insn_t){PET_OP_STORE,0,1,0,0};image.code[3]=(pet_program_insn_t){PET_OP_EVENT,2,0,0,1};image.code[4]=(pet_program_insn_t){PET_OP_DIV,3,1,2,0};image.code[5]=(pet_program_insn_t){PET_OP_OUTPUT,0,1,0,0};image.code[6]=(pet_program_insn_t){PET_OP_HALT,0,0,0,0};
 assert(pet_program_runtime_start_with_publication(&r,&image,owner.generation,authority,NULL,1,NULL,NULL,publish,NULL)==PET_RUNTIME_OK);
 pet_program_event_t e={.count=2,.words={77,1}};assert(pet_program_runtime_enqueue(&r,owner.generation,1,&e)==PET_RUNTIME_OK);
 static pet_program_snapshot_t snap;pet_program_runtime_result_t result;
 for(unsigned i=0;i<10;i++){
  result=pet_program_runtime_pump(&r,owner.generation);assert(result==PET_RUNTIME_YIELDED||result==PET_RUNTIME_COMPLETE);
  assert(pet_program_runtime_snapshot(&r,owner.generation,&snap)==PET_RUNTIME_OK);
  if(result==PET_RUNTIME_COMPLETE)break;
  assert(snap.revision==0&&snap.values[255]==0&&snap.outputs[0]==0);
 }
 assert(result==PET_RUNTIME_COMPLETE&&snap.revision==1&&snap.values[255]==77&&snap.outputs[0]==77);
 pet_program_report_t report;assert(pet_program_runtime_take_report(&r,owner.generation,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_COMMITTED);
 e.words[0]=99;e.words[1]=0;assert(pet_program_runtime_enqueue(&r,owner.generation,2,&e)==PET_RUNTIME_OK);
 for(unsigned i=0;i<12;i++){result=pet_program_runtime_pump(&r,owner.generation);assert(pet_program_runtime_snapshot(&r,owner.generation,&snap)==PET_RUNTIME_OK);assert(snap.revision==1&&snap.values[255]==77&&snap.outputs[0]==77);if(result==PET_RUNTIME_COMPLETE)break;}
 assert(result==PET_RUNTIME_COMPLETE&&pet_program_runtime_take_report(&r,owner.generation,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_DIV_ZERO);
 /* Read after revocation keeps the last immutable committed snapshot. */
 pet_resources_device_revoke(owner);assert(pet_program_runtime_snapshot(&r,owner.generation,&snap)==PET_RUNTIME_OK&&snap.values[255]==77);
 assert(pet_program_runtime_cancel(&r,owner.generation)==PET_RUNTIME_OK);
 assert(pet_resources_device_finish(lease)==PET_RESOURCE_FINISH_REVOKED);
}
int main(void){assert(sizeof(pet_program_publication_t)<=512&&sizeof(pet_program_state_t)>512);boundaries();conflict();runtime_snapshot();puts("v2 double buffer: revoke, realtime, conflict, snapshot; large copies outside mux; 512 gate unchanged");}
