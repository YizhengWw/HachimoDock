#include "pet_p4_event_bus.h"
#include <limits.h>
#ifndef PET_PROGRAM_WASM
#include <stdio.h>
#endif
#define CHECK(c) do{if(!(c))return __LINE__;}while(0)
static pet_resource_broker_t broker,foreign;
static pet_event_bus_t bus;
static pet_resource_owner_t owners[4];
static pet_event_registration_t regs[4];
static bool enabled[4],reenter,reentry_ok;
static unsigned checks,calls;
static pet_program_runtime_result_t response;
static pet_event_message_t received;
static uint64_t delivered_sequence;
static void clear(void *p,size_t n){unsigned char *b=p;for(size_t i=0;i<n;i++)b[i]=0;}
static bool same(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return false;return true;}
static bool current(void *context,pet_resource_owner_t owner){
  (void)context;checks++;
  if(reenter){pet_event_diagnostic_t d={.recipients=77};pet_program_event_t e={0};
    reentry_ok=pet_event_bus_publish(&bus,regs[0],999,1,&e,current,NULL,&d)==PET_EVENT_BUSY&&d.recipients==77
      &&pet_event_bus_close(&bus,regs[0])==PET_EVENT_BUSY&&!pet_event_bus_init(&bus);}
  return owner.broker==&broker&&owner.slot<4&&enabled[owner.slot]&&owner.generation==owners[owner.slot].generation;
}
static pet_program_runtime_result_t enqueue(void *context,pet_resource_owner_t owner,uint64_t sequence,const pet_event_message_t *message){
  (void)context;(void)owner;calls++;received=*message;delivered_sequence=sequence;
  if(reenter){pet_event_diagnostic_t d={.remaining=99};
    reentry_ok=pet_event_bus_deliver(&bus,regs[1],sequence,current,NULL,enqueue,NULL,&d)==PET_EVENT_BUSY&&d.remaining==99
      &&pet_event_bus_close(&bus,regs[1])==PET_EVENT_BUSY;}
  return response;
}
static int setup(unsigned count){
  clear(&bus,sizeof(bus));clear(regs,sizeof(regs));clear(owners,sizeof(owners));clear(enabled,sizeof(enabled));
  response=PET_RUNTIME_OK;checks=calls=0;reenter=reentry_ok=false;
  CHECK(pet_event_bus_init(&bus)&&!pet_event_bus_init(&bus));
  for(unsigned i=0;i<count;i++){
    owners[i]=(pet_resource_owner_t){&broker,10+i,(uint8_t)i};enabled[i]=true;
    CHECK(pet_event_bus_open(&bus,owners[i],i?0:UINT32_MAX,i?3:0,current,NULL,&regs[i])==PET_EVENT_OK);
  }return 0;
}
static int ownership(void){
  CHECK(!setup(2));pet_event_registration_t out={0},before=out;
  CHECK(pet_event_bus_open(&bus,owners[1],1,1,current,NULL,&out)==PET_EVENT_BUSY&&same(&out,&before,sizeof(out)));
  pet_resource_owner_t alien={&foreign,1,0};
  CHECK(pet_event_bus_open(&bus,alien,1,1,current,NULL,&out)==PET_EVENT_DENIED);
  CHECK(pet_event_bus_open(&bus,owners[1],1,1,current,NULL,(void*)&bus.slots[2])==PET_EVENT_INVALID);
  pet_event_registration_t old=regs[1];enabled[1]=false;pet_event_diagnostic_t d={.remaining=77};
  CHECK(pet_event_bus_deliver(&bus,old,1,current,NULL,enqueue,NULL,&d)==PET_EVENT_STOPPED&&!calls);
  enabled[1]=true;CHECK(pet_event_bus_deliver(&bus,old,1,current,NULL,enqueue,NULL,&d)==PET_EVENT_STOPPED&&!calls);
  CHECK(pet_event_bus_close(&bus,old)==PET_EVENT_OK);owners[1].generation++;regs[1]=(pet_event_registration_t){0};
  CHECK(pet_event_bus_open(&bus,owners[1],0,3,current,NULL,&regs[1])==PET_EVENT_OK&&regs[1].serial>old.serial);
  CHECK(pet_event_bus_close(&bus,old)==PET_EVENT_STALE);
  CHECK(pet_event_bus_deliver(&bus,old,1,current,NULL,enqueue,NULL,&d)==PET_EVENT_STALE);
  CHECK(pet_event_bus_deliver(&bus,regs[1],1,current,NULL,enqueue,NULL,&d)==PET_EVENT_IDLE);
  bus.serial=UINT64_MAX;CHECK(pet_event_bus_close(&bus,regs[1])==PET_EVENT_OK);regs[1]=(pet_event_registration_t){0};
  CHECK(pet_event_bus_open(&bus,owners[1],0,3,current,NULL,&regs[1])==PET_EVENT_EXHAUSTED);return 0;
}
static int atomic_fanout(void){
  CHECK(!setup(3));pet_program_event_t e={1,{7,99,99,99}};pet_event_diagnostic_t d;
  reenter=true;
  for(unsigned i=1;i<=4;i++)CHECK(pet_event_bus_publish(&bus,regs[0],i,1,&e,current,NULL,&d)==PET_EVENT_OK&&d.recipients==2&&reentry_ok);
  CHECK(pet_event_bus_deliver(&bus,regs[1],1,current,NULL,enqueue,NULL,&d)==PET_EVENT_DELIVERED&&reentry_ok&&d.remaining==3);
  CHECK(delivered_sequence==1&&received.publisher_sequence==1&&received.publisher_generation==10&&received.topic==1&&received.event.words[0]==7&&!received.event.words[1]);
  pet_event_bus_t before=bus;d.recipients=77;
  CHECK(pet_event_bus_publish(&bus,regs[0],5,1,&e,current,NULL,&d)==PET_EVENT_BACKPRESSURE&&d.recipients==77&&same(&before,&bus,sizeof(bus)));
  CHECK(pet_event_bus_publish(&bus,regs[0],4,1,&e,current,NULL,&d)==PET_EVENT_SEQUENCE_ERROR);
  CHECK(pet_event_bus_deliver(&bus,regs[2],1,current,NULL,enqueue,NULL,&d)==PET_EVENT_DELIVERED);
  CHECK(pet_event_bus_publish(&bus,regs[0],5,1,&e,current,NULL,&d)==PET_EVENT_OK&&d.recipients==2);
  for(unsigned recipient=1;recipient<=2;recipient++)for(unsigned i=2;i<=5;i++){
    CHECK(pet_event_bus_deliver(&bus,regs[recipient],i,current,NULL,enqueue,NULL,&d)==PET_EVENT_DELIVERED&&received.publisher_sequence==i);
  }
  CHECK(pet_event_bus_deliver(&bus,regs[1],6,current,NULL,enqueue,NULL,&d)==PET_EVENT_IDLE);
  CHECK(pet_event_bus_publish(&bus,regs[0],6,32,&e,current,NULL,&d)==PET_EVENT_OK&&!d.recipients);
  CHECK(pet_event_bus_publish(&bus,regs[0],6,32,&e,current,NULL,&d)==PET_EVENT_SEQUENCE_ERROR);
  CHECK(pet_event_bus_publish(&bus,regs[1],1,1,&e,current,NULL,&d)==PET_EVENT_DENIED);
  CHECK(pet_event_bus_publish(&bus,regs[0],7,33,&e,current,NULL,&d)==PET_EVENT_INVALID);
  CHECK(pet_event_bus_publish(&bus,regs[0],7,1,&e,current,NULL,(void*)&bus.slots[0])==PET_EVENT_INVALID);
  CHECK(pet_event_bus_publish(&bus,regs[0],7,1,(void*)&bus.slots[0],current,NULL,&d)==PET_EVENT_INVALID);
  enabled[2]=false;CHECK(pet_event_bus_publish(&bus,regs[0],7,1,&e,current,NULL,&d)==PET_EVENT_OK&&d.recipients==1);
  enabled[0]=false;CHECK(pet_event_bus_publish(&bus,regs[0],8,1,&e,current,NULL,&d)==PET_EVENT_STOPPED);
  enabled[0]=true;CHECK(pet_event_bus_publish(&bus,regs[0],8,1,&e,current,NULL,&d)==PET_EVENT_STOPPED);return 0;
}
static int sequence_edges_and_retirement(void){
  CHECK(!setup(2));pet_program_event_t e={0};pet_event_diagnostic_t d;
  CHECK(pet_event_bus_publish(&bus,regs[0],UINT64_MAX,1,&e,current,NULL,&d)==PET_EVENT_OK);
  CHECK(pet_event_bus_publish(&bus,regs[0],1,1,&e,current,NULL,&d)==PET_EVENT_SEQUENCE_ERROR);
  /* Retirement does not undo a publication accepted while the sender lived. */
  CHECK(pet_event_bus_close(&bus,regs[0])==PET_EVENT_OK);
  CHECK(pet_event_bus_deliver(&bus,regs[1],UINT64_MAX,current,NULL,enqueue,NULL,&d)==PET_EVENT_DELIVERED&&received.publisher_sequence==UINT64_MAX&&!received.event.count);
  unsigned count=calls;
  CHECK(pet_event_bus_deliver(&bus,regs[1],1,current,NULL,enqueue,NULL,&d)==PET_EVENT_SEQUENCE_ERROR&&calls==count);
  CHECK(!setup(4));pet_event_registration_t extra={0};pet_resource_owner_t o={&broker,99,4};
  CHECK(pet_event_bus_open(&bus,o,1,0,current,NULL,&extra)==PET_EVENT_BACKPRESSURE&&!extra.serial);
  CHECK(pet_event_bus_close(&bus,regs[1])==PET_EVENT_OK);enabled[1]=false;extra=(pet_event_registration_t){0};
  CHECK(pet_event_bus_open(&bus,owners[1],1,0,current,NULL,&extra)==PET_EVENT_DENIED&&!extra.serial);return 0;
}
static int delivery_outcomes(void){
  const pet_program_runtime_result_t results[]={PET_RUNTIME_BUSY,PET_RUNTIME_QUEUE_FULL,PET_RUNTIME_DUPLICATE,PET_RUNTIME_DENIED,PET_RUNTIME_INVALID};
  for(unsigned i=0;i<sizeof(results)/sizeof(results[0]);i++){
    CHECK(!setup(2));pet_program_event_t event={1,{4}};pet_event_diagnostic_t d;
    CHECK(pet_event_bus_publish(&bus,regs[0],1,1,&event,current,NULL,&d)==PET_EVENT_OK);
    response=results[i];CHECK(pet_event_bus_deliver(&bus,regs[1],1,current,NULL,enqueue,NULL,&d)==(i<2?PET_EVENT_BACKPRESSURE:i==2?PET_EVENT_SEQUENCE_ERROR:PET_EVENT_STOPPED));
    CHECK(d.offered&&!d.consumed&&d.remaining==1&&calls==1&&bus.slots[regs[1].slot].count==1);
    response=PET_RUNTIME_OK;
    CHECK(pet_event_bus_deliver(&bus,regs[1],1,current,NULL,enqueue,NULL,&d)==(i<2?PET_EVENT_DELIVERED:PET_EVENT_STOPPED));
    CHECK(calls==(i<2?2U:1U));
  }return 0;
}
static uint32_t rng=31;
static uint32_t next(void){rng=rng*1664525U+1013904223U;return rng;}
static int mutations(void){
  CHECK(!setup(4));uint64_t publish_id=1,delivery[4]={0,1,1,1},last[4]={0};
  for(unsigned i=0;i<30000;i++){
    pet_event_diagnostic_t d;unsigned op=(next()>>16)%4;
    if(!op){pet_program_event_t e={1,{(int32_t)(publish_id%1000)}};
      pet_event_bus_result_t r=pet_event_bus_publish(&bus,regs[0],publish_id,1,&e,current,NULL,&d);
      if(r==PET_EVENT_OK){CHECK(d.recipients==3);publish_id++;}else CHECK(r==PET_EVENT_BACKPRESSURE);
    }else{
      response=(next()>>16)%4?PET_RUNTIME_OK:PET_RUNTIME_QUEUE_FULL;
      pet_event_bus_result_t r=pet_event_bus_deliver(&bus,regs[op],delivery[op],current,NULL,enqueue,NULL,&d);
      if(r==PET_EVENT_DELIVERED){CHECK(received.publisher_sequence==last[op]+1&&received.event.words[0]==(int32_t)(received.publisher_sequence%1000));last[op]=received.publisher_sequence;delivery[op]++;}
      else CHECK(r==PET_EVENT_IDLE||r==PET_EVENT_BACKPRESSURE);
    }
    for(unsigned j=1;j<=3;j++)CHECK(bus.slots[regs[j].slot].count<=4&&bus.slots[regs[j].slot].count==publish_id-1-last[j]);
  }
  CHECK(publish_id>1000&&last[1]>1000&&last[2]>1000&&last[3]>1000);return 0;
}
int pet_event_bus_selftest(void){int r=ownership();if(r)return r;r=atomic_fanout();if(r)return r;r=sequence_edges_and_retirement();if(r)return r;r=delivery_outcomes();if(r)return r;return mutations();}
#ifndef PET_PROGRAM_WASM
int main(void){int r=pet_event_bus_selftest();printf("{\"result\":%d,\"trials\":30000,\"busBytes\":%zu}\n",r,sizeof(pet_event_bus_t));return r?1:0;}
#endif
