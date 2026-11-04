/* Real timer -> host -> runtime, synthetic broker admission only. */
#define main program_host_cases_main
#include "p4_program_host_test.c"
#undef main
#include "pet_p4_program_timers.h"
static pet_program_runtime_result_t host_enqueue_timer(void *context,uint64_t generation,uint64_t sequence,const pet_program_event_t *event){
  return pet_program_host_enqueue(context,generation,sequence,event);
}
int main(void){
  setup();uint64_t generation=start();
  pet_program_timers_t timers={0};
  pet_program_timer_spec_t specs[2]={
    {.id=1,.delay_ms=0,.interval_ms=100,.event={1,{3}}},
    {.id=2,.delay_ms=0,.interval_ms=100,.event={1,{7}}}
  };
  assert(pet_program_timers_start(&timers,generation,0,specs,2)==PET_TIMERS_OK);
  pet_program_timer_diagnostic_t d;
  assert(pet_program_timers_tick(&timers,generation,1000,1,host_enqueue_timer,&host,&d)==PET_TIMERS_ENQUEUED&&d.coalesced==10);
  assert(pet_program_timers_tick(&timers,generation,1000,2,host_enqueue_timer,&host,&d)==PET_TIMERS_ENQUEUED&&d.timer_id==2);
  assert(host.runtime.state.revision==0&&host.runtime.queued==2);
  complete(generation);pet_program_report_t report;
  assert(pet_program_host_take_report(&host,generation,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_COMMITTED&&report.sequence==1);
  assert(host.runtime.state.cells[0]==3&&host.runtime.queued==1);
  mock.busy=true;unsigned finishes=mock.finishes;
  assert(pet_program_timers_tick(&timers,generation,1100,3,host_enqueue_timer,&host,&d)==PET_TIMERS_STOPPED);
  assert(!d.accepted&&!host.runtime.active&&!host.runtime.queued&&!mock.held&&mock.finishes==finishes+1);
  mock.busy=false;
  assert(pet_program_timers_tick(&timers,generation,2000,3,host_enqueue_timer,&host,&d)==PET_TIMERS_STOPPED);
  pet_program_snapshot_t seed;assert(pet_program_host_snapshot(&host,generation,&seed)==PET_RUNTIME_OK&&seed.values[0]==3);
  uint64_t next=0;assert(pet_program_host_start(&host,2,&seed,&next)==PET_RUNTIME_OK&&next>generation);
  assert(pet_program_timers_tick(&timers,next,2000,1,host_enqueue_timer,&host,&d)==PET_TIMERS_STALE);
  assert(!host.runtime.queued&&host.runtime.state.cells[0]==3);
  pet_program_timers_t new_schedule={0};
  assert(pet_program_timers_start(&new_schedule,next,2000,specs,2)==PET_TIMERS_OK);
  assert(pet_program_timers_tick(&new_schedule,next,2000,1,host_enqueue_timer,&host,&d)==PET_TIMERS_ENQUEUED&&!d.coalesced);
  assert(pet_program_timers_cancel(&new_schedule,next)==PET_TIMERS_OK);
  assert(pet_program_host_cancel(&host,next)==PET_RUNTIME_OK&&!host.runtime.queued&&!mock.held);
  assert(pet_program_timers_tick(&new_schedule,next,2200,2,host_enqueue_timer,&host,&d)==PET_TIMERS_STOPPED);
  puts("timer host lifetime passed");return 0;
}
