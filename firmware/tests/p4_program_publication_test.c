/* Real broker/factory. Execution quota opens ONLY in this host test binary.
 * Realtime reservations are simulated; no audio IO, provider or driver calls. */
#include "pet_p4_program_host_device.h"
#include "pet_p4_resources_device.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
bool pet_p4_audio_active(void){return false;}
bool pet_p4_audio_playback_active(void){return false;}
bool pet_p4_audio_conversation_active(void){return false;}
bool pet_p4_audio_music_active(void){return false;}
int64_t esp_timer_get_time(void){return 0;} /* Atomic-boundary test, not elapsed-time evidence. */
static pet_program_host_t host;
static pet_resource_lease_t lease;
static pet_resource_realtime_t realtime;
static pet_program_state_t state,candidate;
static atomic_bool ready;
static atomic_uint waiting;
static bool copied;
static void *audio_transition(void *unused){
  (void)unused;atomic_fetch_add(&waiting,1);while(!atomic_load(&ready))sched_yield();
  assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);return NULL;
}
static void *publication(void *unused){
  (void)unused;atomic_fetch_add(&waiting,1);while(!atomic_load(&ready))sched_yield();
  copied=pet_resources_device_program_publish(lease,&state,&candidate);return NULL;
}
static pet_resource_owner_t open_owner(void){
  pet_resource_owner_t owner={0};lease=(pet_resource_lease_t){0};
  assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM),&owner)==PET_RESOURCE_OK);
  assert(pet_resources_device_acquire(owner,PET_RESOURCE_PROGRAM,64,&lease)==PET_RESOURCE_OK);return owner;
}
static void make_candidate(pet_resource_owner_t owner,int value){
  assert(pet_program_init(&state,owner.generation));candidate=state;candidate.revision=1;
  for(unsigned i=0;i<PET_PROGRAM_CELLS;i++)candidate.cells[i]=value+(int)i;
  for(unsigned i=0;i<PET_PROGRAM_OUTPUTS;i++)candidate.outputs[i]=value-(int)i;
}
static bool current(void *unused,uint64_t generation){
  (void)unused;return generation==lease.owner.generation&&pet_resources_device_current(lease);
}
static unsigned publications;
static bool revoke_at_publication(void *unused,uint64_t generation,pet_program_state_t *out,const pet_program_state_t *next){
  (void)unused;assert(generation==lease.owner.generation);publications++;
  assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
  return pet_resources_device_program_publish(lease,out,next);
}
static void exact_commit_boundary(void){
  pet_resource_owner_t owner=open_owner();assert(pet_program_init(&state,owner.generation));
  const pet_program_insn_t code[]={{PET_OP_CONST,0,0,0,7},{PET_OP_CONST,1,0,0,0},
    {PET_OP_STORE,1,0,0,0},{PET_OP_OUTPUT,0,0,0,0},{PET_OP_HALT,0,0,0,0}};
  const pet_program_t program={code,5,64,1,0};pet_program_event_t event={0};
  pet_program_authority_t authority={owner.generation,current,NULL,NULL,0};pet_program_workspace_t work;
  assert(pet_program_begin(&program,&event,&state,&authority,&work)==PET_PROGRAM_RUNNING);
  assert(pet_program_step(&work,&authority,64)==PET_PROGRAM_READY);pet_program_state_t before=state;
  assert(pet_program_commit_with_publication(&state,&work,&authority,NULL,revoke_at_publication,NULL)==PET_PROGRAM_CANCELLED);
  assert(!memcmp(&state,&before,sizeof(state))&&publications==1);
  assert(pet_program_commit_with_publication(&state,&work,&authority,NULL,revoke_at_publication,NULL)==PET_PROGRAM_NOT_READY);
  assert(publications==1);pet_resources_device_realtime_end(&realtime);
  assert(pet_resources_device_finish(lease)==PET_RESOURCE_FINISH_REVOKED);
}
static void real_factory_publication(void){
  assert(pet_program_host_device_init(&host)&&host.publish);
  assert(!pet_program_host_bind_publication(&host,NULL,NULL));
  pet_program_image_t image={.schema=1,.count=5,.fuel=64,.cells=1,.valid=true};
  const pet_program_insn_t code[]={{PET_OP_CONST,0,0,0,9},{PET_OP_CONST,1,0,0,0},
    {PET_OP_STORE,1,0,0,0},{PET_OP_OUTPUT,0,0,0,0},{PET_OP_HALT,0,0,0,0}};
  memcpy(image.code,code,sizeof(code));uint64_t generation=0;
  assert(pet_program_host_start_image(&host,&image,64,NULL,&generation)==PET_RUNTIME_OK);
  assert(!pet_program_host_bind_publication(&host,host.publish,NULL));
  pet_program_event_t event={0};assert(pet_program_host_enqueue(&host,generation,1,&event)==PET_RUNTIME_OK);
  assert(pet_program_host_pump(&host,generation)==PET_RUNTIME_YIELDED);
  assert(pet_program_host_pump(&host,generation)==PET_RUNTIME_YIELDED);
  assert(pet_program_host_pump(&host,generation)==PET_RUNTIME_COMPLETE);
  assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
  pet_program_report_t report;assert(pet_program_host_take_report(&host,generation,&report)==PET_RUNTIME_OK);
  assert(report.result==PET_PROGRAM_COMMITTED&&report.revision==1);
  pet_program_snapshot_t snapshot;assert(pet_program_host_snapshot(&host,generation,&snapshot)==PET_RUNTIME_OK);
  assert(snapshot.values[0]==9&&snapshot.outputs[0]==9);
  assert(pet_program_host_check_current(&host,generation)==PET_RUNTIME_DENIED);
  pet_resources_device_realtime_end(&realtime);
  assert(pet_program_host_pump(&host,generation)==PET_RUNTIME_STOPPED);
  assert(!host.bound&&!host.runtime.active);
}
int main(void){
  exact_commit_boundary();real_factory_publication();
  for(unsigned iteration=0;iteration<1000;iteration++){
    pet_resource_owner_t owner=open_owner();make_candidate(owner,(int)iteration);pet_program_state_t before=state;
    atomic_store(&ready,false);atomic_store(&waiting,0);pthread_t audio,commit;
    assert(!pthread_create(&audio,NULL,audio_transition,NULL));assert(!pthread_create(&commit,NULL,publication,NULL));
    while(atomic_load(&waiting)!=2)sched_yield();atomic_store(&ready,true);
    assert(!pthread_join(audio,NULL)&&!pthread_join(commit,NULL));
    assert(!memcmp(&state,copied?&candidate:&before,sizeof(state)));
    pet_resources_device_realtime_end(&realtime);
    pet_program_state_t retained=state;assert(!pet_resources_device_program_publish(lease,&state,&candidate));
    assert(!memcmp(&state,&retained,sizeof(state)));
    assert(pet_resources_device_finish(lease)==PET_RESOURCE_FINISH_REVOKED);
  }
  pet_resource_owner_t owner=open_owner();make_candidate(owner,1);pet_program_state_t before=state;
  pet_resource_lease_t bad=lease;bad.kind=PET_RESOURCE_STORAGE_WORK;
  assert(!pet_resources_device_program_publish(bad,&state,&candidate));
  assert(!pet_resources_device_program_publish(lease,&state,&state));
  candidate.outbox_count=1;assert(!pet_resources_device_program_publish(lease,&state,&candidate));
  candidate.outbox_count=0;candidate.generation++;assert(!pet_resources_device_program_publish(lease,&state,&candidate));
  assert(!memcmp(&state,&before,sizeof(state)));
  pet_resources_device_revoke(owner);assert(pet_resources_device_finish(lease)==PET_RESOURCE_FINISH_REVOKED);
  pet_resource_usage_t usage;pet_resources_device_usage(&usage);assert(!usage.live_owners&&!usage.revoked_owners&&!usage.realtime_reservations);
  for(unsigned i=0;i<PET_RESOURCE_KIND_COUNT;i++)assert(!usage.leases[i]&&!usage.units[i]);
  puts("PASS real broker/factory: 1000 concurrent publication boundaries, no partial state or automatic restart");return 0;
}
