/* Shared bus -> real bounded program host, synthetic native admission only. */
#define main program_host_cases_main
#include "p4_program_host_test.c"
#undef main
#include "pet_p4_event_bus.h"
static pet_resource_owner_t publisher;
static bool bus_owner_current(void *context,pet_resource_owner_t owner){
  (void)context;
  return same_owner(owner,publisher)||(same_owner(owner,mock.owner)&&mock.live&&mock.held);
}
static pet_program_runtime_result_t bus_host_enqueue(void *context,pet_resource_owner_t owner,uint64_t sequence,const pet_event_message_t *message){
  assert(same_owner(owner,mock.owner)&&message->topic==1);
  return pet_program_host_enqueue(context,owner.generation,sequence,&message->event);
}
int main(void){
  setup();uint64_t generation=start();
  publisher=(pet_resource_owner_t){&identity,generation+100,1};
  pet_event_bus_t b={0};assert(pet_event_bus_init(&b));
  pet_event_registration_t pub={0},receiver={0};
  assert(pet_event_bus_open(&b,publisher,1,0,bus_owner_current,NULL,&pub)==PET_EVENT_OK);
  assert(pet_event_bus_open(&b,mock.owner,0,1,bus_owner_current,NULL,&receiver)==PET_EVENT_OK);
  pet_program_event_t e={1,{7}};pet_event_diagnostic_t d;
  assert(pet_event_bus_publish(&b,pub,1,1,&e,bus_owner_current,NULL,&d)==PET_EVENT_OK&&d.recipients==1);
  for(unsigned i=1;i<=4;i++)enqueue(generation,i,(int)i);
  assert(pet_event_bus_deliver(&b,receiver,5,bus_owner_current,NULL,bus_host_enqueue,&host,&d)==PET_EVENT_BACKPRESSURE&&!d.consumed&&d.remaining==1);
  complete(generation);pet_program_report_t report;
  assert(pet_program_host_take_report(&host,generation,&report)==PET_RUNTIME_OK&&report.sequence==1);
  assert(pet_event_bus_deliver(&b,receiver,5,bus_owner_current,NULL,bus_host_enqueue,&host,&d)==PET_EVENT_DELIVERED&&d.consumed&&!d.remaining);
  for(unsigned i=2;i<=5;i++){
    complete(generation);assert(pet_program_host_take_report(&host,generation,&report)==PET_RUNTIME_OK&&report.sequence==i&&report.result==PET_PROGRAM_COMMITTED);
  }
  assert(host.runtime.state.cells[0]==17&&host.runtime.state.revision==5);
  assert(pet_event_bus_deliver(&b,receiver,6,bus_owner_current,NULL,bus_host_enqueue,&host,&d)==PET_EVENT_IDLE);
  e.words[0]=9;assert(pet_event_bus_publish(&b,pub,2,1,&e,bus_owner_current,NULL,&d)==PET_EVENT_OK);
  mock.busy=true;unsigned finishes=mock.finishes;
  assert(pet_event_bus_deliver(&b,receiver,6,bus_owner_current,NULL,bus_host_enqueue,&host,&d)==PET_EVENT_STOPPED&&!d.consumed&&d.remaining==1);
  assert(mock.finishes==finishes+1&&!mock.held&&!host.runtime.active);
  mock.busy=false;assert(pet_event_bus_deliver(&b,receiver,6,bus_owner_current,NULL,bus_host_enqueue,&host,&d)==PET_EVENT_STOPPED);
  pet_program_snapshot_t seed;assert(pet_program_host_snapshot(&host,generation,&seed)==PET_RUNTIME_OK&&seed.values[0]==17);
  assert(pet_event_bus_close(&b,receiver)==PET_EVENT_OK);pet_event_registration_t old=receiver;receiver=(pet_event_registration_t){0};
  uint64_t next=0;assert(pet_program_host_start(&host,2,&seed,&next)==PET_RUNTIME_OK&&next>generation);
  assert(pet_event_bus_open(&b,mock.owner,0,1,bus_owner_current,NULL,&receiver)==PET_EVENT_OK&&receiver.serial>old.serial);
  assert(pet_event_bus_close(&b,old)==PET_EVENT_STALE);
  assert(pet_event_bus_deliver(&b,receiver,1,bus_owner_current,NULL,bus_host_enqueue,&host,&d)==PET_EVENT_IDLE&&!host.runtime.queued);
  assert(pet_program_host_cancel(&host,next)==PET_RUNTIME_OK);
  assert(pet_event_bus_close(&b,receiver)==PET_EVENT_OK&&pet_event_bus_close(&b,pub)==PET_EVENT_OK);
  assert(mock.finishes==finishes+2);puts("event bus host lifetime passed");return 0;
}
