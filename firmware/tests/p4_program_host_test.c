/* Actual host+runtime+image+core. Synthetic admission is TEST ONLY: it does not
 * patch the production broker's zero PROGRAM quota or any compiled capability. */
#include "pet_p4_program_host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pet_program_host_t host;
static const pet_program_insn_t accumulator[] = {
  {PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
  {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}
};
static pet_program_image_t image;
static pet_resource_broker_t identity;
typedef struct {
  uint64_t generation, serial;
  pet_resource_owner_t owner;
  pet_resource_lease_t ticket;
  unsigned opens, acquires, checks, finishes, revokes, audio_checks, reentries;
  unsigned deny_at;
  bool enabled, live, busy, reenter, held, bad_ticket, deny_open;
} mock_t;
static mock_t mock;
static bool same_owner(pet_resource_owner_t a, pet_resource_owner_t b) {
  return a.broker==b.broker && a.generation==b.generation && a.slot==b.slot;
}
static bool same_ticket(pet_resource_lease_t a, pet_resource_lease_t b) {
  return same_owner(a.owner,b.owner) && a.serial==b.serial && a.units==b.units && a.kind==b.kind && a.slot==b.slot;
}
static void reenter(mock_t *m) {
  if (!m->reenter) return;
  uint64_t g=host.runtime.generation, output=0;
  pet_program_event_t event={1,{99}};
  pet_program_report_t report;
  pet_program_snapshot_t snapshot;
  int32_t base[PET_PROGRAM_UI_VARIABLES]={0};pet_program_ui_snapshot_t ui;
  pet_program_ui_projection_t projection={0};
  pet_program_host_backend_t b=host.backend;
  assert(!pet_program_host_init(&host,&b));
  assert(pet_program_host_stage(&host,&image)==PET_RUNTIME_BUSY);
  assert(pet_program_host_start(&host,1,NULL,&output)==PET_RUNTIME_BUSY);
  assert(pet_program_host_start_image(&host,&image,1,NULL,&output)==PET_RUNTIME_BUSY);
  assert(pet_program_host_start_image_with_contract(&host,&image,1,NULL,NULL,&output)==PET_RUNTIME_BUSY);
  assert(pet_program_host_enqueue(&host,g,99,&event)==PET_RUNTIME_BUSY);
  assert(pet_program_host_pump(&host,g)==PET_RUNTIME_BUSY);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_BUSY);
  assert(pet_program_host_take_report(&host,g,&report)==PET_RUNTIME_BUSY);
  assert(pet_program_host_snapshot(&host,g,&snapshot)==PET_RUNTIME_BUSY);
  assert(pet_program_host_start_image_with_projection(&host,&image,1,NULL,&projection,&output)==PET_RUNTIME_BUSY);
  assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_BUSY);
  assert(pet_program_host_check_current(&host,g)==PET_RUNTIME_BUSY);
  m->reentries++;
}
static bool audio_busy(void *ctx) { mock_t *m=ctx; m->audio_checks++; reenter(m); return m->busy; }
static pet_resource_result_t owner_open(void *ctx,uint32_t mask,pet_resource_owner_t *out) {
  mock_t *m=ctx; reenter(m); m->opens++;
  assert(mask==PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM) && !out->generation && !m->live && !m->held);
  if(m->deny_open) return PET_RESOURCE_BUSY;
  m->owner=(pet_resource_owner_t){&identity,++m->generation,0}; m->live=true; *out=m->owner;
  return PET_RESOURCE_OK;
}
static pet_resource_result_t acquire(void *ctx,pet_resource_owner_t owner,pet_resource_kind_t kind,uint32_t units,pet_resource_lease_t *out) {
  mock_t *m=ctx; reenter(m); m->acquires++;
  assert(same_owner(owner,m->owner) && kind==PET_RESOURCE_PROGRAM && units && !out->serial);
  if(!m->enabled) return PET_RESOURCE_UNAVAILABLE;
  if(m->bad_ticket) {
    *out=(pet_resource_lease_t){{&identity,owner.generation+100,1},999,units,kind,1};
    return PET_RESOURCE_OK; /* foreign ticket must not be finished */
  }
  m->ticket=(pet_resource_lease_t){owner,++m->serial,units,kind,0};
  m->held=true; *out=m->ticket; return PET_RESOURCE_OK;
}
static bool current(void *ctx,pet_resource_lease_t lease) {
  mock_t *m=ctx; reenter(m); m->checks++;
  assert(m->held && same_ticket(lease,m->ticket));
  return m->live && (!m->deny_at || m->checks<m->deny_at);
}
static void revoke(void *ctx,pet_resource_owner_t owner) {
  mock_t *m=ctx; reenter(m); assert(same_owner(owner,m->owner));
  m->revokes++; m->live=false;
}
static pet_resource_finish_t finish(void *ctx,pet_resource_lease_t lease) {
  mock_t *m=ctx; reenter(m);
  assert(m->held && same_ticket(lease,m->ticket));
  /* Physical private work is gone BEFORE finish, including cancellation in an
   * instruction. Terminal reports and committed data deliberately may remain. */
  assert(!host.runtime.active && !host.runtime.inflight && !host.runtime.queued);
  assert(!host.runtime.workspace.program.code);
  m->finishes++; m->held=false; return PET_RESOURCE_FINISH_REVOKED;
}
static void setup(void) {
  memset(&mock,0,sizeof(mock)); mock.enabled=true;
  image=(pet_program_image_t){.schema=42,.count=7,.fuel=64,.cells=1,.valid=true};
  memcpy(image.code,accumulator,sizeof(accumulator));
  pet_program_host_backend_t b={&mock,audio_busy,owner_open,acquire,current,revoke,finish};
  assert(pet_program_host_init(&host,&b));
  assert(!pet_program_host_init(&host,&b));
  assert(pet_program_host_stage(&host,&image)==PET_RUNTIME_OK);
}
static uint64_t start(void) {
  uint64_t generation=999;
  assert(pet_program_host_start(&host,2,NULL,&generation)==PET_RUNTIME_OK);
  assert(generation==mock.owner.generation && host.bound && mock.held);
  return generation;
}
static void enqueue(uint64_t generation,uint64_t sequence,int value) {
  pet_program_event_t event={1,{value}};
  assert(pet_program_host_enqueue(&host,generation,sequence,&event)==PET_RUNTIME_OK);
}
static void complete(uint64_t generation) {
  for(unsigned i=0;i<30;i++) {
    uint16_t before=host.runtime.workspace.used_fuel;
    pet_program_runtime_result_t r=pet_program_host_pump(&host,generation);
    if(r==PET_RUNTIME_COMPLETE) return;
    assert(r==PET_RUNTIME_YIELDED);
    assert(host.runtime.workspace.used_fuel-before<=2);
  }
  assert(!"no bounded completion");
}
static void test_owned_staging_quantum(void) {
  setup(); uint64_t g=start(); image.code[0].op=255;
  enqueue(g,1,7); assert(pet_program_host_pump(&host,g)==PET_RUNTIME_YIELDED);
  assert(host.runtime.workspace.used_fuel==0 && host.runtime.state.revision==0);
  pet_program_image_t next=host.staged; next.fuel=8;
  assert(pet_program_host_stage(&host,&next)==PET_RUNTIME_OK);
  assert(host.lease.units==64 && host.runtime.image.code[0].op==PET_OP_CONST);
  complete(g);
  assert(host.runtime.state.cells[0]==7 && host.runtime.state.revision==1);
  unsigned checks=mock.checks;
  pet_program_snapshot_t out; assert(pet_program_host_snapshot(&host,g,&out)==PET_RUNTIME_OK);
  assert(out.values[0]==7 && mock.checks==checks);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK && mock.finishes==1);
}
static void test_report_backpressure_restart(void) {
  setup(); uint64_t g=start(), next=777; enqueue(g,1,3); enqueue(g,2,4); complete(g);
  assert(pet_program_host_pump(&host,g)==PET_RUNTIME_REPORT_PENDING);
  assert(host.runtime.queued==1 && host.runtime.state.cells[0]==3);
  assert(pet_program_host_start(&host,2,NULL,&next)==PET_RUNTIME_BUSY && next==777);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
  assert(!mock.held && host.runtime.report_pending);
  assert(pet_program_host_start(&host,2,NULL,&next)==PET_RUNTIME_BUSY);
  pet_program_report_t report; assert(pet_program_host_take_report(&host,g,&report)==PET_RUNTIME_OK);
  assert(report.result==PET_PROGRAM_COMMITTED && report.sequence==1);
  pet_program_snapshot_t seed; assert(pet_program_host_snapshot(&host,g,&seed)==PET_RUNTIME_OK);
  assert(pet_program_host_start(&host,2,&seed,&next)==PET_RUNTIME_OK && next>g);
  assert(host.runtime.queued==0 && host.runtime.state.cells[0]==3);
  assert(pet_program_host_pump(&host,g)==PET_RUNTIME_STALE);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_STALE && mock.held);
  assert(pet_program_host_pump(&host,next)==PET_RUNTIME_IDLE);
  assert(pet_program_host_cancel(&host,next)==PET_RUNTIME_OK);
}
static void test_audio_cancel(void) {
  setup(); uint64_t g=start(); enqueue(g,1,8); enqueue(g,2,9);
  assert(pet_program_host_pump(&host,g)==PET_RUNTIME_YIELDED);
  assert(pet_program_host_pump(&host,g)==PET_RUNTIME_YIELDED);
  mock.busy=true; assert(pet_program_host_pump(&host,g)==PET_RUNTIME_DENIED);
  assert(!mock.held && mock.finishes==1 && host.runtime.state.revision==0 && host.runtime.state.cells[0]==0);
  mock.busy=false; assert(pet_program_host_pump(&host,g)==PET_RUNTIME_STOPPED);
  pet_program_report_t r; assert(pet_program_host_take_report(&host,g,&r)==PET_RUNTIME_OK);
  assert(r.result==PET_PROGRAM_CANCELLED && r.sequence==1 && host.runtime.cancelled_events==2);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK && mock.finishes==1);
}
static void test_instruction_cancel_reentry(void) {
  setup(); mock.reenter=true; uint64_t g=start(); enqueue(g,1,9);
  assert(pet_program_host_pump(&host,g)==PET_RUNTIME_YIELDED);
  mock.deny_at=mock.checks+3;
  assert(pet_program_host_pump(&host,g)==PET_RUNTIME_COMPLETE);
  assert(!mock.held && host.runtime.state.cells[0]==0 && host.runtime.report_pending);
  assert(mock.reentries>10 && mock.finishes==1);
  pet_program_report_t r; unsigned checks=mock.checks;
  assert(pet_program_host_take_report(&host,g,&r)==PET_RUNTIME_OK && r.result==PET_PROGRAM_CANCELLED);
  assert(mock.checks==checks);
}
static void test_failed_start_preserves_committed(void) {
  setup(); uint64_t g=start(); enqueue(g,1,5); complete(g);
  pet_program_report_t r; assert(pet_program_host_take_report(&host,g,&r)==PET_RUNTIME_OK);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
  pet_program_runtime_t before=host.runtime; pet_program_image_t prior=host.staged;
  image.code[0].op=255;
  assert(pet_program_host_stage(&host,&image)==PET_RUNTIME_BAD_IMAGE);
  assert(!memcmp(&prior,&host.staged,sizeof(prior)) && !memcmp(&before,&host.runtime,sizeof(before)));
  uint64_t out=777; mock.enabled=false;
  assert(pet_program_host_start(&host,2,NULL,&out)==PET_RUNTIME_DENIED && out==777);
  uint64_t failed_generation=host.greatest_generation;
  assert(failed_generation>g && !mock.live && !mock.held && !memcmp(&before,&host.runtime,sizeof(before)));
  pet_program_snapshot_t saved; assert(pet_program_host_snapshot(&host,g,&saved)==PET_RUNTIME_OK && saved.values[0]==5);
  mock.enabled=true; saved.schema++;
  assert(pet_program_host_start(&host,2,&saved,&out)==PET_RUNTIME_BAD_STATE && out==777);
  assert(!memcmp(&before,&host.runtime,sizeof(before)) && !mock.held && mock.finishes==2);
  saved.schema--; assert(pet_program_host_start(&host,2,&saved,&out)==PET_RUNTIME_OK && out>failed_generation);
  assert(host.runtime.state.cells[0]==5); assert(pet_program_host_cancel(&host,out)==PET_RUNTIME_OK);
}
static void test_binding_faults(void) {
  setup(); uint64_t out=777;
  mock.deny_open=true; assert(pet_program_host_start(&host,2,NULL,&out)==PET_RUNTIME_BUSY);
  assert(!mock.live && !host.greatest_generation && out==777);
  mock.deny_open=false; mock.bad_ticket=true;
  assert(pet_program_host_start(&host,2,NULL,&out)==PET_RUNTIME_DENIED);
  assert(mock.finishes==0 && !mock.live && !mock.held && !host.runtime.active);
  mock.bad_ticket=false; mock.deny_at=1;
  assert(pet_program_host_start(&host,2,NULL,&out)==PET_RUNTIME_DENIED);
  assert(mock.finishes==1 && !mock.held && host.runtime.generation==0);
  mock.deny_at=0;
  uint64_t greatest=host.greatest_generation; mock.generation=greatest-1;
  assert(pet_program_host_start(&host,2,NULL,&out)==PET_RUNTIME_STALE);
  assert(host.greatest_generation==greatest && mock.finishes==1 && !mock.live);
  assert(pet_program_host_start(&host,2,NULL,&out)==PET_RUNTIME_OK && out>greatest);
  assert(pet_program_host_cancel(&host,out)==PET_RUNTIME_OK);
}
static void test_alias_and_arguments(void) {
  setup(); pet_program_host_t before=host; uint64_t out=777;
  assert(pet_program_host_stage(&host,&host.staged)==PET_RUNTIME_INVALID);
  assert(pet_program_host_start(&host,2,(pet_program_snapshot_t *)&host.staged,&out)==PET_RUNTIME_INVALID);
  assert(pet_program_host_start(&host,2,NULL,&host.greatest_generation)==PET_RUNTIME_INVALID);
  assert(pet_program_host_start(&host,0,NULL,&out)==PET_RUNTIME_INVALID);
  assert(!memcmp(&before,&host,sizeof(host)) && !mock.opens);
  uint64_t g=start();
  assert(pet_program_host_enqueue(&host,g,1,(pet_program_event_t *)&host.staged)==PET_RUNTIME_INVALID);
  assert(pet_program_host_snapshot(&host,g,(pet_program_snapshot_t *)&host.staged)==PET_RUNTIME_INVALID);
  assert(pet_program_host_take_report(&host,g,(pet_program_report_t *)&host.staged)==PET_RUNTIME_INVALID);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
  pet_program_host_backend_t b=host.backend;
  assert(!pet_program_host_init(&host,&b));
}
static void test_trap_no_retry(void) {
  setup(); image.count=1; image.code[0]=(pet_program_insn_t){PET_OP_JUMP,0,0,0,0}; image.fuel=4;
  assert(pet_program_host_stage(&host,&image)==PET_RUNTIME_OK); uint64_t g=start(); enqueue(g,1,3); complete(g);
  pet_program_report_t r; assert(pet_program_host_take_report(&host,g,&r)==PET_RUNTIME_OK && r.result==PET_PROGRAM_FUEL_EXHAUSTED);
  pet_program_event_t e={1,{3}};
  assert(pet_program_host_enqueue(&host,g,1,&e)==PET_RUNTIME_DUPLICATE);
  assert(pet_program_host_pump(&host,g)==PET_RUNTIME_IDLE && !host.runtime.state.revision);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
}
static void test_exhausted_revision_restore(void) {
  setup(); uint64_t g=0;
  pet_program_snapshot_t seed={.schema=42,.revision=UINT32_MAX-1U,.cells=1,.values={5}};
  assert(pet_program_host_start(&host,2,&seed,&g)==PET_RUNTIME_OK);
  enqueue(g,1,2); complete(g);
  pet_program_report_t r; assert(pet_program_host_take_report(&host,g,&r)==PET_RUNTIME_OK);
  assert(r.result==PET_PROGRAM_COMMITTED && r.revision==UINT32_MAX);
  assert(pet_program_host_snapshot(&host,g,&seed)==PET_RUNTIME_OK);
  assert(seed.revision==UINT32_MAX && seed.values[0]==7 && seed.outputs[0]==7);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
  uint64_t next=0; assert(pet_program_host_start(&host,2,&seed,&next)==PET_RUNTIME_OK && next>g);
  enqueue(next,1,2); assert(pet_program_host_pump(&host,next)==PET_RUNTIME_COMPLETE);
  assert(pet_program_host_take_report(&host,next,&r)==PET_RUNTIME_OK);
  assert(r.result==PET_PROGRAM_OVERFLOW && r.revision==UINT32_MAX && r.used_fuel==0);
  pet_program_snapshot_t after; assert(pet_program_host_snapshot(&host,next,&after)==PET_RUNTIME_OK);
  assert(!memcmp(&seed,&after,sizeof(seed)));
  pet_program_event_t e={1,{2}};
  assert(pet_program_host_enqueue(&host,next,1,&e)==PET_RUNTIME_DUPLICATE);
  assert(pet_program_host_pump(&host,next)==PET_RUNTIME_IDLE);
  assert(pet_program_host_cancel(&host,next)==PET_RUNTIME_OK && mock.finishes==2);
}
static void test_fresh_semantic_zero_with_padding(void) {
  pet_program_host_t fresh={0};
  pet_program_host_backend_t b={&mock,audio_busy,owner_open,acquire,current,revoke,finish};
  /* Deliberately dirty ONLY padding, not any scalar/member representation.
   * Aggregate initialization is allowed to leave these bytes unspecified. */
  size_t begin=offsetof(pet_program_host_t,owner)+offsetof(pet_resource_owner_t,slot)+sizeof(fresh.owner.slot);
  size_t end=offsetof(pet_program_host_t,owner)+sizeof(fresh.owner);
  assert(end>begin); memset((unsigned char *)&fresh+begin,0xa5,end-begin);
  assert(pet_program_host_init(&fresh,&b));
  pet_program_host_t saved=fresh;
  assert(!pet_program_host_init(&fresh,&b));
  assert(!memcmp(&saved,&fresh,sizeof(fresh)));
  pet_program_host_t invalid={0}; invalid.runtime.marker=1;
  assert(!pet_program_host_init(&invalid,&b) && invalid.runtime.marker==1);
  invalid.runtime.marker=0; invalid.lease.owner.generation=4;
  assert(!pet_program_host_init(&invalid,&b) && invalid.lease.owner.generation==4);
}
static void test_transactional_image_publication(void){
  setup();uint64_t g=start();enqueue(g,1,5);complete(g);
  pet_program_report_t report;assert(pet_program_host_take_report(&host,g,&report)==PET_RUNTIME_OK);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
  pet_program_image_t prior=host.staged,next=image;pet_program_runtime_t runtime=host.runtime;
  next.schema=43;next.cells=2;next.fuel=8;
  uint64_t generation=777;mock.enabled=false;
  assert(pet_program_host_start_image(&host,&next,1,NULL,&generation)==PET_RUNTIME_DENIED);
  assert(generation==777&&!memcmp(&prior,&host.staged,sizeof(prior))&&!memcmp(&runtime,&host.runtime,sizeof(runtime)));
  mock.enabled=true;pet_program_snapshot_t seed={.schema=42,.cells=1,.values={9}};
  assert(pet_program_host_start_image(&host,&next,1,&seed,&generation)==PET_RUNTIME_BAD_STATE);
  assert(generation==777&&!memcmp(&prior,&host.staged,sizeof(prior))&&!memcmp(&runtime,&host.runtime,sizeof(runtime)));
  seed.schema=43;seed.cells=2;
  assert(pet_program_host_start_image(&host,&next,1,&seed,&generation)==PET_RUNTIME_OK&&generation>g);
  assert(host.staged.schema==43&&host.runtime.image.schema==43&&host.lease.units==8);
  next.code[0].op=255;enqueue(generation,1,2);complete(generation);
  assert(host.runtime.state.cells[0]==11&&host.runtime.state.revision==1);
  assert(pet_program_host_start_image(&host,&image,1,NULL,&g)==PET_RUNTIME_BUSY);
  assert(pet_program_host_cancel(&host,generation)==PET_RUNTIME_OK);
}
static void test_transactional_image_aliases(void){
  setup();pet_program_host_t before=host;uint64_t generation=777;
  assert(pet_program_host_start_image(&host,NULL,1,NULL,&generation)==PET_RUNTIME_INVALID);
  assert(pet_program_host_start_image(&host,&host.staged,1,NULL,&generation)==PET_RUNTIME_INVALID);
  assert(pet_program_host_start_image(&host,&image,1,NULL,(uint64_t*)&image.code[0])==PET_RUNTIME_INVALID);
  pet_program_snapshot_t seed={.schema=42,.cells=1};
  assert(pet_program_host_start_image(&host,&image,1,&seed,(uint64_t*)&seed.values[0])==PET_RUNTIME_INVALID);
  assert(!memcmp(&before,&host,sizeof(host))&&!mock.opens&&generation==777);
}
static void test_receiver_output_contract(void){
  setup();pet_program_output_contract_t limits={0};limits.minimum[0]=-10;limits.maximum[0]=10;
  uint64_t g=777;
  assert(pet_program_host_start_image_with_contract(&host,&image,2,NULL,&limits,&g)==PET_RUNTIME_OK);
  limits.minimum[0]=-1000;limits.maximum[0]=1000; /* copied, not retained */
  enqueue(g,1,11);complete(g);pet_program_report_t report;
  assert(pet_program_host_take_report(&host,g,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_BOUNDS&&report.revision==0);
  assert(host.runtime.state.cells[0]==0&&host.runtime.state.outputs[0]==0);
  enqueue(g,2,10);complete(g);
  assert(pet_program_host_take_report(&host,g,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_COMMITTED&&report.revision==1);
  assert(host.runtime.state.cells[0]==10&&host.runtime.state.outputs[0]==10);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
  pet_program_runtime_t prior=host.runtime;pet_program_image_t prior_image=host.staged;
  pet_program_snapshot_t seed={.schema=42,.revision=1,.cells=1,.values={10},.outputs={10}};
  limits.maximum[0]=9;uint64_t next=777;
  assert(pet_program_host_start_image_with_contract(&host,&image,2,&seed,&limits,&next)==PET_RUNTIME_BAD_STATE);
  assert(next==777&&!memcmp(&prior,&host.runtime,sizeof(prior))&&!memcmp(&prior_image,&host.staged,sizeof(prior_image))&&!mock.held);
  limits.maximum[0]=10;
  assert(pet_program_host_start_image_with_contract(&host,&image,2,&seed,&limits,&next)==PET_RUNTIME_OK);
  assert(host.runtime.output_limited&&host.runtime.state.cells[0]==10&&next>g);
  assert(pet_program_host_cancel(&host,next)==PET_RUNTIME_OK);
}
static pet_program_ui_projection_t recipe(void){
  pet_program_ui_projection_t p={.schema=42,.variables=3,.minimum={-10,-100,-1000},.maximum={10,100,1000}};
  memset(p.target,PET_PROGRAM_UI_UNBOUND,sizeof(p.target));p.target[0]=1;return p;
}
static void test_projection_commit_and_copy(void){
  setup();pet_program_ui_projection_t p=recipe();uint64_t g=0;
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&p,&g)==PET_RUNTIME_OK);
  p.target[0]=2;p.maximum[1]=1;p.schema=99; /* caller cannot reroute a live instance */
  int32_t base[PET_PROGRAM_UI_VARIABLES]={4,9,75,INT32_MAX,INT32_MIN,7,8,9};
  pet_program_ui_snapshot_t ui={0};
  assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_OK);
  assert(ui.generation==g&&ui.schema==42&&ui.revision==0&&ui.variables==3);
  assert(ui.values[0]==4&&ui.values[1]==0&&ui.values[2]==75&&ui.values[3]==INT32_MAX&&ui.values[4]==INT32_MIN);
  enqueue(g,1,16);assert(pet_program_host_pump(&host,g)==PET_RUNTIME_YIELDED);
  assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_OK&&ui.revision==0&&ui.values[1]==0);
  complete(g);assert(host.runtime.report_pending);
  base[2]=88; /* latest receiver-only edit must be retained */
  assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_OK&&ui.revision==1&&ui.values[1]==16&&ui.values[2]==88);
  assert(host.runtime.report_pending); /* projection never consumes completion */
  pet_program_ui_snapshot_t prior=ui;
  base[0]=11;assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_BAD_STATE);
  assert(!memcmp(&prior,&ui,sizeof(ui))&&host.bound);
  base[0]=4;
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
  assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_STOPPED&&!memcmp(&prior,&ui,sizeof(ui)));
  pet_program_snapshot_t saved;assert(pet_program_host_snapshot(&host,g,&saved)==PET_RUNTIME_OK&&saved.outputs[0]==16);
}
static void test_projection_latched_revocation(void){
  setup();pet_program_ui_projection_t p=recipe();uint64_t g=0;
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&p,&g)==PET_RUNTIME_OK);
  enqueue(g,1,9);assert(pet_program_host_pump(&host,g)==PET_RUNTIME_YIELDED);
  int32_t base[PET_PROGRAM_UI_VARIABLES]={0};pet_program_ui_snapshot_t ui;
  memset(&ui,0xa5,sizeof(ui));pet_program_ui_snapshot_t prior=ui;
  mock.deny_at=mock.checks+2; /* admitted first read, stale on final publication check */
  assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_DENIED);
  assert(!memcmp(&prior,&ui,sizeof(ui))&&!host.bound&&!host.runtime.active&&mock.finishes==1);
  mock.deny_at=0;
  assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_STOPPED&&!memcmp(&prior,&ui,sizeof(ui)));
  pet_program_report_t report;assert(pet_program_host_take_report(&host,g,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_CANCELLED);
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&p,&g)==PET_RUNTIME_OK);
  mock.busy=true;
  assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_DENIED&&!memcmp(&prior,&ui,sizeof(ui)));
  mock.busy=false;
  assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_STOPPED&&mock.finishes==2);
}
static void test_projection_transactional_restart(void){
  setup();pet_program_ui_projection_t p=recipe();uint64_t g=0,next=777;
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&p,&g)==PET_RUNTIME_OK);
  enqueue(g,1,15);complete(g);pet_program_report_t report;
  assert(pet_program_host_take_report(&host,g,&report)==PET_RUNTIME_OK);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
  pet_program_host_t prior=host;pet_program_snapshot_t seed;
  assert(pet_program_host_snapshot(&host,g,&seed)==PET_RUNTIME_OK);
  p.maximum[1]=14;
  assert(pet_program_host_start_image_with_projection(&host,&image,2,&seed,&p,&next)==PET_RUNTIME_BAD_STATE&&next==777);
  assert(!memcmp(&prior.runtime,&host.runtime,sizeof(host.runtime))&&!memcmp(&prior.projection,&host.projection,sizeof(p)));
  assert(host.projection_bound&&!memcmp(&prior.staged,&host.staged,sizeof(image)));
  p.maximum[1]=100;p.target[0]=2;
  assert(pet_program_host_start_image_with_projection(&host,&image,2,&seed,&p,&next)==PET_RUNTIME_OK&&next>g);
  int32_t base[PET_PROGRAM_UI_VARIABLES]={1,2,3};pet_program_ui_snapshot_t ui={0};
  assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_STALE&&host.bound);
  assert(pet_program_host_project_current(&host,next,base,&ui)==PET_RUNTIME_OK&&ui.values[1]==2&&ui.values[2]==15);
  assert(pet_program_host_cancel(&host,next)==PET_RUNTIME_OK);
  assert(pet_program_host_start(&host,2,&seed,&next)==PET_RUNTIME_OK&&!host.projection_bound);
  pet_program_ui_snapshot_t saved=ui;
  assert(pet_program_host_project_current(&host,next,base,&ui)==PET_RUNTIME_BAD_STATE&&!memcmp(&saved,&ui,sizeof(ui)));
  assert(pet_program_host_cancel(&host,next)==PET_RUNTIME_OK);
}
static void test_projection_invalid_and_aliases(void){
  setup();pet_program_host_t before=host;pet_program_ui_projection_t p=recipe(),bad=p;uint64_t g=777;
  bad.target[1]=1;
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&bad,&g)==PET_RUNTIME_INVALID);
  bad=p;bad.target[1]=3;
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&bad,&g)==PET_RUNTIME_INVALID);
  bad=p;bad.minimum[3]=1;
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&bad,&g)==PET_RUNTIME_INVALID);
  bad=p;bad.minimum[1]=101;
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&bad,&g)==PET_RUNTIME_INVALID);
  bad=p;bad.schema=43;
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&bad,&g)==PET_RUNTIME_BAD_STATE);
  bad=p;memset(bad.target,PET_PROGRAM_UI_UNBOUND,sizeof(bad.target));
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&bad,&g)==PET_RUNTIME_INVALID);
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,NULL,&g)==PET_RUNTIME_INVALID);
  assert(pet_program_host_start_image_with_projection(&host,&host.staged,2,NULL,&p,&g)==PET_RUNTIME_INVALID);
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&host.projection,&g)==PET_RUNTIME_INVALID);
  union {uint64_t aligned;pet_program_ui_projection_t p;} alias={.p=p};
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&alias.p,&alias.aligned)==PET_RUNTIME_INVALID);
  assert(!memcmp(&before,&host,sizeof(host))&&!mock.opens&&g==777);
  assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&p,&g)==PET_RUNTIME_OK);
  int32_t base[PET_PROGRAM_UI_VARIABLES]={0};pet_program_ui_snapshot_t ui={0},prior=ui;
  assert(pet_program_host_project_current(&host,g,NULL,&ui)==PET_RUNTIME_INVALID);
  assert(pet_program_host_project_current(&host,g,base,NULL)==PET_RUNTIME_INVALID);
  assert(pet_program_host_project_current(&host,g,host.runtime.state.outputs,&ui)==PET_RUNTIME_INVALID);
  assert(pet_program_host_project_current(&host,g,base,(pet_program_ui_snapshot_t*)&host.staged)==PET_RUNTIME_INVALID);
  assert(pet_program_host_project_current(&host,g,ui.values,&ui)==PET_RUNTIME_INVALID);
  assert(!memcmp(&prior,&ui,sizeof(ui)));
  mock.reenter=true;assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_OK&&mock.reentries==4);
  assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
}
static void test_projection_all_ports(void){
  setup();uint64_t g=0;
  for(unsigned port=0;port<PET_PROGRAM_OUTPUTS;port++)for(unsigned target=0;target<PET_PROGRAM_UI_VARIABLES;target++){
    pet_program_ui_projection_t p={.schema=42,.variables=8};
    memset(p.target,PET_PROGRAM_UI_UNBOUND,sizeof(p.target));p.target[port]=(uint8_t)target;
    for(unsigned i=0;i<8;i++){p.minimum[i]=INT32_MIN;p.maximum[i]=INT32_MAX;}
    pet_program_snapshot_t seed={.schema=42,.cells=1,.revision=port*8+target};
    seed.outputs[port]=(port&1)?INT32_MIN:INT32_MAX;
    assert(pet_program_host_start_image_with_projection(&host,&image,2,&seed,&p,&g)==PET_RUNTIME_OK);
    int32_t base[8]={1,2,3,4,5,6,7,8};pet_program_ui_snapshot_t ui={0};
    assert(pet_program_host_project_current(&host,g,base,&ui)==PET_RUNTIME_OK&&ui.revision==seed.revision);
    for(unsigned i=0;i<8;i++)assert(ui.values[i]==(i==target?seed.outputs[port]:base[i]));
    assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK);
  }
}
static void test_copied_host_cannot_revoke_original(void){
  setup();uint64_t g=start();enqueue(g,1,9);
  pet_program_host_t copy=host,original=host,copy_before=copy;
  unsigned checks=mock.checks,finishes=mock.finishes;
  pet_program_event_t event={1,{1}};uint64_t next=777;
  pet_program_snapshot_t snapshot={0},snapshot_before=snapshot;
  pet_program_report_t report={0},report_before=report;
  assert(pet_program_host_cancel(&copy,g)==PET_RUNTIME_INVALID);
  assert(pet_program_host_pump(&copy,g)==PET_RUNTIME_INVALID);
  assert(pet_program_host_check_current(&copy,g)==PET_RUNTIME_INVALID);
  assert(pet_program_host_enqueue(&copy,g,2,&event)==PET_RUNTIME_INVALID);
  assert(pet_program_host_snapshot(&copy,g,&snapshot)==PET_RUNTIME_INVALID);
  assert(pet_program_host_take_report(&copy,g,&report)==PET_RUNTIME_INVALID);
  assert(pet_program_host_stage(&copy,&image)==PET_RUNTIME_INVALID);
  assert(pet_program_host_start(&copy,2,NULL,&next)==PET_RUNTIME_INVALID&&next==777);
  assert(!memcmp(&host,&original,sizeof(host))&&!memcmp(&copy,&copy_before,sizeof(copy)));
  assert(!memcmp(&snapshot,&snapshot_before,sizeof(snapshot))&&!memcmp(&report,&report_before,sizeof(report)));
  assert(mock.checks==checks&&mock.finishes==finishes&&mock.live&&mock.held);
  complete(g);assert(pet_program_host_take_report(&host,g,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_COMMITTED);
  assert(host.runtime.state.cells[0]==9&&pet_program_host_cancel(&host,g)==PET_RUNTIME_OK&&mock.finishes==finishes+1);
}
int main(int argc,char **argv) {
  void (*tests[])(void)={test_owned_staging_quantum,test_report_backpressure_restart,test_audio_cancel,
    test_instruction_cancel_reentry,test_failed_start_preserves_committed,test_binding_faults,test_alias_and_arguments,test_trap_no_retry,
    test_exhausted_revision_restore,test_fresh_semantic_zero_with_padding,
    test_transactional_image_publication,test_transactional_image_aliases,test_receiver_output_contract,
    test_projection_commit_and_copy,test_projection_latched_revocation,test_projection_transactional_restart,
    test_projection_invalid_and_aliases,test_projection_all_ports,test_copied_host_cannot_revoke_original};
  assert(argc==2); unsigned n=(unsigned)strtoul(argv[1],NULL,10); assert(n<sizeof(tests)/sizeof(tests[0])); tests[n]();
  printf("{\"ok\":true,\"case\":%u,\"hostBytes\":%zu}\n",n,sizeof(host)); return 0;
}
