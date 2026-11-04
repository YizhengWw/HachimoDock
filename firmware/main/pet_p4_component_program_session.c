#include "pet_p4_component_program_session.h"
#if PET_COMPONENT_PROGRAM_ENABLED
#include <string.h>
#include <stdint.h>
static bool overlap(const void *a,size_t an,const void *b,size_t bn){uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;}
static bool owned(const pet_component_program_session_t *s){
  return s&&s->host.instance==&s->host&&s->host.runtime.instance==&s->host.runtime
    &&(!s->scheduler.instance||s->scheduler.instance==&s->scheduler)
    &&(!s->dispatch||s->dispatch->instance==s->dispatch)
    &&(!s->group.instance||(s->group.instance==&s->group&&s->group.host==&s->host));
}
bool pet_component_program_session_init(pet_component_program_session_t *s,const pet_program_host_backend_t *backend){
  if(!s||s->generation||s->revision||s->sequence||s->live||s->report_valid||s->clocked||s->retiring||s->starting||s->stop_requested
      ||s->scheduled||s->dispatch||s->scheduler.instance||s->group.instance||s->schedule.scheduler||s->schedule.serial)return false;
  return pet_program_host_init(&s->host,backend);
}
static pet_program_scheduler_t *dispatcher(pet_component_program_session_t*s){return s->dispatch?s->dispatch:&s->scheduler;}
bool pet_component_program_session_bind_scheduler(pet_component_program_session_t*s,pet_program_scheduler_t*d){
  if(!owned(s)||!d||overlap(s,sizeof(*s),d,sizeof(*d))||s->dispatch||s->generation||s->starting||s->live||s->retiring
      ||s->scheduled||s->host.inside||s->host.admission_attempted||s->host.bound||s->host.group_owner
      ||s->group.instance||s->scheduler.instance||d->instance!=d||d->inside||d->stopped)return false;
  s->dispatch=d;return true;
}
static void retain_report(pet_component_program_session_t *s){
  pet_program_report_t report;
  if(s->generation&&pet_program_host_take_report(&s->host,s->generation,&report)==PET_RUNTIME_OK){s->last_report=report;s->report_valid=true;}
}
pet_program_runtime_result_t pet_component_program_session_stop(pet_component_program_session_t *s){
  if(!owned(s))return PET_RUNTIME_INVALID;
  s->stop_requested=true;
  if(s->starting){s->live=false;s->retiring=true;return PET_RUNTIME_BUSY;}
  if(!s->generation){s->live=false;return PET_RUNTIME_IDLE;}
  s->live=false;s->retiring=true;
  pet_program_runtime_result_t result;
  if(s->scheduled){
    s->group.stop_requested=true; /* Latch even when the backend is inside work. */
    pet_program_schedule_result_t removed=pet_program_scheduler_remove(dispatcher(s),s->schedule);
    if((removed==PET_SCHED_OK||removed==PET_SCHED_STALE)&&s->group.retired){
      s->scheduled=false;s->schedule=(pet_program_schedule_t){0};result=PET_RUNTIME_OK;
    }else result=PET_RUNTIME_BUSY;
  }else{
    if(s->clocked)(void)pet_program_timers_cancel(&s->timers,s->generation);
    result=pet_program_host_cancel(&s->host,s->generation);
  }
  if(result==PET_RUNTIME_OK){retain_report(s);s->retiring=false;}
  s->phase=result;return result;
}
pet_program_runtime_result_t pet_component_program_session_start(pet_component_program_session_t *s,
    const pet_component_program_config_t *c,const pet_program_snapshot_t *seed,uint64_t revision,uint64_t now){
  if(!owned(s)||!c||!c->enabled||!seed||!revision||s->live||s->retiring||s->starting||s->host.inside||s->scheduled
      ||(s->group.instance&&!s->group.retired)
      ||c->timer_count>PET_PROGRAM_TIMERS
      ||overlap(s,sizeof(*s),c,sizeof(*c))||overlap(s,sizeof(*s),seed,sizeof(*seed)))return PET_RUNTIME_INVALID;
  for(unsigned i=0;i<c->timer_count;i++)if(UINT64_MAX-now<c->timers[i].delay_ms)return PET_RUNTIME_INVALID;
  if(s->dispatch&&s->dispatch->inside){s->phase=PET_RUNTIME_BUSY;return s->phase;}
  if(s->dispatch&&s->dispatch->stopped){s->phase=PET_RUNTIME_STOPPED;return s->phase;}
  uint64_t generation=0;
  s->starting=true;s->stop_requested=false;
  pet_program_runtime_result_t result=pet_program_host_start_image_with_projection(&s->host,&c->image,64,seed,&c->projection,&generation);
  s->starting=false;
  if(result!=PET_RUNTIME_OK){s->retiring=s->stop_requested&&s->host.bound;s->phase=result;return result;}
  s->generation=generation;s->revision=revision;s->sequence=0;s->live=true;s->clocked=false;
  /* Cancellation observed during admission is not overwritten by successful
   * startup. Retire the now-known exact owner before any timer/input or OK. */
  if(s->stop_requested){(void)pet_component_program_session_stop(s);s->phase=PET_RUNTIME_STOPPED;return s->phase;}
  if(!s->dispatch&&((!s->scheduler.instance&&!pet_program_scheduler_init(&s->scheduler))
      ||(s->scheduler.stopped&&pet_program_scheduler_rearm(&s->scheduler)!=PET_SCHED_OK))){
    (void)pet_component_program_session_stop(s);s->phase=PET_RUNTIME_BAD_STATE;return s->phase;
  }
  /* Timers are private to this session; all old invocation paths were retired
   * before the host accepted this strictly newer native generation. */
  memset(&s->timers,0,sizeof(s->timers));
  if(c->timer_count){
    if(pet_program_timers_start(&s->timers,generation,now,c->timers,c->timer_count)!=PET_TIMERS_OK){
      (void)pet_component_program_session_stop(s);s->phase=PET_RUNTIME_BAD_STATE;return s->phase;
    }s->clocked=true;
  }
  /* A new native owner may reuse this PRIVATE, fully retired group only after
   * its exact old registration was removed. Scheduler serials never reset. */
  memset(&s->group,0,sizeof(s->group));
  if(!pet_program_group_attach(&s->group,&s->host,s->clocked?&s->timers:NULL,NULL,(pet_event_registration_t){0})
      ||pet_program_scheduler_add(dispatcher(s),&s->group,generation,&s->schedule)!=PET_SCHED_OK){
    if(s->group.instance)(void)pet_program_group_cancel(&s->group,generation);
    (void)pet_component_program_session_stop(s);s->phase=PET_RUNTIME_BAD_STATE;return s->phase;
  }
  s->scheduled=true;
  /* Scheduler admission rechecks the native backend. A stop observed by that
   * callback must retire this now-registered owner, not escape as success. */
  if(s->stop_requested||!s->live){
    (void)pet_component_program_session_stop(s);s->phase=PET_RUNTIME_STOPPED;return s->phase;
  }
  s->phase=PET_RUNTIME_OK;return PET_RUNTIME_OK;
}
bool pet_component_program_session_can_enqueue(const pet_component_program_session_t *s,uint64_t revision){
  return owned(s)&&!s->starting&&!s->host.inside&&(!s->dispatch||!s->dispatch->inside)&&s->live&&s->scheduled&&!s->group.stopped
    &&!(s->dispatch?s->dispatch:&s->scheduler)->stopped
    &&!s->retiring&&!s->stop_requested&&s->revision==revision&&s->sequence<UINT64_MAX
    &&s->host.runtime.active&&s->host.runtime.queued<PET_PROGRAM_QUEUE;
}
pet_program_runtime_result_t pet_component_program_session_enqueue(pet_component_program_session_t *s,
    uint64_t revision,const pet_program_event_t *event){
  if(!owned(s)||!event||overlap(s,sizeof(*s),event,sizeof(*event)))return PET_RUNTIME_INVALID;
  if(s->starting||s->host.inside||(s->dispatch&&s->dispatch->inside))return PET_RUNTIME_BUSY;
  if(!s->live||revision!=s->revision)return PET_RUNTIME_STALE;
  if(s->sequence==UINT64_MAX){(void)pet_component_program_session_stop(s);return PET_RUNTIME_DENIED;}
  uint64_t sequence=0;
  pet_program_runtime_result_t result=pet_program_group_enqueue(&s->group,s->generation,event,&sequence);
  if(result==PET_RUNTIME_OK)s->sequence=sequence;
  else if(result!=PET_RUNTIME_BUSY&&result!=PET_RUNTIME_QUEUE_FULL){(void)pet_component_program_session_stop(s);}
  s->phase=result;return result;
}
bool pet_component_program_session_step(pet_component_program_session_t *s,uint64_t revision,
    uint64_t now,const int32_t base[8],int32_t outputs[8]){
  if(!owned(s)||s->dispatch||s->starting||s->host.inside||!base||!outputs||overlap(s,sizeof(*s),base,8*sizeof(*base))||overlap(s,sizeof(*s),outputs,8*sizeof(*outputs))
      ||overlap(base,8*sizeof(*base),outputs,8*sizeof(*outputs)))return false;
  if(s->retiring){(void)pet_component_program_session_stop(s);return false;}
  if(!s->live||revision!=s->revision)return false;
  if(pet_program_host_check_current(&s->host,s->generation)!=PET_RUNTIME_OK){(void)pet_component_program_session_stop(s);return false;}
  if(!s->scheduled||s->sequence==UINT64_MAX){(void)pet_component_program_session_stop(s);return false;}
  pet_program_schedule_diagnostic_t diagnostic={0};
  pet_program_schedule_result_t turn=pet_program_scheduler_step(&s->scheduler,now,&diagnostic);
  s->sequence=s->group.sequence;
  if(turn!=PET_SCHED_TURN||!diagnostic.pump_attempted||diagnostic.stopped){
    (void)pet_component_program_session_stop(s);return false;
  }
  s->phase=diagnostic.pump_result;
  if(s->phase==PET_RUNTIME_COMPLETE){
    retain_report(s);
    if(s->report_valid&&s->last_report.generation==s->generation&&s->last_report.result==PET_PROGRAM_COMMITTED){
      pet_program_ui_snapshot_t next;
      if(pet_program_host_project_current(&s->host,s->generation,base,&next)==PET_RUNTIME_OK){memcpy(outputs,next.values,8*sizeof(*outputs));return true;}
    }
    (void)pet_component_program_session_stop(s);return false;
  }
  if(s->phase==PET_RUNTIME_STOPPED||s->phase==PET_RUNTIME_DENIED||s->phase==PET_RUNTIME_STALE)(void)pet_component_program_session_stop(s);
  return false;
}
bool pet_component_program_session_collect(pet_component_program_session_t*s,uint64_t revision,
    const int32_t base[8],int32_t outputs[8]){
  if(!owned(s)||!s->dispatch||s->dispatch->inside||s->starting||s->host.inside||!base||!outputs
      ||overlap(s,sizeof(*s),base,8*sizeof(*base))||overlap(s,sizeof(*s),outputs,8*sizeof(*outputs))
      ||overlap(base,8*sizeof(*base),outputs,8*sizeof(*outputs))
      ||!pet_program_scheduler_disjoint(s->dispatch,base,8*sizeof(*base))
      ||!pet_program_scheduler_disjoint(s->dispatch,outputs,8*sizeof(*outputs)))return false;
  if(s->retiring){(void)pet_component_program_session_stop(s);return false;}
  if(!s->live||revision!=s->revision)return false;
  s->sequence=s->group.sequence;
  if(pet_program_host_check_current(&s->host,s->generation)!=PET_RUNTIME_OK){
    (void)pet_component_program_session_stop(s);return false;
  }
  pet_program_report_t report;
  pet_program_runtime_result_t result=pet_program_host_take_report(&s->host,s->generation,&report);
  if(result==PET_RUNTIME_IDLE||result==PET_RUNTIME_BUSY)return false;
  if(result!=PET_RUNTIME_OK){(void)pet_component_program_session_stop(s);return false;}
  s->last_report=report;s->report_valid=true;s->phase=PET_RUNTIME_COMPLETE;
  if(report.generation==s->generation&&report.result==PET_PROGRAM_COMMITTED){
    pet_program_ui_snapshot_t next;
    if(pet_program_host_project_current(&s->host,s->generation,base,&next)==PET_RUNTIME_OK){
      memcpy(outputs,next.values,8*sizeof(*outputs));return true;
    }
  }
  (void)pet_component_program_session_stop(s);return false;
}
bool pet_component_program_session_snapshot(pet_component_program_session_t *s,pet_program_snapshot_t *out){
  return owned(s)&&!s->starting&&!s->host.inside&&s->generation&&out&&!overlap(s,sizeof(*s),out,sizeof(*out))
    &&pet_program_host_snapshot(&s->host,s->generation,out)==PET_RUNTIME_OK;
}
pet_program_runtime_result_t pet_component_program_session_snapshot_if_changed(
    pet_component_program_session_t *s,const pet_program_snapshot_cursor_t *known,
    pet_program_snapshot_t *out,pet_program_snapshot_cursor_t *cursor){
  if(!owned(s)||s->starting||s->host.inside||!s->generation||!out||!cursor
      ||overlap(s,sizeof(*s),out,sizeof(*out))||overlap(s,sizeof(*s),cursor,sizeof(*cursor))
      ||(known&&overlap(s,sizeof(*s),known,sizeof(*known))))return PET_RUNTIME_INVALID;
  return pet_program_host_snapshot_if_changed(&s->host,s->generation,known,out,cursor);
}
#if PET_COMPONENT_PROGRAM_RAM_RESUME
bool pet_component_program_session_resume_snapshot(pet_component_program_session_t *s,
    const pet_component_program_config_t *c,uint64_t revision,pet_program_snapshot_t *out){
  if(!owned(s)||!c||!out||!revision||s->revision!=revision||!s->generation
      ||s->live||s->retiring||s->starting||s->scheduled||s->host.inside||s->host.bound
      ||!s->group.instance||!s->group.retired||(s->dispatch&&s->dispatch->inside)
      ||!s->host.projection_bound||memcmp(&s->host.runtime.image,&c->image,sizeof(c->image))
      ||memcmp(&s->host.projection,&c->projection,sizeof(c->projection))
      ||overlap(s,sizeof(*s),out,sizeof(*out))||overlap(c,sizeof(*c),out,sizeof(*out))
      ||(s->dispatch&&!pet_program_scheduler_disjoint(s->dispatch,out,sizeof(*out))))return false;
  pet_program_snapshot_t next;
  if(!pet_component_program_session_snapshot(s,&next)||!pet_component_program_seed_valid(c,&next))return false;
  *out=next;return true;
}
#endif
#endif
