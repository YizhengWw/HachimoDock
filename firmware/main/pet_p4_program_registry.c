#include "pet_p4_program_registry.h"
#if PET_COMPONENT_BACKGROUND_ENABLED
#include <string.h>
#include <limits.h>
#if PET_PROGRAM_PHASE_TRACE
#include "esp_timer.h"
#endif
static bool owned(const pet_program_registry_t *r){return r&&r->instance==r&&r->scheduler.instance==&r->scheduler;}
static bool overlap(const void *a,size_t an,const void *b,size_t bn){uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;}
static bool external(const pet_program_registry_t *r,const void *p,size_t n){return p&&!overlap(r,sizeof(*r),p,n);}
static bool id_valid(const char *id){
    if(!id||!id[0])return false;
    for(unsigned n=0;n<48;n++){char c=id[n];if(!c)return true;if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return false;}
    return false;
}
static pet_program_registry_entry_t *entry(pet_program_registry_t *r,pet_program_registration_t h){
    if(!owned(r)||h.registry!=r||!h.serial||h.slot>=PET_PROGRAM_REGISTRY_SLOTS)return NULL;
    pet_program_registry_entry_t *e=&r->entries[h.slot];return e->occupied&&e->serial==h.serial?e:NULL;
}
static pet_program_registration_t ticket(pet_program_registry_t *r,unsigned slot){return (pet_program_registration_t){r,r->entries[slot].serial,(uint8_t)slot};}
static bool same_snapshot(const pet_program_snapshot_t *a,const pet_program_snapshot_t *b){
    return a->schema==b->schema&&a->revision==b->revision&&a->cells==b->cells
        &&!memcmp(a->values,b->values,sizeof(a->values))&&!memcmp(a->outputs,b->outputs,sizeof(a->outputs));
}
static void project(pet_program_registry_entry_t *e){
    for(unsigned i=0;i<8;i++){unsigned at=e->config.projection.target[i];if(at!=PET_PROGRAM_UI_UNBOUND)e->values[at]=e->committed.outputs[i];}
}
/* Historical committed state remains readable after revocation. A commit that
 * succeeded before audio reservation but could not project afterward must not
 * be lost from persistence. This does not create execution/effect authority. */
