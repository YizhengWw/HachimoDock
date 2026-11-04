#include "pet_p4_program_scheduler.h"
#include <limits.h>
#include <string.h>
#define SCHED_MARKER UINT32_C(0x50345331)
static bool overlap(const void*a,size_t an,const void*b,size_t bn){
  if(!a||!b)return false;
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
static bool group_alias(const pet_program_group_t*g,const void*p,size_t n){
  return g&&(overlap(g,sizeof(*g),p,n)||overlap(g->host,sizeof(*g->host),p,n)
    ||overlap(g->timers,sizeof(*g->timers),p,n)||overlap(g->bus,sizeof(*g->bus),p,n));
}
static bool aliases(const pet_program_scheduler_t*s,const void*p,size_t n){
  if(overlap(s,sizeof(*s),p,n))return true;
  if(s&&s->marker==SCHED_MARKER)for(unsigned i=0;i<PET_PROGRAM_SCHEDULER_SLOTS;i++)
    if(group_alias(s->slots[i].group,p,n))return true;
  return false;
}
static pet_program_schedule_result_t enter(pet_program_scheduler_t*s){
  if(!s||s->instance!=s||s->marker!=SCHED_MARKER)return PET_SCHED_INVALID;
  if(s->inside)return PET_SCHED_BUSY;
  s->inside=true;return PET_SCHED_OK;
}
bool pet_program_scheduler_disjoint(const pet_program_scheduler_t*s,const void*p,size_t n){
  return s&&s->instance==s&&s->marker==SCHED_MARKER&&p&&n&&!aliases(s,p,n);
}
static pet_program_schedule_result_t leave(pet_program_scheduler_t*s,pet_program_schedule_result_t r){
  s->inside=false;return r;
}
bool pet_program_scheduler_init(pet_program_scheduler_t*s){
  if(!s||s->instance||s->marker||s->next_serial||s->last_now||s->cursor||s->inside||s->stopped||s->clock_seen)return false;
  for(unsigned i=0;i<PET_PROGRAM_SCHEDULER_SLOTS;i++)
    if(s->slots[i].group||s->slots[i].generation||s->slots[i].serial||s->slots[i].bus_next||s->slots[i].retiring)return false;
  s->instance=s;s->marker=SCHED_MARKER;return true;
}
static pet_program_schedule_t ticket(pet_program_scheduler_t*s,unsigned slot){
  return (pet_program_schedule_t){s,s->slots[slot].serial,(uint8_t)slot};
}
pet_program_schedule_result_t pet_program_scheduler_rearm(pet_program_scheduler_t*s){
  pet_program_schedule_result_t status=enter(s);if(status!=PET_SCHED_OK)return status;
  if(!s->stopped)return leave(s,PET_SCHED_INVALID);
  for(unsigned i=0;i<PET_PROGRAM_SCHEDULER_SLOTS;i++)
    if(s->slots[i].group)return leave(s,PET_SCHED_BUSY);
  if(s->next_serial==UINT64_MAX)return leave(s,PET_SCHED_EXHAUSTED);
  s->stopped=false;s->clock_seen=false;s->last_now=0;
  return leave(s,PET_SCHED_OK);
}
pet_program_schedule_result_t pet_program_scheduler_add(pet_program_scheduler_t*s,
    pet_program_group_t*g,uint64_t generation,pet_program_schedule_t*out){
  if(!g||!out||!generation||aliases(s,out,sizeof(*out))||group_alias(g,s,sizeof(*s))
      ||group_alias(g,out,sizeof(*out))||out->scheduler||out->serial||out->slot)return PET_SCHED_INVALID;
  pet_program_schedule_result_t status=enter(s);if(status!=PET_SCHED_OK)return status;
  if(s->stopped)return leave(s,PET_SCHED_STOPPED);
  if(g->scheduler_owner)return leave(s,PET_SCHED_INVALID);
  unsigned slot=PET_PROGRAM_SCHEDULER_SLOTS;
  for(unsigned i=0;i<PET_PROGRAM_SCHEDULER_SLOTS;i++){
    if(s->slots[i].group==g)return leave(s,PET_SCHED_INVALID);
    if(!s->slots[i].group&&slot==PET_PROGRAM_SCHEDULER_SLOTS)slot=i;
  }
  if(slot==PET_PROGRAM_SCHEDULER_SLOTS)return leave(s,PET_SCHED_FULL);
  if(s->next_serial==UINT64_MAX)return leave(s,PET_SCHED_EXHAUSTED);
  pet_program_runtime_result_t r=pet_program_group_check_current(g,generation);
  if(r!=PET_RUNTIME_OK)return leave(s,r==PET_RUNTIME_BUSY?PET_SCHED_BUSY:r==PET_RUNTIME_STALE?PET_SCHED_STALE:PET_SCHED_INVALID);
  s->slots[slot]=(pet_program_schedule_slot_t){.group=g,.generation=generation,.serial=++s->next_serial};
  g->scheduler_owner=s;*out=ticket(s,slot);return leave(s,PET_SCHED_OK);
}
static pet_program_schedule_result_t retire(pet_program_scheduler_t*s,unsigned slot){
  pet_program_schedule_slot_t*entry=&s->slots[slot];
  entry->retiring=true;
  if(entry->group->scheduler_owner==s&&entry->group->generation==entry->generation)
    entry->group->stop_requested=true;
  pet_program_runtime_result_t r=pet_program_group_cancel(entry->group,entry->generation);
  if((r==PET_RUNTIME_OK||r==PET_RUNTIME_STALE)&&entry->group->retired){
    *entry=(pet_program_schedule_slot_t){0};return PET_SCHED_OK;
  }
  return PET_SCHED_BUSY; /* Retain physical retirement uncertainty and occupancy. */
}
pet_program_schedule_result_t pet_program_scheduler_remove(pet_program_scheduler_t*s,pet_program_schedule_t t){
  pet_program_schedule_result_t status=enter(s);if(status!=PET_SCHED_OK)return status;
  if(t.scheduler!=s||!t.serial||t.slot>=PET_PROGRAM_SCHEDULER_SLOTS
      ||!s->slots[t.slot].group||s->slots[t.slot].serial!=t.serial)return leave(s,PET_SCHED_STALE);
  return leave(s,retire(s,t.slot));
}
static pet_program_schedule_result_t stop(pet_program_scheduler_t*s){
  s->stopped=true;bool done=true;
  for(unsigned i=0;i<PET_PROGRAM_SCHEDULER_SLOTS;i++)
    if(s->slots[i].group&&retire(s,i)!=PET_SCHED_OK)done=false;
  return done?PET_SCHED_STOPPED:PET_SCHED_STOPPING;
}
pet_program_schedule_result_t pet_program_scheduler_stop(pet_program_scheduler_t*s){
  pet_program_schedule_result_t status=enter(s);if(status!=PET_SCHED_OK)return status;
  return leave(s,stop(s));
}
pet_program_schedule_result_t pet_program_scheduler_step(pet_program_scheduler_t*s,
    uint64_t now,pet_program_schedule_diagnostic_t*out){
  if(!out||aliases(s,out,sizeof(*out)))return PET_SCHED_INVALID;
  pet_program_schedule_result_t status=enter(s);if(status!=PET_SCHED_OK)return status;
  pet_program_schedule_diagnostic_t d={0};
  if(s->stopped){status=stop(s);d.stopped=true;*out=d;return leave(s,status);}
  if(s->clock_seen&&now<s->last_now){(void)stop(s);d.stopped=true;*out=d;return leave(s,PET_SCHED_CLOCK_ERROR);}
  s->last_now=now;s->clock_seen=true;
  for(unsigned n=0;n<PET_PROGRAM_SCHEDULER_SLOTS;n++){
    unsigned i=s->cursor;s->cursor=(uint8_t)((i+1)%PET_PROGRAM_SCHEDULER_SLOTS);
    pet_program_schedule_slot_t*entry=&s->slots[i];if(!entry->group)continue;
    pet_program_group_t*g=entry->group;d.schedule=ticket(s,i);d.generation=entry->generation;d.selected=true;
    if(entry->retiring){d.removed=retire(s,i)==PET_SCHED_OK;d.stopped=true;*out=d;return leave(s,PET_SCHED_TURN);}
    if(g->timers&&(!g->bus||!entry->bus_next)){
      pet_program_timer_diagnostic_t td={0};d.ingress=PET_SCHED_TIMER;
      d.timer_result=pet_program_group_tick(g,entry->generation,now,&td);
    }else if(g->bus){
      pet_event_diagnostic_t bd={0};d.ingress=PET_SCHED_BUS;
      d.bus_result=pet_program_group_deliver(g,entry->generation,&bd);
    }
    entry->bus_next=!entry->bus_next;
    if(!g->stopped){d.pump_attempted=true;d.pump_result=pet_program_group_pump(g,entry->generation);}
    if(g->stopped||d.pump_result==PET_RUNTIME_STALE||d.pump_result==PET_RUNTIME_INVALID
        ||d.pump_result==PET_RUNTIME_BAD_STATE){d.removed=retire(s,i)==PET_SCHED_OK;}
    d.stopped=g->stopped;*out=d;return leave(s,PET_SCHED_TURN);
  }
  *out=d;return leave(s,PET_SCHED_IDLE);
}
