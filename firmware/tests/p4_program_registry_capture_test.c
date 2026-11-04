/* Reuse only the real-broker/pin/clock fixture. Do not call its old main. */
#define main original_registry_vectors_main
#include "p4_program_registry_test.c"
#undef main
int main(void){
 assert(pet_program_registry_init(&registry,pet_program_host_device_init));
 config.enabled=true;config.image=(pet_program_image_t){.schema=1,.count=7,.fuel=128,.cells=2,.valid=true};
 const pet_program_insn_t code[]={
  {PET_OP_CONST,0,0,0,1},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
  {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}};
 memcpy(config.image.code,code,sizeof code);
 config.projection=(pet_program_ui_projection_t){.schema=1,.variables=2,.minimum={0,0},.maximum={100,100}};
 memset(config.projection.target,PET_PROGRAM_UI_UNBOUND,sizeof config.projection.target);config.projection.target[0]=0;
 strcpy(config.actions[0].name,"add");config.actions[0].event=(pet_program_event_t){1,{2}};memset(config.actions[0].input,-1,sizeof config.actions[0].input);config.action_count=1;
 seed.schema=1;seed.cells=2;const uint64_t content[3]={4,5,6};pet_program_registration_t h={0};
 assert(pet_program_registry_attach(&registry,"alpha",content,&config,&seed,base,true,0,&h)==PET_REGISTRY_OK);
 pet_program_registry_entry_t *e=&registry.entries[h.slot];pet_program_snapshot_t snapshot;pet_program_snapshot_cursor_t cursor;
 assert(!e->capture_cursor.runtime);assert(pet_program_registry_snapshot(&registry,h,&snapshot));
 assert(e->capture_cursor.runtime==&e->session.host.runtime&&e->capture_cursor.generation==e->session.generation&&!e->checkpoint_pending);
 /* Fast observations still reproject from the current independent UI base. */
 int32_t fresh[8]={31,63};assert(pet_program_registry_view(&registry,h,fresh,values,&snapshot));assert(values[0]==0&&values[1]==63);
 e->config.projection.target[0]=1;
 assert(pet_program_registry_view(&registry,h,fresh,values,&snapshot)&&values[0]==31&&values[1]==0&&e->capture_projection.target[0]==1);
 e->config.projection=config.projection;
 assert(pet_program_registry_view(&registry,h,fresh,values,&snapshot)&&values[0]==0&&values[1]==63&&e->capture_projection.target[0]==0);
 /* Projection changes force receiving-model validation; invalid limits clear
  * the cursor so restoration cannot accidentally take an old validation hit. */
 e->config.projection.maximum[0]=90;assert(pet_program_registry_snapshot(&registry,h,&snapshot));assert(e->capture_projection.maximum[0]==90&&e->capture_cursor.runtime);
 e->config.projection.maximum[0]=-1;assert(pet_program_registry_snapshot(&registry,h,&snapshot));assert(!e->capture_cursor.runtime&&!e->checkpoint_pending);
 e->config.projection=config.projection;assert(pet_program_registry_snapshot(&registry,h,&snapshot));assert(e->capture_cursor.runtime);
 e->config.image.schema=2;assert(pet_program_registry_snapshot(&registry,h,&snapshot));assert(!e->capture_cursor.runtime);e->config.image.schema=1;
 assert(pet_program_registry_snapshot(&registry,h,&snapshot)&&e->capture_cursor.runtime);
 /* Session/host ownership guards are not bypassed by a matching cursor. */
 assert(pet_component_program_session_snapshot_if_changed(&e->session,&e->capture_cursor,(pet_program_snapshot_t*)&e->session.last_report,&cursor)==PET_RUNTIME_INVALID);
 e->session.host.inside=true;assert(pet_program_registry_snapshot(&registry,h,&snapshot));assert(!e->capture_cursor.runtime);e->session.host.inside=false;
 assert(pet_program_registry_snapshot(&registry,h,&snapshot)&&e->capture_cursor.runtime);
#if PET_P4_PROGRAM_V2
 pet_program_publication_t *p=e->session.host.runtime.state.publication;assert(p);const void *self=p->self;p->self=NULL;
 assert(pet_program_registry_snapshot(&registry,h,&snapshot)&&!e->capture_cursor.runtime);p->self=self;
 assert(pet_program_registry_snapshot(&registry,h,&snapshot)&&e->capture_cursor.runtime);
#endif
 /* Real event changes a nonzero cell and projected output, invalidates cursor,
  * and dirties exactly the new commit. Unchanged polls cannot clear pending. */
 assert(pet_program_registry_enqueue(&registry,h,"add",base)==PET_RUNTIME_OK);finish(h,2);
 assert(e->committed.values[1]==2&&e->committed.outputs[0]==2&&e->capture_cursor.revision==1&&e->checkpoint_pending);
 checkpoint_ok=false;assert(!pet_program_registry_checkpoint_one(&registry));assert(pet_program_registry_snapshot(&registry,h,&snapshot)&&e->checkpoint_pending);
 checkpoint_ok=true;assert(pet_program_registry_checkpoint_one(&registry)&&checkpoints==1&&!e->checkpoint_pending);
 for(unsigned i=0;i<5;i++){assert(pet_program_registry_snapshot(&registry,h,&snapshot));assert(pet_program_registry_checkpoint_one(&registry));}assert(checkpoints==1);
 /* Commit then real audio revocation BEFORE collect: retain final state rather
  * than using the old cursor/report/live flag as a reason to skip capture. */
 assert(pet_program_registry_enqueue(&registry,h,"add",base)==PET_RUNTIME_OK);turn();turn();
 pet_program_schedule_diagnostic_t d={0};assert(pet_program_scheduler_step(&registry.scheduler,++now_ms,&d)==PET_SCHED_TURN);
 assert(pet_program_state_read(&e->session.host.runtime.state)->cells[1]==4&&e->committed.values[1]==2);
 pet_resource_realtime_t realtime={0};assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
 (void)pet_program_registry_step(&registry,++now_ms);assert(e->stopped&&e->committed.values[1]==4&&e->capture_cursor.revision==2&&e->checkpoint_pending);
 e->stopped=false;assert(pet_program_registry_snapshot(&registry,h,&snapshot)&&e->stopped); // Same-revision fast path must still propagate stop.
 assert(!pet_program_registry_release(&registry,h));assert(pet_program_registry_checkpoint_one(&registry)&&saved.values[1]==4&&checkpoints==2);
 pet_resources_device_realtime_end(&realtime);assert(pet_program_registry_stop(&registry,h)==PET_RUNTIME_OK);assert(pet_program_registry_release(&registry,h)&&!e->capture_cursor.runtime);empty_broker();
 /* Reused registry slot with same image/schema/revision but NEW native generation. */
 uint64_t oldgen=e->session.generation;pet_program_registration_t next={0};assert(pet_program_registry_attach(&registry,"alpha",content,&config,&saved,base,true,now_ms,&next)==PET_REGISTRY_OK);
 assert(next.serial>h.serial&&e->session.generation>oldgen&&!e->capture_cursor.runtime);
 assert(pet_program_registry_snapshot(&registry,next,&snapshot)&&snapshot.values[1]==4&&e->capture_cursor.generation==e->session.generation&&!e->checkpoint_pending);
 assert(!pet_program_registry_snapshot(&registry,h,&snapshot));
 /* Fault-inject a native publication with an invalid UNUSED cell. A different
  * revision must take full seed validation, retain last good data, and clear
  * the cursor. Repair uses another real publication, never revision rollback. */
 pet_program_state_t candidate=*pet_program_state_read(&e->session.host.runtime.state);
 candidate.revision++;candidate.cells[PET_PROGRAM_CELLS-1]=1;
 assert(pet_resources_device_program_publish(e->session.host.lease,&e->session.host.runtime.state,&candidate));
 assert(pet_program_registry_snapshot(&registry,next,&snapshot)&&snapshot.revision==2&&snapshot.values[1]==4&&!e->capture_cursor.runtime&&!e->checkpoint_pending);
 candidate.revision++;candidate.cells[PET_PROGRAM_CELLS-1]=0;candidate.cells[1]=6;candidate.outputs[0]=6;
 assert(pet_resources_device_program_publish(e->session.host.lease,&e->session.host.runtime.state,&candidate));
 assert(pet_program_registry_snapshot(&registry,next,&snapshot)&&snapshot.revision==4&&snapshot.values[1]==6&&e->capture_cursor.revision==4&&e->checkpoint_pending);
 assert(pet_program_registry_checkpoint_one(&registry)&&saved.values[1]==6&&checkpoints==3);
 assert(pet_program_registry_stop(&registry,next)==PET_RUNTIME_OK);assert(pet_program_registry_release(&registry,next));empty_broker();
 puts("PASS registry capture: dynamic commit/base/projection invalidation, corrupt publication fallback, stop/revoke/checkpoint/slot reuse");return 0;
}
