#include "pet_p4_observation.h"
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#ifndef PET_OBSERVATION_STRESS_ITERATIONS
#define PET_OBSERVATION_STRESS_ITERATIONS 50000U
#endif
typedef struct {uint32_t words[16];} packet_t;
static packet_t packet(uint32_t seed){packet_t p;for(unsigned i=0;i<16;i++)p.words[i]=seed^(i*UINT32_C(0x9e3779b9));return p;}
static bool valid(packet_t p){for(unsigned i=0;i<16;i++)if(p.words[i]!=(p.words[0]^(i*UINT32_C(0x9e3779b9))))return false;return true;}
static void sequential(void){
  pet_observation_t c={0};packet_t one=packet(1),two=packet(2),out,before;
  memset(&out,0xa5,sizeof(out));before=out;
  assert(pet_observation_capture(&c,&out,sizeof(out))==PET_OBSERVATION_NOT_READY&&!memcmp(&out,&before,sizeof(out)));
  assert(pet_observation_publish(&c,&one,sizeof(one))==PET_OBSERVATION_OK);
  assert(pet_observation_capture(&c,&out,sizeof(out))==PET_OBSERVATION_OK&&!memcmp(&one,&out,sizeof(out)));
  unsigned pinned=atomic_load(&c.published);atomic_fetch_add(&c.slots[pinned].readers,1);
  unsigned char retained[64];memcpy(retained,c.slots[pinned].data,64);
  for(unsigned i=2;i<20;i++){
    packet_t p=packet(i);assert(pet_observation_publish(&c,&p,sizeof(p))==PET_OBSERVATION_OK);
    assert(pet_observation_capture(&c,&out,sizeof(out))==PET_OBSERVATION_OK&&!memcmp(&p,&out,sizeof(out)));
    assert(!memcmp(retained,c.slots[pinned].data,64));
  }
  unsigned current=atomic_load(&c.published);
  for(unsigned i=0;i<3;i++)if(i!=current&&i!=pinned)atomic_store(&c.slots[i].readers,1);
  assert(pet_observation_publish(&c,&two,sizeof(two))==PET_OBSERVATION_BUSY&&atomic_load(&c.published)==current);
  for(unsigned i=0;i<3;i++)atomic_store(&c.slots[i].readers,0);
  atomic_store(&c.writer,1);assert(pet_observation_publish(&c,&two,sizeof(two))==PET_OBSERVATION_BUSY);atomic_store(&c.writer,0);
  before=out;atomic_store(&c.slots[current].readers,UINT_MAX);
  assert(pet_observation_capture(&c,&out,sizeof(out))==PET_OBSERVATION_BUSY&&!memcmp(&out,&before,sizeof(out)));
  atomic_store(&c.slots[current].readers,UINT_MAX-1U);
  assert(pet_observation_capture(&c,&out,sizeof(out))==PET_OBSERVATION_BUSY&&atomic_load(&c.slots[current].readers)==UINT_MAX-1U);
  atomic_store(&c.slots[current].readers,0);
  assert(pet_observation_capture(&c,&out,sizeof(out)-1)==PET_OBSERVATION_INVALID&&!memcmp(&out,&before,sizeof(out)));
  assert(pet_observation_publish(&c,&c,1)==PET_OBSERVATION_INVALID);
  assert(pet_observation_capture(&c,&c,1)==PET_OBSERVATION_INVALID);
  assert(pet_observation_publish(&c,&one,65)==PET_OBSERVATION_INVALID);
  assert(pet_observation_publish(&c,&one,0)==PET_OBSERVATION_INVALID);
  for(unsigned length=1;length<=64;length++){
    assert(pet_observation_publish(&c,&one,length)==PET_OBSERVATION_OK);
    memset(&out,0,sizeof(out));assert(pet_observation_capture(&c,&out,length)==PET_OBSERVATION_OK&&!memcmp(&one,&out,length));
    unsigned index=atomic_load(&c.published);for(unsigned j=length;j<64;j++)assert(!c.slots[index].data[j]);
  }
  atomic_store(&c.published,3);before=out;
  assert(pet_observation_publish(&c,&one,sizeof(one))==PET_OBSERVATION_INVALID&&!atomic_load(&c.writer));
  assert(pet_observation_capture(&c,&out,sizeof(out))==PET_OBSERVATION_INVALID&&!memcmp(&out,&before,sizeof(out)));
}
static pet_observation_t shared;
static atomic_bool stopped;
static atomic_uint writes,reads,misses;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool started;
static void start_gate(void){pthread_mutex_lock(&gate);while(!started)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);}
static void *writer(void *context){
  uintptr_t id=(uintptr_t)context;start_gate();
  for(unsigned i=1;i<=PET_OBSERVATION_STRESS_ITERATIONS;i++){
    packet_t p=packet(((uint32_t)id<<28)|i);pet_observation_result_t r=pet_observation_publish(&shared,&p,sizeof(p));
    assert(r==PET_OBSERVATION_OK||r==PET_OBSERVATION_BUSY);
    if(r==PET_OBSERVATION_OK)atomic_fetch_add(&writes,1);
  }return NULL;
}
static void *reader(void *context){
  (void)context;start_gate();
  while(!atomic_load_explicit(&stopped,memory_order_acquire)){
    packet_t p;memset(&p,0xa5,sizeof(p));packet_t old=p;
    pet_observation_result_t r=pet_observation_capture(&shared,&p,sizeof(p));
    if(r==PET_OBSERVATION_OK){assert(valid(p));atomic_fetch_add(&reads,1);}
    else{assert((r==PET_OBSERVATION_BUSY||r==PET_OBSERVATION_NOT_READY)&&!memcmp(&p,&old,sizeof(p)));atomic_fetch_add(&misses,1);}
  }return NULL;
}
int main(void){
  sequential();pthread_t writers[2],readers[4];
  for(unsigned i=0;i<4;i++)assert(!pthread_create(&readers[i],NULL,reader,NULL));
  for(unsigned i=0;i<2;i++)assert(!pthread_create(&writers[i],NULL,writer,(void*)(uintptr_t)(i+1)));
  pthread_mutex_lock(&gate);started=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
  for(unsigned i=0;i<2;i++)assert(!pthread_join(writers[i],NULL));
  atomic_store_explicit(&stopped,true,memory_order_release);for(unsigned i=0;i<4;i++)assert(!pthread_join(readers[i],NULL));
  assert(atomic_load(&writes)>100&&atomic_load(&reads)>100);
  for(unsigned i=0;i<3;i++)assert(!atomic_load(&shared.slots[i].readers));
  assert(!atomic_load(&shared.writer));
  printf("{\"coherent\":true,\"writers\":2,\"readers\":4,\"attempts\":%u,\"writes\":%u,\"reads\":%u,\"misses\":%u,\"cacheBytes\":%zu}\n",2U*PET_OBSERVATION_STRESS_ITERATIONS,atomic_load(&writes),atomic_load(&reads),atomic_load(&misses),sizeof(shared));
  return 0;
}
