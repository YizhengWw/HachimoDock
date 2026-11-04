/* Pure same-core checkpoint and real host deadline boundary. No IO/devices. */
#include "pet_p4_program_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const pet_program_insn_t code[]={
 {PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
 {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}};
typedef struct {int64_t now,delta;unsigned calls,bad_at;pet_program_host_t *reenter;} clock_t;
static int64_t clock_read(void *context){clock_t *c=context;c->calls++;if(c->reenter)assert(pet_program_host_pump(c->reenter,c->reenter->runtime.generation)==PET_RUNTIME_BUSY);if(c->calls==c->bad_at)return -1;int64_t out=c->now;c->now+=c->delta;return out;}
static bool alive=true,busy=false,held=false;
static unsigned authority_calls,finishes;
static pet_resource_broker_t broker;
static uint64_t serial,generation;
static bool core_current(void *unused,uint64_t g){(void)unused;authority_calls++;return alive&&g==1;}
static pet_program_checkpoint_result_t revoke_checkpoint(void *unused){(void)unused;alive=false;return PET_PROGRAM_CONTINUE;}
static pet_program_checkpoint_result_t invalid_checkpoint(void *unused){(void)unused;return (pet_program_checkpoint_result_t)99;}
static pet_program_checkpoint_result_t varied_checkpoint(void *context){uint32_t *value=context;*value=*value*UINT32_C(1664525)+UINT32_C(1013904223);return (*value&3)?PET_PROGRAM_CONTINUE:PET_PROGRAM_YIELD;}
static bool audio_busy(void *unused){(void)unused;return busy;}
static pet_resource_result_t open_owner(void *unused,uint32_t mask,pet_resource_owner_t *out){
 (void)unused;assert(mask==PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM)&&!held);alive=true;*out=(pet_resource_owner_t){&broker,++generation,0};return PET_RESOURCE_OK;
}
static pet_resource_result_t acquire(void *unused,pet_resource_owner_t owner,pet_resource_kind_t kind,uint32_t units,pet_resource_lease_t *out){
 (void)unused;assert(alive&&!held&&kind==PET_RESOURCE_PROGRAM&&owner.generation==generation);held=true;*out=(pet_resource_lease_t){owner,++serial,units,kind,0};return PET_RESOURCE_OK;
}
static bool current(void *unused,pet_resource_lease_t lease){(void)unused;assert(held);return alive&&lease.owner.generation==generation;}
static void revoke(void *unused,pet_resource_owner_t owner){(void)unused;assert(owner.generation==generation);alive=false;}
static pet_resource_finish_t finish(void *unused,pet_resource_lease_t lease){(void)unused;assert(held&&!alive&&lease.owner.generation==generation);held=false;finishes++;return PET_RESOURCE_FINISH_REVOKED;}
static const pet_program_host_backend_t backend={NULL,audio_busy,open_owner,acquire,current,revoke,finish};
static pet_program_image_t image(void){pet_program_image_t out={.schema=1,.count=7,.fuel=64,.cells=1,.valid=true};memcpy(out.code,code,sizeof(code));return out;}
static bool publication(void *unused,pet_resource_lease_t lease,pet_program_state_t *out,const pet_program_state_t *next){(void)unused;assert(held&&lease.owner.generation==generation);*out=*next;return true;}
static void core_cases(void){
 pet_program_t program={code,7,64,1,0};pet_program_event_t event={1,{5}};
 pet_program_state_t state,before;pet_program_workspace_t w;
 pet_program_authority_t authority={1,core_current,NULL,NULL,0};
 assert(pet_program_init(&state,1));before=state;assert(pet_program_begin(&program,&event,&state,&authority,&w)==PET_PROGRAM_RUNNING);
 clock_t clock={.delta=100};unsigned turns=0;
 while(w.result==PET_PROGRAM_RUNNING){
  pet_program_deadline_t d={0};assert(pet_program_deadline_begin(&d,clock_read,&clock,250));
  unsigned old=w.used_fuel;pet_program_result_t result=pet_program_step_with_checkpoint(&w,&authority,64,pet_program_deadline_check,&d);
  assert(w.used_fuel-old<=2&&!memcmp(&state,&before,sizeof(state)));
  if(result==PET_PROGRAM_RUNNING){unsigned calls=clock.calls;assert(pet_program_step_with_checkpoint(&w,&authority,64,pet_program_deadline_check,&d)==PET_PROGRAM_RUNNING);assert(clock.calls==calls&&w.used_fuel-old==2);}
  assert(++turns<=4);
 }
 assert(turns==4&&w.used_fuel==7&&w.result==PET_PROGRAM_READY);
 unsigned calls=clock.calls;assert(pet_program_step_with_checkpoint(&w,&authority,64,invalid_checkpoint,NULL)==PET_PROGRAM_NOT_READY);assert(clock.calls==calls);
 assert(pet_program_commit(&state,&w,&authority)==PET_PROGRAM_COMMITTED&&state.cells[0]==5&&state.outputs[0]==5);
 assert(pet_program_begin(&program,&event,&state,&authority,&w)==PET_PROGRAM_RUNNING);before=state;
 clock=(clock_t){.delta=100,.bad_at=3};pet_program_deadline_t d={0};assert(pet_program_deadline_begin(&d,clock_read,&clock,750));
 assert(pet_program_step_with_checkpoint(&w,&authority,64,pet_program_deadline_check,&d)==PET_PROGRAM_CANCELLED&&w.used_fuel==1);
 assert(!memcmp(&state,&before,sizeof(state)));calls=clock.calls;assert(pet_program_deadline_check(&d)==PET_PROGRAM_CANCEL_SLICE&&clock.calls==calls);
 assert(pet_program_begin(&program,&event,&state,&authority,&w)==PET_PROGRAM_RUNNING);alive=true;authority_calls=0;
 assert(pet_program_step_with_checkpoint(&w,&authority,64,revoke_checkpoint,NULL)==PET_PROGRAM_CANCELLED&&w.used_fuel==0&&authority_calls==1);
 alive=true;assert(pet_program_begin(&program,&event,&state,&authority,&w)==PET_PROGRAM_RUNNING);
 assert(pet_program_step_with_checkpoint(&w,&authority,64,invalid_checkpoint,NULL)==PET_PROGRAM_INVALID&&w.used_fuel==0);
 clock=(clock_t){.now=100,.delta=0};assert(pet_program_deadline_begin(&d,clock_read,&clock,10));clock.now=99;
 assert(pet_program_deadline_check(&d)==PET_PROGRAM_CANCEL_SLICE);
 pet_program_deadline_t unchanged=d;assert(!pet_program_deadline_begin(&d,NULL,NULL,10)&&!memcmp(&d,&unchanged,sizeof(d)));
 assert(!pet_program_deadline_begin(&d,clock_read,&clock,0));assert(!pet_program_deadline_begin(&d,clock_read,&clock,2001));
 for(unsigned trial=0;trial<1000;trial++){
  pet_program_state_t expected;pet_program_workspace_t original;uint32_t random=trial+1;event.words[0]=(int32_t)(trial%101)-50;
  alive=true;assert(pet_program_init(&state,1));expected=state;before=state;
  assert(pet_program_run(&program,&event,&expected,&authority,&original)==PET_PROGRAM_READY);
  assert(pet_program_commit(&expected,&original,&authority)==PET_PROGRAM_COMMITTED);
  assert(pet_program_begin(&program,&event,&state,&authority,&w)==PET_PROGRAM_RUNNING);unsigned slices=0;
  while(w.result==PET_PROGRAM_RUNNING){assert(pet_program_step_with_checkpoint(&w,&authority,(uint16_t)(trial%64+1),varied_checkpoint,&random)==w.result);assert(!memcmp(&state,&before,sizeof(state)));assert(++slices<128);}
  assert(w.result==PET_PROGRAM_READY&&w.used_fuel==7);assert(pet_program_commit(&state,&w,&authority)==PET_PROGRAM_COMMITTED&&!memcmp(&state,&expected,sizeof(state)));
 }
}
static void host_cases(void){
 pet_program_host_t host={0};pet_program_image_t p=image();clock_t clock={.delta=100};uint64_t g=0;
 assert(pet_program_host_init(&host,&backend));assert(pet_program_host_bind_clock(&host,clock_read,&clock,250));
 assert(!pet_program_host_bind_clock(&host,clock_read,&clock,250));assert(pet_program_host_bind_publication(&host,publication,NULL));
 busy=true;assert(pet_program_host_start_image(&host,&p,64,NULL,&g)==PET_RUNTIME_DENIED&&!g&&!held&&!clock.calls);
 assert(host.admission_attempted&&!pet_program_host_bind_publication(&host,publication,NULL));busy=false;
 assert(pet_program_host_start_image(&host,&p,64,NULL,&g)==PET_RUNTIME_OK);
 clock.reenter=&host;
 pet_program_event_t event={1,{5}};assert(pet_program_host_enqueue(&host,g,1,&event)==PET_RUNTIME_OK);assert(pet_program_host_enqueue(&host,g,2,&event)==PET_RUNTIME_OK);
 unsigned reports=0,pumps=0;while(reports<2){
  pet_program_runtime_result_t r=pet_program_host_pump(&host,g);assert(r==PET_RUNTIME_YIELDED||r==PET_RUNTIME_COMPLETE);assert(++pumps<30);
  if(r==PET_RUNTIME_COMPLETE){pet_program_report_t report;assert(pet_program_host_take_report(&host,g,&report)==PET_RUNTIME_OK);assert(report.sequence==++reports&&report.used_fuel==7&&report.result==PET_PROGRAM_COMMITTED);}
 }
 assert(pumps==12&&host.runtime.state.cells[0]==10&&host.runtime.state.revision==2&&held);
 unsigned calls=clock.calls;assert(pet_program_host_pump(&host,g+1)==PET_RUNTIME_STALE&&clock.calls==calls);
 assert(pet_program_host_enqueue(&host,g,3,&event)==PET_RUNTIME_OK);assert(pet_program_host_pump(&host,g)==PET_RUNTIME_YIELDED);
 // A backwards value between slices is not a fresh valid clock epoch.
 clock.now=0;unsigned old_finishes=finishes;assert(pet_program_host_pump(&host,g)==PET_RUNTIME_DENIED&&!held&&finishes==old_finishes+1);
 assert(host.runtime.state.cells[0]==10&&host.runtime.state.revision==2);calls=clock.calls;
 assert(pet_program_host_pump(&host,g)==PET_RUNTIME_STOPPED&&clock.calls==calls);
 assert(!pet_program_host_bind_clock(&host,clock_read,&clock,750));
 pet_program_snapshot_t seed;assert(pet_program_host_snapshot(&host,g,&seed)==PET_RUNTIME_OK);
 clock.now=10000;uint64_t next=0;assert(pet_program_host_start_image(&host,&p,64,&seed,&next)==PET_RUNTIME_BUSY&&!next);
 pet_program_report_t stopped;assert(pet_program_host_take_report(&host,g,&stopped)==PET_RUNTIME_OK&&stopped.sequence==3&&stopped.result==PET_PROGRAM_CANCELLED&&stopped.revision==2);
 assert(pet_program_host_start_image(&host,&p,64,&seed,&next)==PET_RUNTIME_OK&&next>g);
 assert(host.runtime.state.cells[0]==10&&host.runtime.state.revision==2&&!host.runtime.queued);
 assert(pet_program_host_enqueue(&host,g,99,&event)==PET_RUNTIME_STALE);
 assert(pet_program_host_pump(&host,next)==PET_RUNTIME_IDLE);
 assert(pet_program_host_enqueue(&host,next,1,&event)==PET_RUNTIME_OK);assert(pet_program_host_pump(&host,next)==PET_RUNTIME_YIELDED);
 clock.delta=1000;
 for(unsigned i=0;i<7;i++)assert(pet_program_host_pump(&host,next)==PET_RUNTIME_YIELDED&&host.runtime.workspace.used_fuel==0&&held);
 assert(pet_program_host_pump(&host,next)==PET_RUNTIME_DENIED&&!held&&host.slice_stalled&&host.zero_progress_turns==8);
 assert(host.runtime.state.cells[0]==10&&host.runtime.state.revision==2);
 assert(pet_program_host_take_report(&host,next,&stopped)==PET_RUNTIME_OK&&stopped.used_fuel==0&&stopped.result==PET_PROGRAM_CANCELLED);
 clock.delta=0;g=0;assert(pet_program_host_start_image(&host,&p,64,&seed,&g)==PET_RUNTIME_OK&&g>next&&!host.slice_stalled&&!host.zero_progress_turns);
 assert(pet_program_host_cancel(&host,g)==PET_RUNTIME_OK&&!held);
 // No publisher/clock may be attached after a first audio-denied attempt,
 // even though no owner generation was allocated by the backend.
 pet_program_host_t denied={0};assert(pet_program_host_init(&denied,&backend));busy=true;g=0;
 assert(pet_program_host_start_image(&denied,&p,64,NULL,&g)==PET_RUNTIME_DENIED);
 assert(!pet_program_host_bind_clock(&denied,clock_read,&clock,750));assert(!pet_program_host_bind_publication(&denied,publication,NULL));busy=false;
}
int main(void){core_cases();host_cases();puts("PASS cooperative deadline, exact continuation, clock faults, ownership and immutable factory");return 0;}
