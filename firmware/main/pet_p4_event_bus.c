#include "pet_p4_event_bus.h"
#include <limits.h>
#define BUS_MARKER UINT32_C(0x50344531)
static void clear(void *p,size_t n){unsigned char *b=p;for(size_t i=0;i<n;i++)b[i]=0;}
static bool overlaps(const void *a,size_t an,const void *b,size_t bn){
  if(!a||!b)return false;
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
  return x<=y?y-x<an:x-y<bn;
}
static bool same_owner(pet_resource_owner_t a,pet_resource_owner_t b){return a.broker==b.broker&&a.generation==b.generation&&a.slot==b.slot;}
static bool valid_owner(pet_resource_owner_t o){return o.broker&&o.generation&&o.slot<PET_RESOURCE_OWNER_SLOTS;}
static pet_event_bus_result_t enter(pet_event_bus_t *b){
  if(!b||b->marker!=BUS_MARKER)return PET_EVENT_INVALID;
  if(b->inside)return PET_EVENT_BUSY;
  b->inside=true;return PET_EVENT_OK;
}
static pet_event_bus_result_t leave(pet_event_bus_t *b,pet_event_bus_result_t result){b->inside=false;return result;}
static bool registered(const pet_event_bus_t *b,pet_event_registration_t r){
  return r.bus==b&&r.serial&&r.slot<PET_EVENT_BUS_OWNERS&&b->slots[r.slot].live&&b->slots[r.slot].serial==r.serial;
}
bool pet_event_bus_init(pet_event_bus_t *b){
  if(!b||b->marker||b->inside||b->serial)return false;
  clear(b,sizeof(*b));b->marker=BUS_MARKER;return true;
}
pet_event_bus_result_t pet_event_bus_open(pet_event_bus_t *b,pet_resource_owner_t owner,
    uint32_t publish_mask,uint32_t subscribe_mask,pet_event_owner_current_t current,void *context,pet_event_registration_t *out){
  if(!out||!current||!valid_owner(owner)||(!publish_mask&&!subscribe_mask)||overlaps(b,sizeof(*b),out,sizeof(*out)))return PET_EVENT_INVALID;
  pet_event_bus_result_t status=enter(b);if(status!=PET_EVENT_OK)return status;
  if(out->bus||out->serial||out->slot)return leave(b,PET_EVENT_INVALID);
  unsigned vacant=PET_EVENT_BUS_OWNERS;
  for(unsigned i=0;i<PET_EVENT_BUS_OWNERS;i++){
    if(!b->slots[i].live){if(vacant==PET_EVENT_BUS_OWNERS)vacant=i;continue;}
    if(b->slots[i].owner.broker!=owner.broker)return leave(b,PET_EVENT_DENIED);
    if(same_owner(b->slots[i].owner,owner))return leave(b,PET_EVENT_BUSY);
  }
  if(vacant==PET_EVENT_BUS_OWNERS)return leave(b,PET_EVENT_BACKPRESSURE);
  if(b->serial==UINT64_MAX)return leave(b,PET_EVENT_EXHAUSTED);
  if(!current(context,owner))return leave(b,PET_EVENT_DENIED);
  clear(&b->slots[vacant],sizeof(b->slots[vacant]));
  b->slots[vacant].owner=owner;b->slots[vacant].publish_mask=publish_mask;b->slots[vacant].subscribe_mask=subscribe_mask;
  b->slots[vacant].serial=++b->serial;b->slots[vacant].live=true;
  *out=(pet_event_registration_t){b,b->serial,(uint8_t)vacant};return leave(b,PET_EVENT_OK);
}
pet_event_bus_result_t pet_event_bus_publish(pet_event_bus_t *b,pet_event_registration_t publisher,
    uint64_t sequence,unsigned topic,const pet_program_event_t *event,
    pet_event_owner_current_t current,void *context,pet_event_diagnostic_t *out){
  if(!out||!current||!event||!sequence||!topic||topic>32||event->count>PET_PROGRAM_EVENT_WORDS
      ||overlaps(b,sizeof(*b),out,sizeof(*out))||overlaps(b,sizeof(*b),event,sizeof(*event))
      ||overlaps(out,sizeof(*out),event,sizeof(*event)))return PET_EVENT_INVALID;
  pet_event_bus_result_t status=enter(b);if(status!=PET_EVENT_OK)return status;
  if(!registered(b,publisher))return leave(b,PET_EVENT_STALE);
  if(b->slots[publisher.slot].stopped)return leave(b,PET_EVENT_STOPPED);
  uint32_t mask=UINT32_C(1)<<(topic-1);
  if(!(b->slots[publisher.slot].publish_mask&mask))return leave(b,PET_EVENT_DENIED);
  if(sequence<=b->slots[publisher.slot].last_publish)return leave(b,PET_EVENT_SEQUENCE_ERROR);
  if(!current(context,b->slots[publisher.slot].owner)){
    b->slots[publisher.slot].stopped=true;return leave(b,PET_EVENT_STOPPED);
  }
  uint8_t targets=0;
  for(unsigned i=0;i<PET_EVENT_BUS_OWNERS;i++){
    if(!b->slots[i].live||b->slots[i].stopped||!(b->slots[i].subscribe_mask&mask))continue;
    if(!current(context,b->slots[i].owner))continue;
    if(b->slots[i].count==PET_EVENT_BUS_QUEUE)return leave(b,PET_EVENT_BACKPRESSURE);
    targets|=(uint8_t)(1U<<i);
  }
  clear(out,sizeof(*out));out->enqueue_result=PET_RUNTIME_IDLE;
  for(unsigned i=0;i<PET_EVENT_BUS_OWNERS;i++)if(targets&(1U<<i)){
    unsigned tail=(b->slots[i].head+b->slots[i].count)%PET_EVENT_BUS_QUEUE;
    pet_event_message_t *message=&b->slots[i].queue[tail];clear(message,sizeof(*message));
    message->publisher_generation=b->slots[publisher.slot].owner.generation;message->publisher_sequence=sequence;
    message->topic=(uint8_t)topic;message->event.count=event->count;
    for(unsigned j=0;j<event->count;j++)message->event.words[j]=event->words[j];
    b->slots[i].count++;out->recipients++;
  }
  b->slots[publisher.slot].last_publish=sequence;return leave(b,PET_EVENT_OK);
}
pet_event_bus_result_t pet_event_bus_deliver(pet_event_bus_t *b,pet_event_registration_t receiver,
    uint64_t sequence,pet_event_owner_current_t current,void *current_context,
    pet_event_enqueue_t enqueue,void *enqueue_context,pet_event_diagnostic_t *out){
  if(!out||!current||!enqueue||!sequence||overlaps(b,sizeof(*b),out,sizeof(*out)))return PET_EVENT_INVALID;
  pet_event_bus_result_t status=enter(b);if(status!=PET_EVENT_OK)return status;
  if(!registered(b,receiver))return leave(b,PET_EVENT_STALE);
  if(b->slots[receiver.slot].stopped)return leave(b,PET_EVENT_STOPPED);
  if(sequence<=b->slots[receiver.slot].last_delivery)return leave(b,PET_EVENT_SEQUENCE_ERROR);
  clear(out,sizeof(*out));out->enqueue_result=PET_RUNTIME_IDLE;
  if(!current(current_context,b->slots[receiver.slot].owner)){
    b->slots[receiver.slot].stopped=true;return leave(b,PET_EVENT_STOPPED);
  }
  if(!b->slots[receiver.slot].count)return leave(b,PET_EVENT_IDLE);
  out->offered=true;
  pet_program_runtime_result_t result=enqueue(enqueue_context,b->slots[receiver.slot].owner,sequence,&b->slots[receiver.slot].queue[b->slots[receiver.slot].head]);
  out->enqueue_result=result;out->remaining=b->slots[receiver.slot].count;
  if(result==PET_RUNTIME_BUSY||result==PET_RUNTIME_QUEUE_FULL)return leave(b,PET_EVENT_BACKPRESSURE);
  if(result!=PET_RUNTIME_OK){b->slots[receiver.slot].stopped=true;return leave(b,result==PET_RUNTIME_DUPLICATE?PET_EVENT_SEQUENCE_ERROR:PET_EVENT_STOPPED);}
  clear(&b->slots[receiver.slot].queue[b->slots[receiver.slot].head],sizeof(pet_event_message_t));
  b->slots[receiver.slot].head=(uint8_t)((b->slots[receiver.slot].head+1U)%PET_EVENT_BUS_QUEUE);
  b->slots[receiver.slot].count--;b->slots[receiver.slot].last_delivery=sequence;
  out->consumed=true;out->remaining=b->slots[receiver.slot].count;return leave(b,PET_EVENT_DELIVERED);
}
pet_event_bus_result_t pet_event_bus_close(pet_event_bus_t *b,pet_event_registration_t r){
  pet_event_bus_result_t status=enter(b);if(status!=PET_EVENT_OK)return status;
  if(!registered(b,r))return leave(b,PET_EVENT_STALE);
  clear(&b->slots[r.slot],sizeof(b->slots[r.slot]));return leave(b,PET_EVENT_OK);
}
