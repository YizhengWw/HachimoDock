/* Actual host/runtime/image/core/timer/bus/group, synthetic admission only.
 * No real broker quota patch, package, scheduler, IO, audio or device contact. */
#include "pet_p4_program_group.h"
#include <limits.h>
#define main existing_host_unit_main
#include "p4_program_host_test.c"
#undef main

static pet_program_group_t group,replacement;
static pet_program_timers_t timers;
static pet_event_bus_t bus;
static pet_event_registration_t publisher,receiver;
static pet_resource_owner_t publisher_owner;
static bool group_reenter;
static unsigned group_reentries;
static bool bus_current(void *context,pet_resource_owner_t owner){
  (void)context;
  return same_owner(owner,publisher_owner)||(mock.live&&same_owner(owner,mock.owner));
}
static bool group_audio(void *context){
  if(group_reenter){
    pet_program_event_t event={1,{99}};uint64_t sequence=99;
    pet_program_timer_diagnostic_t td={.timer_id=99};pet_event_diagnostic_t bd={.remaining=99};
    pet_program_snapshot_t snapshot;pet_program_report_t report;pet_program_ui_snapshot_t ui;
    int32_t base[8]={0};uint64_t g=group.generation;
    assert(pet_program_group_enqueue(&group,g,&event,&sequence)==PET_RUNTIME_BUSY&&sequence==99);
    assert(pet_program_group_pump(&group,g)==PET_RUNTIME_BUSY);
    assert(pet_program_group_check_current(&group,g)==PET_RUNTIME_BUSY);
    assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_BUSY);
    assert(pet_program_group_tick(&group,g,100,&td)==PET_TIMERS_BUSY&&td.timer_id==99);
    assert(pet_program_group_deliver(&group,g,&bd)==PET_EVENT_BUSY&&bd.remaining==99);
    assert(pet_program_group_snapshot(&group,g,&snapshot)==PET_RUNTIME_BUSY);
    assert(pet_program_group_take_report(&group,g,&report)==PET_RUNTIME_BUSY);
    assert(pet_program_group_project(&group,g,base,&ui)==PET_RUNTIME_BUSY);
    assert(!pet_program_group_attach(&group,&host,&timers,&bus,receiver));
    group_reentries++;
  }
  return audio_busy(context);
}
static uint64_t group_prepare(bool children){
  mock.enabled=true;
  image=(pet_program_image_t){.schema=42,.count=7,.fuel=64,.cells=1,.valid=true};
  memcpy(image.code,accumulator,sizeof(accumulator));
  pet_program_host_backend_t b={&mock,group_audio,owner_open,acquire,current,revoke,finish};
  assert(pet_program_host_init(&host,&b));
  pet_program_ui_projection_t p={.schema=42,.variables=1,.maximum={100000}};
  memset(p.target,PET_PROGRAM_UI_UNBOUND,sizeof(p.target));p.target[0]=0;
  uint64_t g=0;assert(pet_program_host_start_image_with_projection(&host,&image,2,NULL,&p,&g)==PET_RUNTIME_OK);
  if(children){
    pet_program_timer_spec_t spec={.id=1,.delay_ms=0,.interval_ms=100,.event={1,{2}}};
    assert(pet_program_timers_start(&timers,g,100,&spec,1)==PET_TIMERS_OK);
    assert(pet_event_bus_init(&bus));publisher_owner=(pet_resource_owner_t){&identity,1000,1};
    assert(pet_event_bus_open(&bus,publisher_owner,1,0,bus_current,NULL,&publisher)==PET_EVENT_OK);
    assert(pet_event_bus_open(&bus,host.owner,0,1,bus_current,NULL,&receiver)==PET_EVENT_OK);
  }
  return g;
}
static uint64_t group_setup(bool children){
  uint64_t g=group_prepare(children);
  assert(pet_program_group_attach(&group,&host,children?&timers:NULL,children?&bus:NULL,receiver));
  assert(host.group_owner==&group);
  assert(!pet_program_group_attach(&group,&host,NULL,NULL,(pet_event_registration_t){0}));return g;
}
static void publish_event(int value){
  pet_program_event_t event={1,{value}};pet_event_diagnostic_t d;
  assert(pet_event_bus_publish(&bus,publisher,1,1,&event,bus_current,NULL,&d)==PET_EVENT_OK&&d.recipients==1);
}
static void group_complete(uint64_t g){
  for(unsigned i=0;i<30;i++){
    unsigned before=host.runtime.workspace.used_fuel;
    pet_program_runtime_result_t r=pet_program_group_pump(&group,g);
    if(r==PET_RUNTIME_COMPLETE)return;
    assert(r==PET_RUNTIME_YIELDED&&host.runtime.workspace.used_fuel-before<=2);
  }
  assert(!"group completion must be cooperatively bounded");
}
static void test_shared_sequence_and_committed_projection(void){
  uint64_t g=group_setup(true),seq=99;pet_program_event_t e={1,{1}};
  pet_program_timer_diagnostic_t td;pet_event_diagnostic_t bd;
  assert(pet_program_group_enqueue(&group,g,&e,&seq)==PET_RUNTIME_OK&&seq==1);
  assert(pet_program_group_tick(&group,g,100,&td)==PET_TIMERS_ENQUEUED&&td.accepted&&group.sequence==2);
  publish_event(3);assert(pet_program_group_deliver(&group,g,&bd)==PET_EVENT_DELIVERED&&bd.consumed&&group.sequence==3);
  for(unsigned i=1;i<=3;i++){
    assert(pet_program_group_pump(&group,g)==PET_RUNTIME_YIELDED);
    int32_t base[8]={0};pet_program_ui_snapshot_t ui;
    assert(pet_program_group_project(&group,g,base,&ui)==PET_RUNTIME_OK&&ui.revision==i-1);
    group_complete(g);pet_program_report_t report;
    assert(pet_program_group_take_report(&group,g,&report)==PET_RUNTIME_OK&&report.sequence==i&&report.result==PET_PROGRAM_COMMITTED);
  }
  pet_program_snapshot_t snapshot;assert(pet_program_group_snapshot(&group,g,&snapshot)==PET_RUNTIME_OK&&snapshot.values[0]==6);
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK&&group.stopped&&group.retired);
  assert(timers.cancelled&&!bus.slots[receiver.slot].live&&bus.slots[publisher.slot].live&&mock.finishes==1);
  assert(pet_program_group_snapshot(&group,g,&snapshot)==PET_RUNTIME_OK&&snapshot.values[0]==6);
}
static void test_backpressure_and_one_pump(void){
  uint64_t g=group_setup(true),seq=0;pet_program_event_t e={1,{1}};
  for(unsigned i=1;i<=4;i++)assert(pet_program_group_enqueue(&group,g,&e,&seq)==PET_RUNTIME_OK&&seq==i);
  seq=99;assert(pet_program_group_enqueue(&group,g,&e,&seq)==PET_RUNTIME_QUEUE_FULL&&seq==99&&group.sequence==4);
  pet_program_timer_diagnostic_t td;pet_event_diagnostic_t bd;
  assert(pet_program_group_tick(&group,g,100,&td)==PET_TIMERS_BACKPRESSURE&&!td.accepted&&group.sequence==4);
  publish_event(3);assert(pet_program_group_deliver(&group,g,&bd)==PET_EVENT_BACKPRESSURE&&!bd.consumed&&group.sequence==4);
  assert(pet_program_group_pump(&group,g)==PET_RUNTIME_YIELDED&&host.runtime.inflight==1&&host.runtime.queued==3&&!host.runtime.workspace.used_fuel);
  assert(pet_program_group_tick(&group,g,100,&td)==PET_TIMERS_ENQUEUED&&group.sequence==5);
  group_complete(g);pet_program_report_t report;assert(pet_program_group_take_report(&group,g,&report)==PET_RUNTIME_OK&&report.sequence==1);
  assert(pet_program_group_pump(&group,g)==PET_RUNTIME_YIELDED&&host.runtime.inflight==2);
  assert(pet_program_group_deliver(&group,g,&bd)==PET_EVENT_DELIVERED&&group.sequence==6&&bus.slots[receiver.slot].last_delivery==6);
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK);
  assert(pet_program_group_take_report(&group,g,&report)==PET_RUNTIME_OK&&report.sequence==2&&report.result==PET_PROGRAM_CANCELLED);
  assert(!host.runtime.queued&&host.runtime.state.cells[0]==1&&mock.finishes==1);
}
static void test_manual_stop_no_resume_and_report_preservation(void){
  uint64_t g=group_setup(true),seq=0;pet_program_event_t e={1,{4}};
  assert(pet_program_group_enqueue(&group,g,&e,&seq)==PET_RUNTIME_OK);group_complete(g);
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK);
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK&&mock.finishes==1);
  seq=99;pet_program_timer_diagnostic_t td={.timer_id=99};pet_event_diagnostic_t bd={.remaining=99};
  assert(pet_program_group_enqueue(&group,g,&e,&seq)==PET_RUNTIME_STOPPED&&seq==99);
  assert(pet_program_group_tick(&group,g,100000,&td)==PET_TIMERS_STOPPED&&td.timer_id==99);
  assert(pet_program_group_deliver(&group,g,&bd)==PET_EVENT_STOPPED&&bd.remaining==99);
  assert(pet_program_group_pump(&group,g)==PET_RUNTIME_STOPPED);
  pet_program_report_t r;assert(pet_program_group_take_report(&group,g,&r)==PET_RUNTIME_OK&&r.result==PET_PROGRAM_COMMITTED&&r.sequence==1);
  pet_program_snapshot_t s;assert(pet_program_group_snapshot(&group,g,&s)==PET_RUNTIME_OK&&s.values[0]==4);
}
static void test_audio_revoke_cancels_all_children(void){
  uint64_t g=group_setup(true),seq=99;publish_event(3);mock.busy=true;
  pet_program_event_t e={1,{1}};
  assert(pet_program_group_enqueue(&group,g,&e,&seq)==PET_RUNTIME_DENIED&&seq==99);
  assert(group.stopped&&group.retired&&timers.cancelled&&!bus.slots[receiver.slot].live&&mock.finishes==1);
  mock.busy=false;assert(pet_program_group_pump(&group,g)==PET_RUNTIME_STOPPED&&!host.runtime.state.revision);
}
static void test_revoke_inside_timer_enqueue(void){
  uint64_t g=group_setup(true);mock.deny_at=mock.checks+2;
  pet_program_timer_diagnostic_t td;
  assert(pet_program_group_tick(&group,g,100,&td)==PET_TIMERS_STOPPED&&td.offered&&!td.accepted);
  assert(!group.sequence&&group.retired&&timers.cancelled&&!bus.slots[receiver.slot].live&&mock.finishes==1);
  assert(!host.runtime.state.revision&&!host.runtime.queued);
}
static void test_wrong_generation_and_aliases_preserve(void){
  uint64_t g=group_setup(true),seq=99;pet_program_event_t e={1,{1}};
  pet_program_timer_diagnostic_t td={.timer_id=99};pet_event_diagnostic_t bd={.remaining=99};
  pet_program_host_t old_host=host;pet_program_timers_t old_timers=timers;pet_event_bus_t old_bus=bus;
  pet_program_group_t old_group=group;
  assert(pet_program_group_enqueue(&group,g+1,&e,&seq)==PET_RUNTIME_STALE&&seq==99);
  assert(pet_program_group_tick(&group,g+1,100,&td)==PET_TIMERS_STALE&&td.timer_id==99);
  assert(pet_program_group_deliver(&group,g+1,&bd)==PET_EVENT_STALE&&bd.remaining==99);
  assert(pet_program_group_cancel(&group,g+1)==PET_RUNTIME_STALE);
  assert(pet_program_group_enqueue(&group,g,&e,&group.sequence)==PET_RUNTIME_INVALID);
  assert(pet_program_group_enqueue(&group,g,(void*)&bus.slots[0],&seq)==PET_RUNTIME_INVALID);
  assert(pet_program_group_tick(&group,g,100,(void*)&host.staged)==PET_TIMERS_INVALID);
  assert(pet_program_group_deliver(&group,g,(void*)&timers)==PET_EVENT_INVALID);
  assert(pet_program_group_snapshot(&group,g,(void*)&group)==PET_RUNTIME_INVALID);
  assert(pet_program_group_take_report(&group,g,(void*)&bus)==PET_RUNTIME_INVALID);
  pet_program_ui_snapshot_t ui;assert(pet_program_group_project(&group,g,(void*)&timers,&ui)==PET_RUNTIME_INVALID);
  assert(!memcmp(&old_host,&host,sizeof(host))&&!memcmp(&old_timers,&timers,sizeof(timers))
    &&!memcmp(&old_bus,&bus,sizeof(bus))&&!memcmp(&old_group,&group,sizeof(group)));
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK);
}
static void test_reentry_all_wrappers(void){
  uint64_t g=group_setup(true),seq=0;mock.reenter=true;group_reenter=true;
  pet_program_event_t e={1,{1}};
  assert(pet_program_group_enqueue(&group,g,&e,&seq)==PET_RUNTIME_OK&&seq==1);
  group_complete(g);assert(group_reentries&&mock.reentries);
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK);
}
static void test_new_owner_never_cancelled_by_old_group(void){
  uint64_t g=group_setup(true);assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK);
  pet_program_snapshot_t seed;assert(pet_program_group_snapshot(&group,g,&seed)==PET_RUNTIME_OK);
  uint64_t next=0;assert(pet_program_host_start(&host,2,&seed,&next)==PET_RUNTIME_OK&&next>g);
  memset(&timers,0,sizeof(timers));pet_program_timer_spec_t spec={.id=1,.event={1,{2}}};
  assert(pet_program_timers_start(&timers,next,100,&spec,1)==PET_TIMERS_OK);
  pet_event_registration_t fresh={0};assert(pet_event_bus_open(&bus,host.owner,0,1,bus_current,NULL,&fresh)==PET_EVENT_OK&&fresh.serial!=receiver.serial);
  assert(pet_program_group_attach(&replacement,&host,&timers,&bus,fresh));
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_STALE&&group.retired);
  assert(host.runtime.active&&host.bound&&!timers.cancelled&&bus.slots[fresh.slot].live&&mock.finishes==1);
  pet_program_snapshot_t kept=seed;assert(pet_program_group_snapshot(&group,g,&seed)==PET_RUNTIME_STALE&&!memcmp(&seed,&kept,sizeof(seed)));
  assert(pet_program_group_cancel(&replacement,next)==PET_RUNTIME_OK&&mock.finishes==2);
}
static void test_clock_and_sequence_faults_stop_group(void){
  uint64_t g=group_setup(true);pet_program_timer_diagnostic_t td;
  assert(pet_program_group_tick(&group,g,100,&td)==PET_TIMERS_ENQUEUED);
  assert(pet_program_group_tick(&group,g,99,&td)==PET_TIMERS_CLOCK_ERROR&&group.retired&&!host.runtime.queued);
  assert(mock.finishes==1&&timers.cancelled&&!bus.slots[receiver.slot].live);
}
static void test_exhaustion_and_optional_children(void){
  uint64_t g=group_setup(false);pet_program_timer_diagnostic_t td;pet_event_diagnostic_t bd;
  assert(pet_program_group_tick(&group,g,100,&td)==PET_TIMERS_INVALID&&!group.stopped);
  assert(pet_program_group_deliver(&group,g,&bd)==PET_EVENT_INVALID&&!group.stopped);
  group.sequence=UINT64_MAX;uint64_t seq=99;pet_program_event_t e={1,{1}};
  assert(pet_program_group_enqueue(&group,g,&e,&seq)==PET_RUNTIME_BAD_STATE&&seq==99&&group.retired);
  assert(!host.runtime.queued&&mock.finishes==1);
}
static void test_attachment_rejects_mismatched_ownership(void){
  uint64_t g=group_prepare(true);pet_program_group_t out={0},before=out;
  assert(!pet_program_group_attach(&out,&host,&timers,&bus,publisher));
  timers.generation=g+1;assert(!pet_program_group_attach(&out,&host,&timers,&bus,receiver));timers.generation=g;
  timers.last_sequence=1;assert(!pet_program_group_attach(&out,&host,&timers,&bus,receiver));timers.last_sequence=0;
  bus.slots[receiver.slot].last_delivery=1;assert(!pet_program_group_attach(&out,&host,&timers,&bus,receiver));bus.slots[receiver.slot].last_delivery=0;
  assert(!pet_program_group_attach((void*)&host.staged,&host,&timers,&bus,receiver));
  assert(!memcmp(&out,&before,sizeof(out))&&!host.group_owner&&mock.held);
  assert(pet_program_group_attach(&group,&host,&timers,&bus,receiver));
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK);
}
static void test_unique_attachment_survives_failed_restart(void){
  uint64_t g=group_setup(true);pet_program_group_t other={0},before=other;
  assert(!pet_program_group_attach(&other,&host,&timers,&bus,receiver));
  assert(!memcmp(&other,&before,sizeof(other))&&host.group_owner==&group);
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK);
  uint64_t next=99;mock.enabled=false;
  assert(pet_program_host_start(&host,2,NULL,&next)==PET_RUNTIME_DENIED&&next==99&&host.group_owner==&group);
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK);
  mock.enabled=true;
  assert(pet_program_host_start(&host,2,NULL,&next)==PET_RUNTIME_OK&&next>g&&!host.group_owner);
  assert(pet_program_group_attach(&other,&host,NULL,NULL,(pet_event_registration_t){0})&&host.group_owner==&other);
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_STALE&&host.runtime.active&&host.group_owner==&other);
  assert(pet_program_group_cancel(&other,next)==PET_RUNTIME_OK);
}
static void test_bypassed_sequencer_is_not_guessed_accepted(void){
  uint64_t g=group_setup(true),seq=99;pet_program_event_t e={1,{4}};
  /* A broken parent bypassed the transferred invocation path. Fail closed;
   * do not consume another ID, guess acceptance or offer a second paste/event. */
  assert(pet_program_host_enqueue(&host,g,9,&e)==PET_RUNTIME_OK);
  assert(pet_program_group_enqueue(&group,g,&e,&seq)==PET_RUNTIME_DUPLICATE&&seq==99);
  assert(group.retired&&!group.sequence&&timers.cancelled&&!bus.slots[receiver.slot].live);
  assert(!host.runtime.queued&&!host.runtime.state.revision&&mock.finishes==1);
}
static void test_copied_group_cannot_retire_original_children(void){
  uint64_t g=group_setup(true);publish_event(3);
  pet_program_group_t copy=group,copy_before=copy,original=group;
  pet_program_host_t before_host=host;pet_program_timers_t before_timers=timers;
  pet_event_bus_t before_bus=bus;unsigned checks=mock.checks;
  pet_program_timer_diagnostic_t td={.timer_id=99};pet_event_diagnostic_t bd={.remaining=99};
  assert(pet_program_group_cancel(&copy,g)==PET_RUNTIME_INVALID);
  assert(pet_program_group_check_current(&copy,g)==PET_RUNTIME_INVALID);
  assert(pet_program_group_pump(&copy,g)==PET_RUNTIME_INVALID);
  assert(pet_program_group_tick(&copy,g,100,&td)==PET_TIMERS_INVALID&&td.timer_id==99);
  assert(pet_program_group_deliver(&copy,g,&bd)==PET_EVENT_INVALID&&bd.remaining==99);
  assert(!memcmp(&copy,&copy_before,sizeof(copy))&&!memcmp(&group,&original,sizeof(group)));
  assert(!memcmp(&host,&before_host,sizeof(host))&&!memcmp(&timers,&before_timers,sizeof(timers))&&!memcmp(&bus,&before_bus,sizeof(bus)));
  assert(mock.live&&mock.held&&!mock.finishes&&mock.checks==checks);
  assert(pet_program_group_deliver(&group,g,&bd)==PET_EVENT_DELIVERED);group_complete(g);
  pet_program_report_t report;assert(pet_program_group_take_report(&group,g,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_COMMITTED);
  assert(pet_program_group_cancel(&group,g)==PET_RUNTIME_OK&&mock.finishes==1);
}
int main(int argc,char **argv){
  assert(argc==2);unsigned index=(unsigned)strtoul(argv[1],NULL,10);
  void(*cases[])(void)={test_shared_sequence_and_committed_projection,test_backpressure_and_one_pump,
    test_manual_stop_no_resume_and_report_preservation,test_audio_revoke_cancels_all_children,
    test_revoke_inside_timer_enqueue,test_wrong_generation_and_aliases_preserve,test_reentry_all_wrappers,
    test_new_owner_never_cancelled_by_old_group,test_clock_and_sequence_faults_stop_group,
    test_exhaustion_and_optional_children,test_attachment_rejects_mismatched_ownership,
    test_bypassed_sequencer_is_not_guessed_accepted,test_unique_attachment_survives_failed_restart,
    test_copied_group_cannot_retire_original_children};
  assert(index<sizeof(cases)/sizeof(cases[0]));cases[index]();
  printf("{\"ok\":true,\"groupBytes\":%zu}\n",sizeof(group));return 0;
}