static void capture(pet_program_registry_entry_t *e){
    pet_program_snapshot_t s;
    pet_program_snapshot_cursor_t cursor;
    pet_program_runtime_result_t result=pet_component_program_session_snapshot_if_changed(
        &e->session,&e->capture_cursor,&s,&cursor);
    if(result==PET_RUNTIME_UNCHANGED){
        /* Only private VM commits change the selected payload; each advances
         * revision (overflow is rejected). No cached state comes from wire/UI.
         * Recheck receiving layout/projection before trusting past validation.
         * Even a cache hit MUST refresh bound UI values and observe stop below. */
        if(e->config.enabled&&e->config.image.schema==e->committed.schema
            &&e->config.image.cells==e->committed.cells
            &&!memcmp(&e->config.projection,&e->capture_projection,sizeof(e->capture_projection))){
            project(e);
        }else if(pet_component_program_seed_valid(&e->config,&e->committed)){
            e->capture_projection=e->config.projection;project(e);
        }else e->capture_cursor=(pet_program_snapshot_cursor_t){0};
    }else if(result==PET_RUNTIME_OK&&pet_component_program_seed_valid(&e->config,&s)){
        if(!same_snapshot(&s,&e->committed)){e->committed=s;e->checkpoint_pending=true;}
        e->capture_projection=e->config.projection;e->capture_cursor=cursor;
        project(e);
    }else e->capture_cursor=(pet_program_snapshot_cursor_t){0};
    if(e->session.stop_requested||e->session.retiring||(!e->session.live&&e->session.generation))e->stopped=true;
}
bool pet_program_registry_init(pet_program_registry_t *r,bool (*factory)(pet_program_host_t*)){
    if(!r||!factory||r->instance||r->next_serial||r->inside)return false;
    /* Claim initialization before any native callback; partial initialization
     * is intentionally not restartable or reported as an owned live table. */
    r->instance=r;r->inside=true;
    if(!pet_program_scheduler_init(&r->scheduler))return false;
    for(unsigned i=0;i<PET_PROGRAM_REGISTRY_SLOTS;i++){
        pet_program_registry_entry_t *e=&r->entries[i];
        if(e->occupied||e->serial||e->pin.serial||!factory(&e->session.host)
            ||!pet_component_program_session_bind_scheduler(&e->session,&r->scheduler))return false;
    }
    r->inside=false;return true;
}
bool pet_program_registry_find(pet_program_registry_t *r,const char *id,pet_program_registration_t *out){
    if(!owned(r)||r->inside||!id_valid(id)||!external(r,out,sizeof(*out))||out->registry||out->serial||out->slot)return false;
    for(unsigned i=0;i<PET_PROGRAM_REGISTRY_SLOTS;i++)if(r->entries[i].occupied&&!strcmp(r->entries[i].id,id)){*out=ticket(r,i);return true;}
    return false;
}
bool pet_program_registry_snapshot(pet_program_registry_t *r,pet_program_registration_t h,pet_program_snapshot_t *out){
    pet_program_registry_entry_t *e=entry(r,h);
    if(!e||r->inside||!external(r,out,sizeof(*out)))return false;
    capture(e);*out=e->committed;return true;
}
pet_program_registry_result_t pet_program_registry_attach(pet_program_registry_t *r,const char *id,
    const uint64_t content[3],const pet_component_program_config_t *config,const pet_program_snapshot_t *seed,
    const int32_t base[8],bool background,uint64_t now,pet_program_registration_t *out){
    if(!owned(r)||r->inside||!id_valid(id)||!external(r,content,3*sizeof(*content))||!external(r,config,sizeof(*config))
        ||!external(r,seed,sizeof(*seed))||!external(r,base,8*sizeof(*base))||!external(r,out,sizeof(*out))
        ||out->registry||out->serial||out->slot||!(content[0]|content[1]|content[2])
        ||config->action_count>PET_COMPONENT_PROGRAM_ACTIONS||config->timer_count>PET_PROGRAM_TIMERS
        ||!pet_component_program_seed_valid(config,seed)
        ||overlap(out,sizeof(*out),content,3*sizeof(*content))||overlap(out,sizeof(*out),config,sizeof(*config))
        ||overlap(out,sizeof(*out),seed,sizeof(*seed))||overlap(out,sizeof(*out),base,8*sizeof(*base)))return PET_REGISTRY_INVALID;
    unsigned free_slot=PET_PROGRAM_REGISTRY_SLOTS;
    for(unsigned i=0;i<PET_PROGRAM_REGISTRY_SLOTS;i++){
        pet_program_registry_entry_t *e=&r->entries[i];
        if(e->occupied&&!strcmp(e->id,id)){
            if(memcmp(e->content,content,sizeof(e->content))||memcmp(&e->config,config,sizeof(*config))||e->background!=background)return PET_REGISTRY_CONFLICT;
            r->inside=true;
            if(e->session.live)(void)pet_program_host_check_current(&e->session.host,e->session.generation);
            /* Authority observation may cancel the host before session flags
             * catch up. Collect owns that retirement, never a fresh dispatch. */
            int32_t next[8];
            if(pet_component_program_session_collect(&e->session,e->serial,e->values,next))memcpy(e->values,next,sizeof(next));
            capture(e);r->inside=false;
            if(e->stopped||!e->session.live)return PET_REGISTRY_STOPPED;
            *out=ticket(r,i);return PET_REGISTRY_ATTACHED;
        }
        if(!e->occupied&&!e->pin.serial&&!e->session.live&&!e->session.retiring&&!e->session.scheduled&&!e->session.host.bound&&free_slot==PET_PROGRAM_REGISTRY_SLOTS)free_slot=i;
    }
    if(free_slot==PET_PROGRAM_REGISTRY_SLOTS)return PET_REGISTRY_FULL;
    if(r->next_serial==UINT64_MAX)return PET_REGISTRY_EXHAUSTED;
    if(r->scheduler.stopped&&pet_program_scheduler_rearm(&r->scheduler)!=PET_SCHED_OK)return PET_REGISTRY_STOPPED;
    pet_program_registry_entry_t *e=&r->entries[free_slot];r->inside=true;
    if(!pet_app_store_pin_scoped(PET_PROGRAM_SAVE_DOMAIN,id,e,&e->pin)){r->inside=false;return PET_REGISTRY_DENIED;}
    e->serial=++r->next_serial;e->occupied=true;e->background=background;e->stopped=e->checkpoint_pending=false;
    e->capture_cursor=(pet_program_snapshot_cursor_t){0};
    strcpy(e->id,id);memcpy(e->content,content,sizeof(e->content));e->config=*config;e->committed=*seed;
    memcpy(e->values,base,sizeof(e->values));project(e);
    pet_program_runtime_result_t result=pet_component_program_session_start(&e->session,&e->config,&e->committed,e->serial,now);
    if(result!=PET_RUNTIME_OK){
        e->stopped=true;
        if(e->session.live||e->session.retiring||e->session.scheduled||e->session.host.bound){r->inside=false;return PET_REGISTRY_PENDING;}
        if(!pet_app_store_unpin(&e->pin)){r->inside=false;return PET_REGISTRY_PENDING;}
        e->occupied=false;e->serial=0;e->id[0]=0;r->inside=false;return PET_REGISTRY_DENIED;
    }
    *out=ticket(r,free_slot);r->inside=false;return PET_REGISTRY_OK;
}
bool pet_program_registry_view(pet_program_registry_t *r,pet_program_registration_t h,const int32_t base[8],int32_t values[8],pet_program_snapshot_t *snapshot){
    pet_program_registry_entry_t *e=entry(r,h);
    if(!e||r->inside||!external(r,base,8*sizeof(*base))||!external(r,values,8*sizeof(*values))||!external(r,snapshot,sizeof(*snapshot))
        ||overlap(values,8*sizeof(*values),snapshot,sizeof(*snapshot))||overlap(base,8*sizeof(*base),values,8*sizeof(*values))
        ||overlap(base,8*sizeof(*base),snapshot,sizeof(*snapshot)))return false;
    memcpy(e->values,base,sizeof(e->values));capture(e);project(e);memcpy(values,e->values,sizeof(e->values));*snapshot=e->committed;return true;
}
pet_program_runtime_result_t pet_program_registry_enqueue(pet_program_registry_t *r,pet_program_registration_t h,const char *action,const int32_t base[8]){
    pet_program_registry_entry_t *e=entry(r,h);
    if(!e||r->inside||!action||!external(r,base,8*sizeof(*base)))return PET_RUNTIME_STALE;
    if(e->stopped)return PET_RUNTIME_STOPPED;
    pet_program_event_t event;
    if(!pet_component_program_event(&e->config,action,base,&event))return PET_RUNTIME_INVALID;
    r->inside=true;pet_program_runtime_result_t result=pet_component_program_session_enqueue(&e->session,e->serial,&event);capture(e);r->inside=false;return result;
}
pet_program_schedule_result_t pet_program_registry_step(pet_program_registry_t *r,uint64_t now){
    if(!owned(r)||r->inside)return PET_SCHED_INVALID;r->inside=true;
#if PET_PROGRAM_PHASE_TRACE
    pet_program_phase_t phase={0};int64_t started=esp_timer_get_time(),part=started;
    phase.start_us=started<0?0:(uint64_t)started;
#endif
    pet_program_schedule_diagnostic_t d={0};pet_program_schedule_result_t result=pet_program_scheduler_step(&r->scheduler,now,&d);
#if PET_PROGRAM_PHASE_TRACE
    int64_t ended=esp_timer_get_time();pet_program_phase_add(&phase,&phase.scheduler_us,part,ended);
#endif
    for(unsigned i=0;i<PET_PROGRAM_REGISTRY_SLOTS;i++){
        pet_program_registry_entry_t *e=&r->entries[i];if(!e->occupied)continue;
        int32_t next[8];
#if PET_PROGRAM_PHASE_TRACE
        part=esp_timer_get_time();
#endif
        if(pet_component_program_session_collect(&e->session,e->serial,e->values,next))memcpy(e->values,next,sizeof(next));
#if PET_PROGRAM_PHASE_TRACE
        ended=esp_timer_get_time();pet_program_phase_add(&phase,&phase.collect_us,part,ended);part=ended;
        phase.collected++;
#endif
        capture(e);
#if PET_PROGRAM_PHASE_TRACE
        ended=esp_timer_get_time();pet_program_phase_add(&phase,&phase.capture_us,part,ended);
#endif
    }
#if PET_PROGRAM_PHASE_TRACE
    ended=esp_timer_get_time();phase.end_us=ended<0?0:(uint64_t)ended;
    pet_program_phase_add(&phase,&phase.parent_us,started,ended);r->last_phase=phase;
#endif
    r->inside=false;return result;
}
bool pet_program_registry_checkpoint_one(pet_program_registry_t *r){
    if(!owned(r)||r->inside)return false;r->inside=true;
    for(unsigned n=0;n<PET_PROGRAM_REGISTRY_SLOTS;n++){
        unsigned at=(r->checkpoint_cursor+n)%PET_PROGRAM_REGISTRY_SLOTS;pet_program_registry_entry_t *e=&r->entries[at];
        if(!e->occupied||!e->checkpoint_pending)continue;
        r->checkpoint_cursor=(uint8_t)((at+1)%PET_PROGRAM_REGISTRY_SLOTS);
        bool ok=e->pin.serial&&pet_program_persistence_enqueue(e->id,&e->committed);
        if(ok)e->checkpoint_pending=false;r->inside=false;return ok;
    }
    r->inside=false;return true;
}
pet_program_runtime_result_t pet_program_registry_stop(pet_program_registry_t *r,pet_program_registration_t h){
    pet_program_registry_entry_t *e=entry(r,h);if(!e||r->inside)return PET_RUNTIME_STALE;
    r->inside=true;e->stopped=true;pet_program_runtime_result_t result=pet_component_program_session_stop(&e->session);capture(e);r->inside=false;return result;
}
bool pet_program_registry_release(pet_program_registry_t *r,pet_program_registration_t h){
    pet_program_registry_entry_t *e=entry(r,h);
    if(!e||r->inside||!e->stopped||e->session.live||e->session.retiring||e->session.scheduled||e->session.host.bound)return false;
    capture(e);if(e->checkpoint_pending)return false;
    r->inside=true;bool ok=pet_app_store_unpin(&e->pin);
    if(ok){e->occupied=false;e->serial=0;e->id[0]=0;e->capture_cursor=(pet_program_snapshot_cursor_t){0};}
    r->inside=false;return ok;
}
#endif
