#include "pet_p4_program_timers.h"
#include <limits.h>
#define TIMER_MARKER UINT32_C(0x50345431)
static void clear(void *out,size_t count){unsigned char *p=out;for(size_t i=0;i<count;i++)p[i]=0;}
static bool overlap(const void *a,size_t an,const void *b,size_t bn){
  if(!a||!b)return false;
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
static uint32_t saturate(uint32_t a,uint64_t b){return b>UINT32_MAX-a?UINT32_MAX:a+(uint32_t)b;}
static pet_program_timers_result_t enter(pet_program_timers_t *t,uint64_t generation){
  if(!t||t->instance!=t||t->marker!=TIMER_MARKER)return PET_TIMERS_INVALID;
  if(t->inside)return PET_TIMERS_BUSY;
  if(!generation||generation!=t->generation)return PET_TIMERS_STALE;
  return PET_TIMERS_OK;
}
pet_program_timers_result_t pet_program_timers_start(pet_program_timers_t *t,
    uint64_t generation,uint64_t now,const pet_program_timer_spec_t *specs,unsigned count){
  if(!t||!specs||!generation||!count||count>PET_PROGRAM_TIMERS
      ||overlap(t,sizeof(*t),specs,count*sizeof(*specs)))return PET_TIMERS_INVALID;
  if(t->inside)return PET_TIMERS_BUSY;
  if(t->instance||t->marker||t->generation||t->cancelled)return PET_TIMERS_INVALID;
  for(unsigned i=0;i<count;i++){
    const pet_program_timer_spec_t *s=&specs[i];
    if(!s->id||s->event.count>PET_PROGRAM_EVENT_WORDS||now>UINT64_MAX-s->delay_ms
        ||(s->interval_ms&&s->interval_ms<PET_PROGRAM_TIMER_MIN_INTERVAL_MS))return PET_TIMERS_INVALID;
    for(unsigned j=0;j<i;j++)if(s->id==specs[j].id)return PET_TIMERS_INVALID;
  }
  /* No callback, allocation or observable partial publication. */
  for(unsigned i=0;i<PET_PROGRAM_TIMERS;i++){
    clear(&t->specs[i],sizeof(t->specs[i]));t->deadlines[i]=0;
    if(i<count){
      t->specs[i].id=specs[i].id;t->specs[i].delay_ms=specs[i].delay_ms;
      t->specs[i].interval_ms=specs[i].interval_ms;t->specs[i].event.count=specs[i].event.count;
      for(unsigned j=0;j<specs[i].event.count;j++)t->specs[i].event.words[j]=specs[i].event.words[j];
      t->deadlines[i]=now+specs[i].delay_ms;
    }
  }
  t->generation=generation;t->last_now=now;t->last_sequence=0;t->missed=0;t->accepted=0;
  t->count=(uint8_t)count;t->cursor=0;t->active_mask=(uint8_t)((1U<<count)-1U);
  t->instance=t;t->inside=false;t->cancelled=false;t->marker=TIMER_MARKER;return PET_TIMERS_OK;
}
pet_program_timers_result_t pet_program_timers_tick(pet_program_timers_t *t,
    uint64_t generation,uint64_t now,uint64_t sequence,
    pet_program_timer_enqueue_t enqueue,void *context,pet_program_timer_diagnostic_t *out){
  if(!out||!enqueue||!sequence||overlap(t,sizeof(*t),out,sizeof(*out)))return PET_TIMERS_INVALID;
  pet_program_timers_result_t status=enter(t,generation);if(status!=PET_TIMERS_OK)return status;
  clear(out,sizeof(*out));out->enqueue_result=PET_RUNTIME_IDLE;
  if(t->cancelled)return PET_TIMERS_STOPPED;
  if(now<t->last_now){t->cancelled=true;return PET_TIMERS_CLOCK_ERROR;}
  if(sequence<=t->last_sequence){t->cancelled=true;return PET_TIMERS_SEQUENCE_ERROR;}
  t->last_now=now;
  unsigned index=t->count;
  for(unsigned n=0;n<t->count;n++){
    unsigned i=(t->cursor+n)%t->count;
    if((t->active_mask&(1U<<i))&&t->deadlines[i]<=now){index=i;break;}
  }
  if(index==t->count)return PET_TIMERS_IDLE;
  out->timer_id=t->specs[index].id;out->offered=true;t->inside=true;
  pet_program_runtime_result_t result=enqueue(context,generation,sequence,&t->specs[index].event);
  t->inside=false;out->enqueue_result=result;
  if(result==PET_RUNTIME_QUEUE_FULL||result==PET_RUNTIME_BUSY)return PET_TIMERS_BACKPRESSURE;
  if(result!=PET_RUNTIME_OK){
    t->cancelled=true;
    return result==PET_RUNTIME_DUPLICATE?PET_TIMERS_SEQUENCE_ERROR:PET_TIMERS_STOPPED;
  }
  out->accepted=true;t->accepted=saturate(t->accepted,1);t->last_sequence=sequence;
  t->cursor=(uint8_t)((index+1U)%t->count);
  uint32_t interval=t->specs[index].interval_ms;
  if(!interval)t->active_mask&=(uint8_t)~(1U<<index);
  else{
    uint64_t skipped=(now-t->deadlines[index])/interval;
    out->coalesced=saturate(0,skipped);t->missed=saturate(t->missed,skipped);
    /* Phase-aligned next time computed without overflowing a multiplication. */
    uint64_t remaining=interval-(now-t->deadlines[index])%interval;
    if(now>UINT64_MAX-remaining){
      t->active_mask&=(uint8_t)~(1U<<index);out->deadline_exhausted=true;
    }else t->deadlines[index]=now+remaining;
  }
  return PET_TIMERS_ENQUEUED;
}
pet_program_timers_result_t pet_program_timers_cancel(pet_program_timers_t *t,uint64_t generation){
  pet_program_timers_result_t status=enter(t,generation);if(status!=PET_TIMERS_OK)return status;
  t->cancelled=true;return PET_TIMERS_OK;
}
