#include "pet_p4_program_group.h"
#include "pet_p4_program_scheduler.h"
#include <limits.h>

#define GROUP_MARKER UINT32_C(0x50344731)
static bool overlap(const void *a,size_t an,const void *b,size_t bn){
  if(!a||!b)return false;
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
static bool same_owner(pet_resource_owner_t a,pet_resource_owner_t b){
  return a.broker==b.broker&&a.generation==b.generation&&a.slot==b.slot;
}
static bool aliases(const pet_program_group_t *g,const void *p,size_t n){
  if(overlap(g,sizeof(*g),p,n))return true;
  return g&&g->marker==GROUP_MARKER
    &&(overlap(g->host,sizeof(*g->host),p,n)
      ||overlap(g->timers,sizeof(*g->timers),p,n)||overlap(g->bus,sizeof(*g->bus),p,n));
}
static bool registration(const pet_event_bus_t *b,pet_event_registration_t r,pet_resource_owner_t o){
  return b&&r.bus==b&&r.serial&&r.slot<PET_EVENT_BUS_OWNERS&&b->marker&&!b->inside
    &&b->slots[r.slot].live&&!b->slots[r.slot].stopped&&b->slots[r.slot].serial==r.serial
    &&same_owner(b->slots[r.slot].owner,o);
}
bool pet_program_group_attach(pet_program_group_t *g,pet_program_host_t *h,
    pet_program_timers_t *t,pet_event_bus_t *b,pet_event_registration_t r){
  if(!g||!h||overlap(g,sizeof(*g),h,sizeof(*h))||overlap(g,sizeof(*g),t,sizeof(*t))
      ||overlap(g,sizeof(*g),b,sizeof(*b))||overlap(h,sizeof(*h),t,sizeof(*t))
      ||overlap(h,sizeof(*h),b,sizeof(*b))||overlap(t,sizeof(*t),b,sizeof(*b)))return false;
  if(g->instance||g->marker||g->host||g->timers||g->bus||g->inside||g->stopped||g->retired||g->stop_requested||g->generation||g->sequence||g->scheduler_owner
      ||g->subscription.bus||g->subscription.serial||g->subscription.slot
      ||g->owner.broker||g->owner.generation||g->owner.slot)return false;
  if(h->instance!=h||h->runtime.instance!=&h->runtime||!h->marker||h->inside||h->runtime.inside||h->group_owner||!h->bound||!h->runtime.active
      ||!h->owner.broker||!h->owner.generation||h->owner.slot>=PET_RESOURCE_OWNER_SLOTS
      ||h->runtime.generation!=h->owner.generation||!same_owner(h->lease.owner,h->owner)
      ||(h->admission_kind!=PET_RESOURCE_PROGRAM&&h->admission_kind!=PET_RESOURCE_NATIVE_PROBE)
      ||h->lease.kind!=h->admission_kind||!h->lease.serial||!h->lease.units)return false;
  if(t&&(t->instance!=t||!t->marker||t->inside||t->cancelled||t->generation!=h->owner.generation
      ||!t->count||t->count>PET_PROGRAM_TIMERS||t->last_sequence>h->runtime.last_sequence))return false;
  if(b?!registration(b,r,h->owner):(r.bus||r.serial||r.slot))return false;
  if(b&&b->slots[r.slot].last_delivery>h->runtime.last_sequence)return false;
  *g=(pet_program_group_t){.instance=g,.host=h,.timers=t,.bus=b,.subscription=r,.owner=h->owner,
    .generation=h->owner.generation,.sequence=h->runtime.last_sequence,.marker=GROUP_MARKER};
  h->group_owner=g;
  return true;
}
static pet_program_runtime_result_t leave(pet_program_group_t *g,pet_program_runtime_result_t r){
  g->inside=false;return r;
}
/* Called only inside the group's parent serialization/reentry boundary. */
static pet_program_runtime_result_t retire(pet_program_group_t *g){
  g->stopped=true;bool done=true;
  if(g->timers&&g->timers->generation==g->generation){
    pet_program_timers_result_t r=pet_program_timers_cancel(g->timers,g->generation);
    done=r==PET_TIMERS_OK||r==PET_TIMERS_STALE;
  }
  if(g->bus){
    pet_event_bus_result_t r=pet_event_bus_close(g->bus,g->subscription);
    done=(r==PET_EVENT_OK||r==PET_EVENT_STALE)&&done;
  }
  if(g->host->runtime.generation==g->generation&&g->host->group_owner==g){
    pet_program_runtime_result_t r=pet_program_host_cancel(g->host,g->generation);
    done=(r==PET_RUNTIME_OK||r==PET_RUNTIME_STALE)&&done;
  }
  g->retired=done;return done?PET_RUNTIME_OK:PET_RUNTIME_BUSY;
}
static pet_program_runtime_result_t enter(pet_program_group_t *g,uint64_t generation,bool live){
  if(!g||g->instance!=g||g->marker!=GROUP_MARKER||!g->host)return PET_RUNTIME_INVALID;
  if(g->inside)return PET_RUNTIME_BUSY;
  if(!generation||generation!=g->generation)return PET_RUNTIME_STALE;
  if(g->host->inside||g->host->runtime.inside||(g->timers&&g->timers->inside)||(g->bus&&g->bus->inside))return PET_RUNTIME_BUSY;
  if(live&&g->stopped)return PET_RUNTIME_STOPPED;
  g->inside=true;
  if(g->host->runtime.generation!=g->generation||g->host->group_owner!=g){
    (void)retire(g);return leave(g,PET_RUNTIME_STALE);
  }
  if(live&&(g->stop_requested||(g->scheduler_owner&&g->scheduler_owner->stopped))){
    (void)retire(g);return leave(g,PET_RUNTIME_STOPPED);
  }
  if(live){
    pet_program_runtime_result_t r=pet_program_host_check_current(g->host,g->generation);
    if(r!=PET_RUNTIME_OK){(void)retire(g);return leave(g,r);}
  }
  return PET_RUNTIME_OK;
}
pet_program_runtime_result_t pet_program_group_check_current(pet_program_group_t *g,uint64_t generation){
  pet_program_runtime_result_t r=enter(g,generation,true);
  return r==PET_RUNTIME_OK?leave(g,r):r;
}
static void stopped_host(pet_program_group_t *g){
  if(!g->host->runtime.active||!g->host->bound)(void)retire(g);
}
static bool next_sequence(pet_program_group_t *g){
  if(g->sequence<UINT64_MAX)return true;
  (void)retire(g);return false;
}
pet_program_runtime_result_t pet_program_group_enqueue(pet_program_group_t *g,uint64_t generation,
    const pet_program_event_t *event,uint64_t *sequence){
  if(!event||!sequence||aliases(g,event,sizeof(*event))||aliases(g,sequence,sizeof(*sequence))
      ||overlap(event,sizeof(*event),sequence,sizeof(*sequence))||event->count>PET_PROGRAM_EVENT_WORDS)return PET_RUNTIME_INVALID;
  pet_program_runtime_result_t r=enter(g,generation,true);if(r!=PET_RUNTIME_OK)return r;
  if(!next_sequence(g))return leave(g,PET_RUNTIME_BAD_STATE);
  r=pet_program_host_enqueue(g->host,generation,g->sequence+1,event);
  if(r==PET_RUNTIME_OK)*sequence=++g->sequence;
  else if(r!=PET_RUNTIME_BUSY&&r!=PET_RUNTIME_QUEUE_FULL)(void)retire(g);
  stopped_host(g);return leave(g,r);
}
static pet_program_runtime_result_t timer_enqueue(void *context,uint64_t generation,
    uint64_t sequence,const pet_program_event_t *event){
  pet_program_group_t *g=context;
  if(!g->inside||generation!=g->generation||sequence!=g->sequence+1)return PET_RUNTIME_INVALID;
  return pet_program_host_enqueue(g->host,generation,sequence,event);
}
static pet_program_timers_result_t timer_status(pet_program_runtime_result_t r){
  if(r==PET_RUNTIME_BUSY)return PET_TIMERS_BUSY;
  if(r==PET_RUNTIME_STALE)return PET_TIMERS_STALE;
  if(r==PET_RUNTIME_INVALID)return PET_TIMERS_INVALID;
  return PET_TIMERS_STOPPED;
}
pet_program_timers_result_t pet_program_group_tick(pet_program_group_t *g,uint64_t generation,
    uint64_t now,pet_program_timer_diagnostic_t *out){
  if(!out||aliases(g,out,sizeof(*out)))return PET_TIMERS_INVALID;
  pet_program_runtime_result_t status=enter(g,generation,true);if(status!=PET_RUNTIME_OK)return timer_status(status);
  if(!g->timers){leave(g,PET_RUNTIME_OK);return PET_TIMERS_INVALID;}
  if(!next_sequence(g)){leave(g,PET_RUNTIME_OK);return PET_TIMERS_SEQUENCE_ERROR;}
  pet_program_timers_result_t r=pet_program_timers_tick(g->timers,generation,now,g->sequence+1,timer_enqueue,g,out);
  if(r==PET_TIMERS_ENQUEUED)g->sequence++;
  else if(r!=PET_TIMERS_IDLE&&r!=PET_TIMERS_BACKPRESSURE&&r!=PET_TIMERS_BUSY)(void)retire(g);
  stopped_host(g);leave(g,PET_RUNTIME_OK);return r;
}
static bool receiver_current(void *context,pet_resource_owner_t owner){
  pet_program_group_t *g=context;
  return g->inside&&same_owner(owner,g->owner)
    &&pet_program_host_check_current(g->host,g->generation)==PET_RUNTIME_OK;
}
static pet_program_runtime_result_t bus_enqueue(void *context,pet_resource_owner_t owner,
    uint64_t sequence,const pet_event_message_t *message){
  pet_program_group_t *g=context;
  if(!g->inside||!same_owner(owner,g->owner)||sequence!=g->sequence+1)return PET_RUNTIME_INVALID;
  return pet_program_host_enqueue(g->host,g->generation,sequence,&message->event);
}
static pet_event_bus_result_t bus_status(pet_program_runtime_result_t r){
  if(r==PET_RUNTIME_BUSY)return PET_EVENT_BUSY;
  if(r==PET_RUNTIME_STALE)return PET_EVENT_STALE;
  if(r==PET_RUNTIME_INVALID)return PET_EVENT_INVALID;
  return PET_EVENT_STOPPED;
}
pet_event_bus_result_t pet_program_group_deliver(pet_program_group_t *g,uint64_t generation,
    pet_event_diagnostic_t *out){
  if(!out||aliases(g,out,sizeof(*out)))return PET_EVENT_INVALID;
  pet_program_runtime_result_t status=enter(g,generation,true);if(status!=PET_RUNTIME_OK)return bus_status(status);
  if(!g->bus){leave(g,PET_RUNTIME_OK);return PET_EVENT_INVALID;}
  if(!next_sequence(g)){leave(g,PET_RUNTIME_OK);return PET_EVENT_SEQUENCE_ERROR;}
  pet_event_bus_result_t r=pet_event_bus_deliver(g->bus,g->subscription,g->sequence+1,
    receiver_current,g,bus_enqueue,g,out);
  if(r==PET_EVENT_DELIVERED)g->sequence++;
  else if(r!=PET_EVENT_IDLE&&r!=PET_EVENT_BACKPRESSURE&&r!=PET_EVENT_BUSY)(void)retire(g);
  stopped_host(g);leave(g,PET_RUNTIME_OK);return r;
}
pet_program_runtime_result_t pet_program_group_pump(pet_program_group_t *g,uint64_t generation){
  pet_program_runtime_result_t r=enter(g,generation,true);if(r!=PET_RUNTIME_OK)return r;
  r=pet_program_host_pump(g->host,generation);stopped_host(g);return leave(g,r);
}
pet_program_runtime_result_t pet_program_group_cancel(pet_program_group_t *g,uint64_t generation){
  pet_program_runtime_result_t r=enter(g,generation,false);if(r!=PET_RUNTIME_OK)return r;
  return leave(g,retire(g));
}
pet_program_runtime_result_t pet_program_group_snapshot(pet_program_group_t *g,uint64_t generation,
    pet_program_snapshot_t *out){
  if(!out||aliases(g,out,sizeof(*out)))return PET_RUNTIME_INVALID;
  pet_program_runtime_result_t r=enter(g,generation,false);if(r!=PET_RUNTIME_OK)return r;
  return leave(g,pet_program_host_snapshot(g->host,generation,out));
}
pet_program_runtime_result_t pet_program_group_take_report(pet_program_group_t *g,uint64_t generation,
    pet_program_report_t *out){
  if(!out||aliases(g,out,sizeof(*out)))return PET_RUNTIME_INVALID;
  pet_program_runtime_result_t r=enter(g,generation,false);if(r!=PET_RUNTIME_OK)return r;
  return leave(g,pet_program_host_take_report(g->host,generation,out));
}
pet_program_runtime_result_t pet_program_group_project(pet_program_group_t *g,uint64_t generation,
    const int32_t base[PET_PROGRAM_UI_VARIABLES],pet_program_ui_snapshot_t *out){
  if(!base||!out||aliases(g,base,sizeof(int32_t)*PET_PROGRAM_UI_VARIABLES)||aliases(g,out,sizeof(*out))
      ||overlap(base,sizeof(int32_t)*PET_PROGRAM_UI_VARIABLES,out,sizeof(*out)))return PET_RUNTIME_INVALID;
  pet_program_runtime_result_t r=enter(g,generation,true);if(r!=PET_RUNTIME_OK)return r;
  r=pet_program_host_project_current(g->host,generation,base,out);stopped_host(g);return leave(g,r);
}
