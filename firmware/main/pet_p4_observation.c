#include "pet_p4_observation.h"
#include <limits.h>
_Static_assert(ATOMIC_INT_LOCK_FREE==2&&UINT_MAX==UINT32_MAX,"observation requires lock-free 32-bit atomics");
#define WRITING UINT_MAX
static int overlaps(const void *a,size_t an,const void *b,size_t bn){
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
pet_observation_result_t pet_observation_publish(pet_observation_t *c,const void *bytes,size_t length){
  if(!c||!bytes||!length||length>PET_OBSERVATION_BYTES||overlaps(c,sizeof(*c),bytes,length))return PET_OBSERVATION_INVALID;
  unsigned expected=0;
  if(!atomic_compare_exchange_weak_explicit(&c->writer,&expected,1,memory_order_acquire,memory_order_relaxed))return PET_OBSERVATION_BUSY;
  unsigned current=atomic_load_explicit(&c->published,memory_order_acquire);
  if(current>=PET_OBSERVATION_SLOTS){atomic_store_explicit(&c->writer,0,memory_order_release);return PET_OBSERVATION_INVALID;}
  for(unsigned offset=1;offset<PET_OBSERVATION_SLOTS;offset++){
    unsigned index=(current+offset)%PET_OBSERVATION_SLOTS;expected=0;
    if(!atomic_compare_exchange_weak_explicit(&c->slots[index].readers,&expected,WRITING,memory_order_acquire,memory_order_relaxed))continue;
    const unsigned char *input=bytes;
    for(size_t i=0;i<PET_OBSERVATION_BYTES;i++)c->slots[index].data[i]=i<length?input[i]:0;
    c->slots[index].length=(uint8_t)length;
    atomic_store_explicit(&c->slots[index].readers,0,memory_order_release);
    atomic_store_explicit(&c->published,index,memory_order_release);
    atomic_store_explicit(&c->writer,0,memory_order_release);return PET_OBSERVATION_OK;
  }
  atomic_store_explicit(&c->writer,0,memory_order_release);return PET_OBSERVATION_BUSY;
}
pet_observation_result_t pet_observation_capture(pet_observation_t *c,void *out,size_t length){
  if(!c||!out||!length||length>PET_OBSERVATION_BYTES||overlaps(c,sizeof(*c),out,length))return PET_OBSERVATION_INVALID;
  unsigned char staged[PET_OBSERVATION_BYTES];
  for(unsigned attempt=0;attempt<PET_OBSERVATION_SLOTS;attempt++){
    unsigned index=atomic_load_explicit(&c->published,memory_order_acquire);
    if(index>=PET_OBSERVATION_SLOTS)return PET_OBSERVATION_INVALID;
    unsigned readers=atomic_load_explicit(&c->slots[index].readers,memory_order_relaxed);
    if(readers>=WRITING-1U||!atomic_compare_exchange_weak_explicit(&c->slots[index].readers,&readers,readers+1U,memory_order_acquire,memory_order_relaxed))continue;
    uint8_t actual=c->slots[index].length;
    if(actual==length)for(size_t i=0;i<length;i++)staged[i]=c->slots[index].data[i];
    unsigned after=atomic_load_explicit(&c->published,memory_order_acquire);
    atomic_fetch_sub_explicit(&c->slots[index].readers,1,memory_order_release);
    if(after!=index)continue;
    if(!actual)return PET_OBSERVATION_NOT_READY;
    if(actual!=length)return PET_OBSERVATION_INVALID;
    unsigned char *output=out;for(size_t i=0;i<length;i++)output[i]=staged[i];
    return PET_OBSERVATION_OK;
  }
  return PET_OBSERVATION_BUSY;
}
