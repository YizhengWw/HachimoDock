/* Same pure event planner on native sanitizers and product WASM. */
#include "pet_p4_program_timers.h"
#include <limits.h>
#ifndef PET_PROGRAM_WASM
#include <stdio.h>
#endif
#define CHECK(c) do{if(!(c))return __LINE__;}while(0)
typedef struct {
  pet_program_timers_t *timer;
  pet_program_runtime_result_t result;
  pet_program_event_t seen;
  unsigned calls;
  uint64_t sequence,generation;
  bool reenter,reentry_ok;
} mock_t;
static bool equal(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return false;return true;}
static pet_program_runtime_result_t enqueue(void *ctx,uint64_t generation,uint64_t sequence,const pet_program_event_t *event){
  mock_t *m=ctx;m->calls++;m->seen=*event;m->sequence=sequence;m->generation=generation;
  if(m->reenter){
    pet_program_timer_diagnostic_t d={.timer_id=999};
    m->reentry_ok=pet_program_timers_tick(m->timer,generation,0,sequence,enqueue,ctx,&d)==PET_TIMERS_BUSY&&d.timer_id==999
      &&pet_program_timers_cancel(m->timer,generation)==PET_TIMERS_BUSY;
  }
  return m->result;
}
static pet_program_timer_spec_t spec(unsigned id,unsigned delay,unsigned interval,int32_t word){
  pet_program_timer_spec_t s={.id=(uint16_t)id,.delay_ms=delay,.interval_ms=interval,.event={1,{word,99,99,99}}};return s;
}
static int validation(void){
  pet_program_timers_t t={0},zero=t;pet_program_timer_spec_t specs[2]={spec(1,10,100,5),spec(2,20,0,7)};
  CHECK(pet_program_timers_start(&t,0,0,specs,2)==PET_TIMERS_INVALID&&equal(&t,&zero,sizeof(t)));
  CHECK(pet_program_timers_start(&t,1,0,specs,0)==PET_TIMERS_INVALID);
  CHECK(pet_program_timers_start(&t,1,UINT64_MAX,specs,2)==PET_TIMERS_INVALID);
  specs[1].id=1;CHECK(pet_program_timers_start(&t,1,0,specs,2)==PET_TIMERS_INVALID);specs[1].id=2;
  specs[1].interval_ms=99;CHECK(pet_program_timers_start(&t,1,0,specs,2)==PET_TIMERS_INVALID);specs[1].interval_ms=0;
  specs[1].event.count=5;CHECK(pet_program_timers_start(&t,1,0,specs,2)==PET_TIMERS_INVALID);specs[1].event.count=1;
  CHECK(equal(&t,&zero,sizeof(t)));
  CHECK(pet_program_timers_start(&t,1,0,(const void*)&t.specs[0],1)==PET_TIMERS_INVALID);
  CHECK(pet_program_timers_start(&t,1,0,specs,2)==PET_TIMERS_OK);
  CHECK(!t.specs[0].event.words[1]&&!t.specs[0].event.words[2]&&!t.specs[0].event.words[3]);
  CHECK(pet_program_timers_start(&t,2,0,specs,2)==PET_TIMERS_INVALID);
  pet_program_timers_t before=t;mock_t m={.timer=&t};pet_program_timer_diagnostic_t d={.timer_id=999};
  CHECK(pet_program_timers_tick(&t,2,10,1,enqueue,&m,&d)==PET_TIMERS_STALE&&d.timer_id==999&&equal(&t,&before,sizeof(t)));
  CHECK(pet_program_timers_tick(&t,1,10,1,enqueue,&m,(void*)&t.specs[0])==PET_TIMERS_INVALID&&equal(&t,&before,sizeof(t)));
  CHECK(pet_program_timers_cancel(&t,2)==PET_TIMERS_STALE);
  CHECK(pet_program_timers_cancel(&t,1)==PET_TIMERS_OK);
  CHECK(pet_program_timers_tick(&t,1,10,1,enqueue,&m,&d)==PET_TIMERS_STOPPED&&!m.calls);
  CHECK(pet_program_timers_start(&t,2,0,specs,2)==PET_TIMERS_INVALID);return 0;
}
static int backpressure_fairness(void){
  pet_program_timers_t t={0};pet_program_timer_spec_t specs[2]={spec(1,0,100,5),spec(2,0,100,7)};
  CHECK(pet_program_timers_start(&t,9,100,specs,2)==PET_TIMERS_OK);specs[0].event.words[0]=99;
  mock_t m={.timer=&t,.result=PET_RUNTIME_QUEUE_FULL,.reenter=true};pet_program_timer_diagnostic_t d;
  CHECK(pet_program_timers_tick(&t,9,1100,1,enqueue,&m,&d)==PET_TIMERS_BACKPRESSURE);
  CHECK(m.calls==1&&m.reentry_ok&&m.seen.words[0]==5&&d.offered&&!d.accepted&&t.deadlines[0]==100&&!t.accepted&&!t.last_sequence);
  m.result=PET_RUNTIME_OK;CHECK(pet_program_timers_tick(&t,9,1100,1,enqueue,&m,&d)==PET_TIMERS_ENQUEUED);
  CHECK(m.calls==2&&m.generation==9&&m.sequence==1&&d.timer_id==1&&d.coalesced==10&&d.accepted&&t.deadlines[0]==1200);
  CHECK(pet_program_timers_tick(&t,9,1100,2,enqueue,&m,&d)==PET_TIMERS_ENQUEUED&&d.timer_id==2&&d.coalesced==10);
  CHECK(pet_program_timers_tick(&t,9,1100,3,enqueue,&m,&d)==PET_TIMERS_IDLE&&m.calls==3&&!d.offered&&t.missed==20);
  CHECK(pet_program_timers_tick(&t,9,1201,3,enqueue,&m,&d)==PET_TIMERS_ENQUEUED&&d.timer_id==1&&t.deadlines[0]==1300);
  CHECK(pet_program_timers_tick(&t,9,1201,4,enqueue,&m,&d)==PET_TIMERS_ENQUEUED&&d.timer_id==2);
  unsigned calls=m.calls;CHECK(pet_program_timers_tick(&t,9,1200,5,enqueue,&m,&d)==PET_TIMERS_CLOCK_ERROR&&m.calls==calls);
  CHECK(pet_program_timers_tick(&t,9,2000,5,enqueue,&m,&d)==PET_TIMERS_STOPPED&&m.calls==calls);return 0;
}
static int edges(void){
  pet_program_timer_spec_t s=spec(1,0,100,1);pet_program_timers_t t={0};mock_t m={.timer=&t};pet_program_timer_diagnostic_t d;
  CHECK(pet_program_timers_start(&t,1,0,&s,1)==PET_TIMERS_OK);
  CHECK(pet_program_timers_tick(&t,1,UINT64_MAX,UINT64_MAX,enqueue,&m,&d)==PET_TIMERS_ENQUEUED);
  CHECK(d.deadline_exhausted&&d.coalesced==UINT32_MAX&&t.missed==UINT32_MAX&&!t.active_mask&&m.calls==1);
  CHECK(pet_program_timers_tick(&t,1,UINT64_MAX,1,enqueue,&m,&d)==PET_TIMERS_SEQUENCE_ERROR&&m.calls==1);
  t=(pet_program_timers_t){0};s.interval_ms=0;s.delay_ms=1;
  CHECK(pet_program_timers_start(&t,1,0,&s,1)==PET_TIMERS_OK);
  CHECK(pet_program_timers_tick(&t,1,0,1,enqueue,&m,&d)==PET_TIMERS_IDLE);
  CHECK(pet_program_timers_tick(&t,1,1,1,enqueue,&m,&d)==PET_TIMERS_ENQUEUED&&!t.active_mask);
  CHECK(pet_program_timers_tick(&t,1,UINT64_MAX,2,enqueue,&m,&d)==PET_TIMERS_IDLE&&m.calls==2);
  for(unsigned i=0;i<4;i++){
    t=(pet_program_timers_t){0};s.delay_ms=0;
    CHECK(pet_program_timers_start(&t,1,0,&s,1)==PET_TIMERS_OK);
    const pet_program_runtime_result_t failures[]={PET_RUNTIME_DENIED,PET_RUNTIME_STALE,PET_RUNTIME_STOPPED,PET_RUNTIME_DUPLICATE};
    m.result=failures[i];unsigned calls=m.calls;
    CHECK(pet_program_timers_tick(&t,1,0,1,enqueue,&m,&d)==(i==3?PET_TIMERS_SEQUENCE_ERROR:PET_TIMERS_STOPPED));
    CHECK(!d.accepted&&!t.accepted&&t.cancelled&&m.calls==calls+1);
    m.result=PET_RUNTIME_OK;CHECK(pet_program_timers_tick(&t,1,100,2,enqueue,&m,&d)==PET_TIMERS_STOPPED&&m.calls==calls+1);
  }return 0;
}
static uint32_t rng=19;
static uint32_t next(void){rng=rng*1664525U+1013904223U;return rng;}
static int fuzz(void){
  pet_program_timers_t t={0};pet_program_timer_spec_t specs[8];
  for(unsigned i=0;i<8;i++)specs[i]=spec(i+1,i*19,100+i*17,(int32_t)i);
  CHECK(pet_program_timers_start(&t,7,0,specs,8)==PET_TIMERS_OK);
  mock_t m={.timer=&t};pet_program_timer_diagnostic_t d;uint64_t now=0;
  for(unsigned i=1;i<=20000;i++){
    now+=next()%10000;m.result=next()%5?PET_RUNTIME_OK:PET_RUNTIME_QUEUE_FULL;
    uint64_t deadlines[8];for(unsigned j=0;j<8;j++)deadlines[j]=t.deadlines[j];unsigned calls=m.calls;
    pet_program_timers_result_t r=pet_program_timers_tick(&t,7,now,i,enqueue,&m,&d);
    CHECK(m.calls-calls<=1);
    if(r==PET_TIMERS_ENQUEUED){
      CHECK(d.accepted&&d.timer_id>=1&&d.timer_id<=8&&m.seen.words[0]==d.timer_id-1&&t.deadlines[d.timer_id-1]>now);
      CHECK(d.coalesced==(now-deadlines[d.timer_id-1])/specs[d.timer_id-1].interval_ms);
    }else CHECK(r==PET_TIMERS_BACKPRESSURE||r==PET_TIMERS_IDLE);
    for(unsigned j=0;j<8;j++)if(!d.accepted||j!=(unsigned)d.timer_id-1U)CHECK(t.deadlines[j]==deadlines[j]);
  }return 0;
}
static int copied_instance(void){
  pet_program_timers_t t={0};pet_program_timer_spec_t s=spec(1,0,100,7);
  CHECK(pet_program_timers_start(&t,1,0,&s,1)==PET_TIMERS_OK);
  pet_program_timers_t copy=t,before=t,copy_before=copy;mock_t m={.timer=&t,.result=PET_RUNTIME_OK};
  pet_program_timer_diagnostic_t d={.timer_id=999};
  CHECK(pet_program_timers_tick(&copy,1,100,1,enqueue,&m,&d)==PET_TIMERS_INVALID&&d.timer_id==999&&!m.calls);
  CHECK(pet_program_timers_cancel(&copy,1)==PET_TIMERS_INVALID);
  CHECK(equal(&t,&before,sizeof(t))&&equal(&copy,&copy_before,sizeof(copy)));
  CHECK(pet_program_timers_tick(&t,1,100,1,enqueue,&m,&d)==PET_TIMERS_ENQUEUED&&m.calls==1&&d.accepted);
  CHECK(pet_program_timers_cancel(&t,1)==PET_TIMERS_OK);return 0;
}
int pet_program_timers_selftest(void){int r=validation();if(r)return r;r=backpressure_fairness();if(r)return r;r=edges();if(r)return r;r=copied_instance();if(r)return r;return fuzz();}
#ifndef PET_PROGRAM_WASM
int main(void){int r=pet_program_timers_selftest();printf("{\"result\":%d,\"trials\":20000,\"timerBytes\":%zu}\n",r,sizeof(pet_program_timers_t));return r?1:0;}
#endif
