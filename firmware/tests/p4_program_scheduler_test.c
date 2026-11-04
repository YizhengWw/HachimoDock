/* Actual native scheduler/group/host/runtime, synthetic admission only.
 * Fixed private fixtures; no production quota, device, IO, audio or package. */
#include "pet_p4_program_scheduler.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define NODES (PET_PROGRAM_SCHEDULER_SLOTS+1U)
static pet_program_scheduler_t scheduler,other;
static pet_program_host_t hosts[NODES];
static pet_program_group_t groups[NODES],replacement;
static pet_program_timers_t timers[NODES];
static pet_program_schedule_t tokens[NODES];
static pet_resource_broker_t identity;
static pet_event_bus_t bus;
static pet_event_registration_t publisher,receivers[NODES];
static pet_resource_owner_t publisher_owner;
static uint64_t native_generation;
typedef struct {unsigned index,finishes,checks;bool held,live,busy,reenter;} admission_t;
static admission_t admissions[NODES];
static unsigned reentries;
static bool owner_equal(pet_resource_owner_t a,pet_resource_owner_t b){
  return a.broker==b.broker&&a.generation==b.generation&&a.slot==b.slot;
}
static void check_reentry(admission_t*m){
  if(!m->reenter)return;
  pet_program_schedule_diagnostic_t d={.generation=999};pet_program_schedule_t out={0};
  assert(pet_program_scheduler_step(&scheduler,100,&d)==PET_SCHED_BUSY&&d.generation==999);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_BUSY);
  assert(pet_program_scheduler_remove(&scheduler,tokens[0])==PET_SCHED_BUSY);
  assert(pet_program_scheduler_add(&scheduler,&groups[NODES-1],groups[NODES-1].generation,&out)==PET_SCHED_BUSY&&!out.serial);
  assert(!pet_program_scheduler_init(&scheduler));reentries++;
}
static bool audio(void*context){admission_t*m=context;check_reentry(m);return m->busy;}
static pet_resource_result_t owner_open(void*context,uint32_t mask,pet_resource_owner_t*out){
  admission_t*m=context;assert(!m->held&&!m->live&&!out->generation&&mask==PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM));
  *out=(pet_resource_owner_t){&identity,++native_generation,(uint8_t)m->index};m->live=true;return PET_RESOURCE_OK;
}
static pet_resource_result_t acquire(void*context,pet_resource_owner_t owner,pet_resource_kind_t kind,uint32_t units,pet_resource_lease_t*out){
  admission_t*m=context;assert(m->live&&!m->held&&owner.slot==m->index&&kind==PET_RESOURCE_PROGRAM&&units==64&&!out->serial);
  *out=(pet_resource_lease_t){owner,owner.generation,units,kind,(uint8_t)m->index};m->held=true;return PET_RESOURCE_OK;
}
static bool current(void*context,pet_resource_lease_t lease){
  admission_t*m=context;assert(m->held&&owner_equal(lease.owner,hosts[m->index].owner));m->checks++;check_reentry(m);return m->live;
}
static void revoke(void*context,pet_resource_owner_t owner){
  admission_t*m=context;assert(owner_equal(owner,hosts[m->index].owner));check_reentry(m);m->live=false;
}
static pet_resource_finish_t finish(void*context,pet_resource_lease_t lease){
  admission_t*m=context;pet_program_host_t*h=&hosts[m->index];
  assert(m->held&&owner_equal(lease.owner,h->owner)&&!h->runtime.active&&!h->runtime.inflight&&!h->runtime.queued);
  assert(!h->runtime.workspace.program.code);check_reentry(m);m->held=false;m->finishes++;return PET_RESOURCE_FINISH_REVOKED;
}
static bool bus_current(void*context,pet_resource_owner_t owner){
  (void)context;if(owner_equal(owner,publisher_owner))return true;
  for(unsigned i=0;i<NODES;i++)if(admissions[i].live&&owner_equal(owner,hosts[i].owner))return true;
  return false;
}
static void prepare(unsigned index,bool children){
  static const pet_program_insn_t code[]={
    {PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
    {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}};
  pet_program_image_t image={.schema=42,.count=7,.fuel=64,.cells=1,.valid=true};memcpy(image.code,code,sizeof(code));
  admissions[index].index=index;pet_program_host_backend_t backend={&admissions[index],audio,owner_open,acquire,current,revoke,finish};
  assert(pet_program_host_init(&hosts[index],&backend));uint64_t generation=0;
  assert(pet_program_host_start_image(&hosts[index],&image,2,NULL,&generation)==PET_RUNTIME_OK);
  if(children){
    pet_program_timer_spec_t spec={.id=1,.delay_ms=0,.interval_ms=100,.event={1,{2}}};
    assert(pet_program_timers_start(&timers[index],generation,100,&spec,1)==PET_TIMERS_OK);
    assert(pet_event_bus_open(&bus,hosts[index].owner,0,1,bus_current,NULL,&receivers[index])==PET_EVENT_OK);
  }
  assert(pet_program_group_attach(&groups[index],&hosts[index],children?&timers[index]:NULL,children?&bus:NULL,receivers[index]));
}
static void setup(unsigned count,bool children){
  assert(pet_program_scheduler_init(&scheduler));assert(pet_program_scheduler_init(&other));
  if(children){assert(pet_event_bus_init(&bus));publisher_owner=(pet_resource_owner_t){&identity,100000,7};
    assert(pet_event_bus_open(&bus,publisher_owner,1,0,bus_current,NULL,&publisher)==PET_EVENT_OK);}
  for(unsigned i=0;i<count;i++){prepare(i,children);assert(pet_program_scheduler_add(&scheduler,&groups[i],groups[i].generation,&tokens[i])==PET_SCHED_OK);}
}
static void enqueue(unsigned i,int value){
  pet_program_event_t event={1,{value}};uint64_t sequence=0;
  assert(pet_program_group_enqueue(&groups[i],groups[i].generation,&event,&sequence)==PET_RUNTIME_OK&&sequence==groups[i].sequence);
}
static pet_program_schedule_diagnostic_t step(uint64_t now){
  pet_program_schedule_diagnostic_t d={0};assert(pet_program_scheduler_step(&scheduler,now,&d)==PET_SCHED_TURN&&d.selected);return d;
}
static void test_round_robin_and_single_quantum(void){
  setup(4,false);for(unsigned i=0;i<4;i++)enqueue(i,(int)i+1);
  for(unsigned turn=0;turn<24;turn++){
    unsigned i=turn%4;uint16_t fuel=hosts[i].runtime.workspace.used_fuel;
    pet_program_schedule_diagnostic_t d=step(100);assert(d.schedule.slot==i&&d.pump_attempted&&d.ingress==PET_SCHED_NO_INGRESS);
    if(turn<4)assert(!hosts[i].runtime.workspace.used_fuel&&hosts[i].runtime.inflight==1);
    if(d.pump_result==PET_RUNTIME_YIELDED&&hosts[i].runtime.workspace.used_fuel)assert(hosts[i].runtime.workspace.used_fuel-fuel<=2);
  }
  for(unsigned i=0;i<4;i++){pet_program_report_t r;assert(pet_program_group_take_report(&groups[i],groups[i].generation,&r)==PET_RUNTIME_OK&&r.sequence==1&&r.result==PET_PROGRAM_COMMITTED);
    assert(hosts[i].runtime.state.cells[0]==(int)i+1);}
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
}
static void test_report_backpressure_cannot_starve_other_groups(void){
  setup(4,false);for(unsigned i=0;i<4;i++){enqueue(i,1);enqueue(i,2);}
  for(unsigned turn=0;turn<120;turn++){
    pet_program_schedule_diagnostic_t d=step(100);assert(d.schedule.slot==turn%4);
    unsigned i=d.schedule.slot;
    if(i&&hosts[i].runtime.report_pending){pet_program_report_t r;assert(pet_program_group_take_report(&groups[i],groups[i].generation,&r)==PET_RUNTIME_OK);}
  }
  assert(hosts[0].runtime.report_pending&&hosts[0].runtime.state.cells[0]==1&&hosts[0].runtime.queued==1);
  for(unsigned i=1;i<4;i++)assert(hosts[i].runtime.state.cells[0]==3&&!hosts[i].runtime.queued);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
  pet_program_report_t r;assert(pet_program_group_take_report(&groups[0],groups[0].generation,&r)==PET_RUNTIME_OK&&r.result==PET_PROGRAM_COMMITTED&&r.sequence==1);
}
static void test_ingress_alternates_and_preserves_backpressure(void){
  setup(1,true);for(unsigned i=0;i<4;i++)enqueue(0,1);
  pet_program_event_t e={1,{3}};pet_event_diagnostic_t bd;
  assert(pet_event_bus_publish(&bus,publisher,1,1,&e,bus_current,NULL,&bd)==PET_EVENT_OK);
  pet_program_schedule_diagnostic_t d=step(100);
  assert(d.ingress==PET_SCHED_TIMER&&d.timer_result==PET_TIMERS_BACKPRESSURE&&groups[0].sequence==4&&timers[0].active_mask);
  d=step(100);assert(d.ingress==PET_SCHED_BUS&&d.bus_result==PET_EVENT_DELIVERED&&groups[0].sequence==5);
  d=step(100);assert(d.ingress==PET_SCHED_TIMER&&d.timer_result==PET_TIMERS_BACKPRESSURE&&groups[0].sequence==5);
  d=step(100);assert(d.ingress==PET_SCHED_BUS&&d.bus_result==PET_EVENT_IDLE);
  for(unsigned turn=4;turn<40;turn++){
    uint64_t before=groups[0].sequence;d=step(100);assert(groups[0].sequence-before<=1);
    assert(d.ingress==(turn%2?PET_SCHED_BUS:PET_SCHED_TIMER));
    if(hosts[0].runtime.report_pending){pet_program_report_t r;assert(pet_program_group_take_report(&groups[0],groups[0].generation,&r)==PET_RUNTIME_OK);}
  }
  assert(timers[0].accepted==1&&groups[0].sequence==6&&bus.slots[receivers[0].slot].last_delivery==5);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED&&timers[0].cancelled&&!bus.slots[receivers[0].slot].live&&bus.slots[publisher.slot].live);
}
static void test_capacity_unique_attachment_and_output_preservation(void){
  setup(4,false);prepare(4,false);pet_program_schedule_t out={0};
  pet_program_scheduler_t before=scheduler;
  assert(pet_program_scheduler_add(&scheduler,&groups[4],groups[4].generation,&out)==PET_SCHED_FULL&&!out.serial&&!out.scheduler);
  assert(!memcmp(&before,&scheduler,sizeof(before))&&!groups[4].scheduler_owner);
  assert(pet_program_scheduler_add(&other,&groups[0],groups[0].generation,&out)==PET_SCHED_INVALID&&!out.serial);
  out.serial=99;assert(pet_program_scheduler_add(&other,&groups[4],groups[4].generation,&out)==PET_SCHED_INVALID&&out.serial==99);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
  assert(pet_program_group_cancel(&groups[4],groups[4].generation)==PET_RUNTIME_OK);
}
static void test_exact_registration_after_slot_reuse(void){
  setup(4,false);prepare(4,false);enqueue(0,7);
  assert(pet_program_scheduler_remove(&scheduler,tokens[0])==PET_SCHED_OK&&admissions[0].finishes==1);
  assert(pet_program_scheduler_add(&scheduler,&groups[4],groups[4].generation,&tokens[4])==PET_SCHED_OK&&tokens[4].slot==0&&tokens[4].serial>tokens[0].serial);
  assert(pet_program_scheduler_remove(&scheduler,tokens[0])==PET_SCHED_STALE&&admissions[4].held&&hosts[4].runtime.active);
  assert(pet_program_scheduler_remove(&other,tokens[4])==PET_SCHED_STALE&&admissions[4].held);
  assert(!pet_program_scheduler_init(&scheduler));pet_program_schedule_t out={0};
  assert(pet_program_scheduler_add(&other,&groups[0],groups[0].generation,&out)==PET_SCHED_INVALID);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
}
static void test_replaced_native_host_is_not_cancelled_by_old_slot(void){
  setup(1,false);uint64_t old=groups[0].generation;
  assert(pet_program_group_cancel(&groups[0],old)==PET_RUNTIME_OK);
  uint64_t next=0;assert(pet_program_host_start(&hosts[0],2,NULL,&next)==PET_RUNTIME_OK&&next>old);
  assert(pet_program_group_attach(&replacement,&hosts[0],NULL,NULL,(pet_event_registration_t){0}));
  pet_program_schedule_diagnostic_t d=step(100);assert(d.removed&&!replacement.stopped&&hosts[0].runtime.active&&admissions[0].held&&admissions[0].finishes==1);
  pet_program_schedule_t fresh={0};assert(pet_program_scheduler_add(&scheduler,&replacement,next,&fresh)==PET_SCHED_OK&&fresh.serial>tokens[0].serial);
  assert(pet_program_scheduler_remove(&scheduler,tokens[0])==PET_SCHED_STALE&&admissions[0].held);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED&&admissions[0].finishes==2);
}
static void test_busy_retirement_latches_all_ingress_stop(void){
  setup(4,false);for(unsigned i=0;i<4;i++)enqueue(i,1);
  hosts[0].inside=true;assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPING&&scheduler.slots[0].group&&admissions[0].held);
  for(unsigned i=1;i<4;i++)assert(groups[i].stopped&&!admissions[i].held&&!scheduler.slots[i].group);
  hosts[0].inside=false;pet_program_event_t event={1,{9}};uint64_t seq=99;
  assert(pet_program_group_enqueue(&groups[0],groups[0].generation,&event,&seq)==PET_RUNTIME_STOPPED&&seq==99&&!admissions[0].held);
  pet_program_schedule_diagnostic_t d={0};assert(pet_program_scheduler_step(&scheduler,1000,&d)==PET_SCHED_STOPPED&&d.stopped&&!d.pump_attempted);
  for(unsigned i=0;i<4;i++)assert(!hosts[i].runtime.state.revision&&admissions[i].finishes==1);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
}
static void test_backwards_clock_never_resumes_or_pumps(void){
  setup(3,true);for(unsigned i=0;i<3;i++)enqueue(i,1);
  (void)step(100);pet_program_schedule_diagnostic_t d={0};
  assert(pet_program_scheduler_step(&scheduler,99,&d)==PET_SCHED_CLOCK_ERROR&&d.stopped&&!d.pump_attempted);
  assert(pet_program_scheduler_step(&scheduler,UINT64_MAX,&d)==PET_SCHED_STOPPED&&!d.pump_attempted);
  for(unsigned i=0;i<3;i++)assert(groups[i].retired&&timers[i].cancelled&&admissions[i].finishes==1&&!hosts[i].runtime.state.revision);
}
static void test_audio_loss_isolated_no_resume(void){
  setup(4,false);for(unsigned i=0;i<4;i++)enqueue(i,1);admissions[0].busy=true;
  pet_program_schedule_diagnostic_t d=step(100);assert(d.schedule.slot==0&&d.removed&&d.stopped&&d.pump_result==PET_RUNTIME_DENIED);
  admissions[0].busy=false;
  for(unsigned i=1;i<=3;i++){d=step(100);assert(d.schedule.slot==i&&!d.removed&&d.pump_result==PET_RUNTIME_YIELDED);}
  d=step(100);assert(d.schedule.slot==1&&admissions[0].finishes==1&&!hosts[0].runtime.state.revision);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
}
static void test_alias_and_wrong_generation_do_not_mutate(void){
  setup(1,true);prepare(1,false);pet_program_scheduler_t before=scheduler;
  assert(pet_program_scheduler_step(&scheduler,100,(void*)&scheduler)==PET_SCHED_INVALID);
  assert(pet_program_scheduler_step(&scheduler,100,(void*)&hosts[0])==PET_SCHED_INVALID);
  assert(pet_program_scheduler_step(&scheduler,100,(void*)&timers[0])==PET_SCHED_INVALID);
  assert(pet_program_scheduler_step(&scheduler,100,(void*)&bus)==PET_SCHED_INVALID);
  assert(pet_program_scheduler_add(&scheduler,&groups[1],groups[1].generation,(void*)&hosts[0])==PET_SCHED_INVALID);
  assert(pet_program_scheduler_add(&scheduler,&groups[1],groups[1].generation+1,&tokens[1])==PET_SCHED_STALE&&!tokens[1].serial);
  assert(!memcmp(&before,&scheduler,sizeof(before))&&hosts[1].runtime.active&&admissions[1].held);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
  assert(pet_program_group_cancel(&groups[1],groups[1].generation)==PET_RUNTIME_OK);
}
static void test_backend_callback_reentry(void){
  setup(1,false);prepare(4,false);enqueue(0,1);admissions[0].reenter=true;
  for(unsigned i=0;i<10;i++)(void)step(100);
  assert(reentries);assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
  admissions[0].reenter=false;assert(pet_program_group_cancel(&groups[4],groups[4].generation)==PET_RUNTIME_OK);
}
static void test_registration_callback_reentry(void){
  setup(0,false);prepare(0,false);prepare(4,false);admissions[0].reenter=true;
  assert(pet_program_scheduler_add(&scheduler,&groups[0],groups[0].generation,&tokens[0])==PET_SCHED_OK&&reentries);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
  admissions[0].reenter=false;assert(pet_program_group_cancel(&groups[4],groups[4].generation)==PET_RUNTIME_OK);
}
static void test_busy_group_gets_no_extra_turns(void){
  setup(4,false);for(unsigned i=0;i<4;i++)enqueue(i,1);hosts[0].inside=true;
  pet_program_schedule_diagnostic_t d=step(100);assert(d.schedule.slot==0&&d.pump_result==PET_RUNTIME_BUSY&&!d.removed);
  for(unsigned i=1;i<4;i++){d=step(100);assert(d.schedule.slot==i&&hosts[i].runtime.inflight==1);}
  hosts[0].inside=false;d=step(100);assert(d.schedule.slot==0&&hosts[0].runtime.inflight==1);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
}
static void test_uncertain_retirement_keeps_occupied_slot(void){
  setup(1,false);uint64_t g=groups[0].generation;
  /* Deliberately break the private parent invariant: stale by itself does not
   * prove that any physical retirement happened. Never infer a free slot. */
  groups[0].generation++;
  assert(pet_program_scheduler_remove(&scheduler,tokens[0])==PET_SCHED_BUSY);
  assert(scheduler.slots[0].group==&groups[0]&&!groups[0].retired&&admissions[0].held&&!admissions[0].finishes);
  groups[0].generation=g;
  assert(pet_program_scheduler_remove(&scheduler,tokens[0])==PET_SCHED_OK&&!scheduler.slots[0].group&&!admissions[0].held);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
}
static void test_remove_busy_only_retires_no_resume(void){
  setup(2,false);for(unsigned i=0;i<2;i++){
    enqueue(i,1);hosts[i].inside=true;
    assert(pet_program_scheduler_remove(&scheduler,tokens[i])==PET_SCHED_BUSY);
    assert(scheduler.slots[i].retiring&&groups[i].stop_requested&&admissions[i].held&&!groups[i].retired);
  }
  hosts[0].inside=false;pet_program_schedule_diagnostic_t d=step(100);
  assert(d.schedule.slot==0&&d.removed&&d.stopped&&!d.pump_attempted&&d.ingress==PET_SCHED_NO_INGRESS);
  assert(!hosts[0].runtime.state.revision&&!hosts[0].runtime.inflight&&admissions[0].finishes==1);
  hosts[1].inside=false;pet_program_event_t event={1,{9}};uint64_t seq=99;
  assert(pet_program_group_enqueue(&groups[1],groups[1].generation,&event,&seq)==PET_RUNTIME_STOPPED&&seq==99);
  assert(!hosts[1].runtime.state.revision&&admissions[1].finishes==1);
  d=step(100);assert(d.schedule.slot==1&&d.removed&&!d.pump_attempted);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
}
static void test_registration_exhaustion_never_wraps(void){
  assert(pet_program_scheduler_init(&scheduler));prepare(0,false);prepare(1,false);scheduler.next_serial=UINT64_MAX-1;
  assert(pet_program_scheduler_add(&scheduler,&groups[0],groups[0].generation,&tokens[0])==PET_SCHED_OK&&tokens[0].serial==UINT64_MAX);
  assert(pet_program_scheduler_remove(&scheduler,tokens[0])==PET_SCHED_OK);
  pet_program_scheduler_t before=scheduler;
  assert(pet_program_scheduler_add(&scheduler,&groups[1],groups[1].generation,&tokens[1])==PET_SCHED_EXHAUSTED&&!tokens[1].serial&&!groups[1].scheduler_owner);
  assert(!memcmp(&before,&scheduler,sizeof(before)));
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
  assert(pet_program_group_cancel(&groups[1],groups[1].generation)==PET_RUNTIME_OK);
}
static void test_many_turns_nonvacuous_fair_progress(void){
  setup(4,false);unsigned accepted[4]={0},committed[4]={0};
  for(unsigned turn=0;turn<10000;turn++){
    unsigned i=turn%4;pet_program_event_t event={1,{1}};uint64_t seq=0;
    pet_program_runtime_result_t r=pet_program_group_enqueue(&groups[i],groups[i].generation,&event,&seq);
    assert(r==PET_RUNTIME_OK||r==PET_RUNTIME_QUEUE_FULL);if(r==PET_RUNTIME_OK)accepted[i]++;
    pet_program_schedule_diagnostic_t d=step(100+turn);assert(d.schedule.slot==i&&!d.removed);
    if(hosts[i].runtime.report_pending&&turn%7){pet_program_report_t report;
      assert(pet_program_group_take_report(&groups[i],groups[i].generation,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_COMMITTED);committed[i]++;}
  }
  for(unsigned i=0;i<4;i++)assert(committed[i]>300&&accepted[i]>=committed[i]&&hosts[i].runtime.state.cells[0]>300);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
}
static void test_copied_scheduler_cannot_stop_original_owners(void){
  setup(3,true);for(unsigned i=0;i<3;i++)enqueue(i,1);
  pet_program_scheduler_t copy=scheduler,copy_before=copy,original=scheduler;
  pet_program_schedule_diagnostic_t diagnostic={.generation=999};
  assert(pet_program_scheduler_stop(&copy)==PET_SCHED_INVALID);
  assert(pet_program_scheduler_remove(&copy,tokens[0])==PET_SCHED_INVALID);
  assert(pet_program_scheduler_step(&copy,100,&diagnostic)==PET_SCHED_INVALID&&diagnostic.generation==999);
  assert(!memcmp(&copy,&copy_before,sizeof(copy))&&!memcmp(&scheduler,&original,sizeof(scheduler)));
  for(unsigned i=0;i<3;i++)assert(admissions[i].live&&admissions[i].held&&!admissions[i].finishes&&!timers[i].cancelled&&!groups[i].stopped);
  pet_program_schedule_diagnostic_t d=step(100);assert(d.schedule.slot==0&&d.pump_attempted);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
  for(unsigned i=0;i<3;i++)assert(admissions[i].finishes==1&&timers[i].cancelled);
}
static void test_explicit_rearm_preserves_ticket_uniqueness(void){
  setup(1,false);pet_program_schedule_t old=tokens[0];
  assert(pet_program_scheduler_rearm(&scheduler)==PET_SCHED_INVALID);
  hosts[0].inside=true;
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPING);
  assert(pet_program_scheduler_rearm(&scheduler)==PET_SCHED_BUSY&&scheduler.stopped&&admissions[0].held);
  hosts[0].inside=false;
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED);
  uint64_t serial=scheduler.next_serial;
  assert(pet_program_scheduler_rearm(&scheduler)==PET_SCHED_OK&&!scheduler.stopped&&scheduler.next_serial==serial);
  prepare(1,false);
  assert(pet_program_scheduler_add(&scheduler,&groups[1],groups[1].generation,&tokens[1])==PET_SCHED_OK&&tokens[1].serial>old.serial);
  assert(pet_program_scheduler_remove(&scheduler,old)==PET_SCHED_STALE&&admissions[1].held);
  enqueue(1,3);pet_program_schedule_diagnostic_t d=step(1);assert(d.pump_attempted);
  assert(pet_program_scheduler_stop(&scheduler)==PET_SCHED_STOPPED&&!admissions[1].held);
  pet_program_scheduler_t copy=scheduler,copy_before=copy;
  assert(pet_program_scheduler_rearm(&copy)==PET_SCHED_INVALID&&!memcmp(&copy,&copy_before,sizeof(copy)));
  scheduler.next_serial=UINT64_MAX;
  assert(pet_program_scheduler_rearm(&scheduler)==PET_SCHED_EXHAUSTED&&scheduler.stopped);
}
int main(int argc,char**argv){
  assert(argc==2);unsigned c=(unsigned)strtoul(argv[1],NULL,10);
  void(*cases[])(void)={test_round_robin_and_single_quantum,test_report_backpressure_cannot_starve_other_groups,
    test_ingress_alternates_and_preserves_backpressure,test_capacity_unique_attachment_and_output_preservation,
    test_exact_registration_after_slot_reuse,test_replaced_native_host_is_not_cancelled_by_old_slot,
    test_busy_retirement_latches_all_ingress_stop,test_backwards_clock_never_resumes_or_pumps,
    test_audio_loss_isolated_no_resume,test_alias_and_wrong_generation_do_not_mutate,
    test_backend_callback_reentry,test_registration_exhaustion_never_wraps,test_many_turns_nonvacuous_fair_progress,
    test_registration_callback_reentry,test_busy_group_gets_no_extra_turns,test_uncertain_retirement_keeps_occupied_slot,
    test_remove_busy_only_retires_no_resume,test_copied_scheduler_cannot_stop_original_owners,
    test_explicit_rearm_preserves_ticket_uniqueness};
  assert(c<sizeof(cases)/sizeof(cases[0]));cases[c]();
  printf("{\"ok\":true,\"schedulerBytes\":%zu}\n",sizeof(scheduler));return 0;
}
