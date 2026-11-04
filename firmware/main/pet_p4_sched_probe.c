#include "pet_p4_sched_probe.h"
#if PET_P4_SCHED_PROBE_ENABLE
#if !PET_P4_SCHED_TRACE_ENABLE
#error "scheduler probe requires explicitly enabled scheduler trace"
#endif
#include "pet_p4_resources_device.h"
#include "pet_p4_audio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdatomic.h>
#include <string.h>

/* Full-slot exclusion reservation shared with the native program diagnostic.
 * NOT fuel executed, trace bytes, a CPU budget, or a 2ms admission guarantee. */
#define NATIVE_DIAGNOSTIC_RESERVATION 4096U
_Static_assert(ATOMIC_INT_LOCK_FREE==2 && sizeof(unsigned)==4,"ISR gate cannot use libatomic locks");
typedef struct {
    pet_sched_probe_status_t public;
    pet_resource_owner_t resource_owner;
    pet_resource_lease_t lease;
    uint64_t armed_at,capture_at,frozen_at,last_now;
    uint32_t window_us,save_sequence;
    bool finish_attempted,completion_bad_clock;
} probe_t;
static DRAM_ATTR portMUX_TYPE probe_mux=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR probe_t stored;
static DRAM_ATTR bool ready,claimed,owner_live,end_pending,end_clock_bad;
static DRAM_ATTR uint64_t next_owner,next_revision,live_owner,end_at;
static DRAM_ATTR bool end_ok;
static DRAM_ATTR pet_sched_probe_save_t ended;
static DRAM_ATTR atomic_uint published_trace,marker_busy,marker_contended;

static bool same(pet_sched_probe_ticket_t a,pet_sched_probe_ticket_t b){return a.owner==b.owner && a.revision==b.revision && a.owner && a.revision;}
static bool live(uint64_t owner){
    portENTER_CRITICAL(&probe_mux);bool ok=owner && owner_live && owner==live_owner;portEXIT_CRITICAL(&probe_mux);return ok;
}
static bool any_live(void){portENTER_CRITICAL(&probe_mux);bool yes=owner_live;portEXIT_CRITICAL(&probe_mux);return yes;}
static uint64_t revision(void){
    portENTER_CRITICAL(&probe_mux);uint64_t r=next_revision==UINT64_MAX?0:++next_revision;portEXIT_CRITICAL(&probe_mux);return r;
}
static bool take(probe_t *p){
    portENTER_CRITICAL(&probe_mux);
    bool ok=!claimed;
    if(ok){
        claimed=true;*p=stored;
        if(end_pending && same(ended.ticket,p->public.ticket) && ended.trace_generation==p->public.trace.generation){
            p->public.save_inflight=false;p->public.save_ok=end_ok;p->public.save_completed_us=end_at;
            /* Completion is asynchronous: it may legitimately predate a
             * newer control sample published while this latch was pending. */
            p->completion_bad_clock=end_clock_bad || end_at<p->capture_at;
            end_pending=false;
        }
    }
    portEXIT_CRITICAL(&probe_mux);return ok;
}
static bool audio_busy(void){
    /* Fresh independent observations, no short-circuit and no audio action. */
    bool a=pet_p4_audio_active(),b=pet_p4_audio_playback_active();
    bool c=pet_p4_audio_conversation_active(),d=pet_p4_audio_music_active();
    bool e=pet_p4_audio_stream_playing();return a||b||c||d||e;
}
static void stop_trace(probe_t *p){
    atomic_store_explicit(&published_trace,0,memory_order_release);
    if(p->public.trace.generation){
        if(!pet_sched_trace_stop(p->public.trace.generation) || !pet_sched_trace_status(p->public.trace.generation,&p->public.trace))
            p->public.reason=PET_SCHED_PROBE_TRACE_ERROR;
    }
    p->public.marker_contended=atomic_load_explicit(&marker_contended,memory_order_acquire)!=0;
}
static void freeze(probe_t *p,pet_sched_probe_reason_t why,uint64_t now){
    if(p->public.state!=PET_SCHED_PROBE_ARMED && p->public.state!=PET_SCHED_PROBE_CAPTURING)return;
    p->public.reason=why;stop_trace(p);p->public.state=PET_SCHED_PROBE_FROZEN;p->frozen_at=now;
    /* Diagnostic evidence remains readable to the separate trusted controller,
     * but the revoked work owner cannot authorize further physical capture. */
    if(p->resource_owner.generation)pet_resources_device_revoke(p->resource_owner);
}
static void drain(probe_t *p,pet_sched_probe_reason_t why){
    if(p->public.state==PET_SCHED_PROBE_FAILED)return;
    p->public.reason=why;stop_trace(p);p->public.state=PET_SCHED_PROBE_DRAINING;
    if(p->resource_owner.generation)pet_resources_device_revoke(p->resource_owner);
    if(p->public.save_inflight || atomic_load_explicit(&marker_busy,memory_order_acquire))return;
    if(p->lease.serial){
        if(p->finish_attempted){p->public.state=PET_SCHED_PROBE_FAILED;p->public.reason=PET_SCHED_PROBE_FINISH_FAILED;return;}
        p->finish_attempted=true;
        if(pet_resources_device_finish(p->lease)!=PET_RESOURCE_FINISH_REVOKED){
            p->public.state=PET_SCHED_PROBE_FAILED;p->public.reason=PET_SCHED_PROBE_FINISH_FAILED;return;
        }
    }
    uint64_t r=revision();
    if(!r){p->public.state=PET_SCHED_PROBE_FAILED;p->public.reason=PET_SCHED_PROBE_FINISH_FAILED;return;}
    memset(&p->lease,0,sizeof(p->lease));memset(&p->resource_owner,0,sizeof(p->resource_owner));
    memset(&p->public.trace,0,sizeof(p->public.trace));p->finish_attempted=false;
    p->public.ticket.revision=r;p->public.state=PET_SCHED_PROBE_IDLE;
}
static void advance(probe_t *p,int64_t sample,bool audio){
    if(!p->public.ticket.owner || p->public.state==PET_SCHED_PROBE_FAILED)return;
    bool bad=p->completion_bad_clock || sample<0 || (uint64_t)sample<p->last_now;
    p->completion_bad_clock=false;
    uint64_t now=bad?p->last_now:(uint64_t)sample;
    if(!bad)p->last_now=now;
    if(!live(p->public.ticket.owner)){drain(p,PET_SCHED_PROBE_OWNER_LOST);return;}
    if(p->public.state==PET_SCHED_PROBE_DRAINING){drain(p,p->public.reason);return;}
    if(bad){freeze(p,PET_SCHED_PROBE_CLOCK,now);return;}
    if(p->public.state==PET_SCHED_PROBE_CAPTURING){
        pet_sched_trace_status_t trace;
        if(!pet_sched_trace_status(p->public.trace.generation,&trace)){freeze(p,PET_SCHED_PROBE_TRACE_ERROR,now);return;}
        p->public.trace=trace;
        if(trace.flags&PET_SCHED_FLAG_BAD_CLOCK){freeze(p,PET_SCHED_PROBE_CLOCK,now);return;}
        if(trace.flags&PET_SCHED_FLAG_BAD_CORE){freeze(p,PET_SCHED_PROBE_TRACE_ERROR,now);return;}
        if((trace.flags&PET_SCHED_FLAG_EXPIRED) || now-p->capture_at>=p->window_us){freeze(p,PET_SCHED_PROBE_WINDOW_END,p->capture_at+p->window_us);}
        else if(!p->public.save_inflight){freeze(p,PET_SCHED_PROBE_COMPLETE,p->public.save_completed_us);}
    }
    if(p->public.state==PET_SCHED_PROBE_ARMED || p->public.state==PET_SCHED_PROBE_CAPTURING){
        if(audio)freeze(p,PET_SCHED_PROBE_AUDIO,now);
        else if(!pet_resources_device_current(p->lease))freeze(p,PET_SCHED_PROBE_RESOURCE_LOST,now);
        else if(p->public.state==PET_SCHED_PROBE_ARMED && now-p->armed_at>=PET_SCHED_PROBE_ARM_US)freeze(p,PET_SCHED_PROBE_ARM_TIMEOUT,p->armed_at+PET_SCHED_PROBE_ARM_US);
    }
    if(p->public.state==PET_SCHED_PROBE_FROZEN && now>=p->frozen_at && now-p->frozen_at>=PET_SCHED_PROBE_FROZEN_US)
        drain(p,PET_SCHED_PROBE_EXPIRED);
    p->public.marker_contended=atomic_load_explicit(&marker_contended,memory_order_acquire)!=0;
}
static pet_sched_probe_result_t put(probe_t *p,pet_sched_probe_result_t result){
    /* Revocation can arrive while an external trace/broker call is in progress.
     * Keep the claim through cleanup; owner_open cannot replace this owner. */
    for(;;){
        portENTER_CRITICAL(&probe_mux);
        bool lost=p->public.ticket.owner && (!owner_live || p->public.ticket.owner!=live_owner);
        if(lost && p->public.state!=PET_SCHED_PROBE_IDLE && p->public.state!=PET_SCHED_PROBE_DRAINING && p->public.state!=PET_SCHED_PROBE_FAILED){
            portEXIT_CRITICAL(&probe_mux);drain(p,PET_SCHED_PROBE_OWNER_LOST);continue;
        }
        stored=*p;claimed=false;portEXIT_CRITICAL(&probe_mux);
        return lost?PET_SCHED_PROBE_STALE:result;
    }
}
static bool matches(probe_t *p,pet_sched_probe_ticket_t ticket){return same(p->public.ticket,ticket) && live(ticket.owner);}
static void update(probe_t *p){int64_t now=esp_timer_get_time();bool audio=audio_busy();advance(p,now,audio);}

pet_sched_probe_result_t pet_sched_probe_init(void *main_task,void *save_task,void *protocol_task){
    if(!main_task || !save_task || !protocol_task || main_task==save_task || main_task==protocol_task || save_task==protocol_task)return PET_SCHED_PROBE_INVALID;
    probe_t p;if(!take(&p))return PET_SCHED_PROBE_BUSY;
    if(ready || live_owner)return put(&p,PET_SCHED_PROBE_BUSY);
    bool ok=pet_sched_trace_register(PET_SCHED_TASK_MAIN,main_task) && pet_sched_trace_register(PET_SCHED_TASK_SAVE,save_task) && pet_sched_trace_register(PET_SCHED_TASK_PROTOCOL,protocol_task);
    portENTER_CRITICAL(&probe_mux);ready=ok;portEXIT_CRITICAL(&probe_mux);
    return put(&p,ok?PET_SCHED_PROBE_OK:PET_SCHED_PROBE_FAULT);
}
pet_sched_probe_result_t pet_sched_probe_owner_open(pet_sched_probe_owner_t *out){
    if(!out || out->generation)return PET_SCHED_PROBE_INVALID;
    probe_t p;if(!take(&p))return PET_SCHED_PROBE_BUSY;
    if(!ready)return put(&p,PET_SCHED_PROBE_NOT_READY);
    if(p.public.state!=PET_SCHED_PROBE_IDLE || any_live()){
        pet_sched_probe_result_t denied=p.public.state==PET_SCHED_PROBE_FAILED?PET_SCHED_PROBE_FAULT:PET_SCHED_PROBE_BUSY;
        (void)put(&p,denied);return denied; /* new owner has no stale old ticket */
    }
    if(next_owner==UINT64_MAX)return put(&p,PET_SCHED_PROBE_FAULT);
    uint64_t r=revision();if(!r)return put(&p,PET_SCHED_PROBE_FAULT);
    portENTER_CRITICAL(&probe_mux);live_owner=++next_owner;owner_live=true;uint64_t owner=live_owner;portEXIT_CRITICAL(&probe_mux);
    p=(probe_t){.public={.ticket={owner,r},.state=PET_SCHED_PROBE_IDLE}};
    pet_sched_probe_result_t result=put(&p,PET_SCHED_PROBE_OK);if(result==PET_SCHED_PROBE_OK)out->generation=owner;return result;
}
pet_sched_probe_result_t pet_sched_probe_owner_revoke(pet_sched_probe_owner_t owner){
    portENTER_CRITICAL(&probe_mux);bool ok=owner.generation && owner.generation==live_owner;
    if(ok){owner_live=false;atomic_store_explicit(&published_trace,0,memory_order_release);}
    portEXIT_CRITICAL(&probe_mux);
    if(ok)(void)pet_sched_probe_poll();
    return ok?PET_SCHED_PROBE_OK:PET_SCHED_PROBE_STALE;
}
pet_sched_probe_result_t pet_sched_probe_poll(void){
    probe_t p;if(!take(&p))return PET_SCHED_PROBE_BUSY;
    if(p.public.state!=PET_SCHED_PROBE_IDLE)update(&p);
    return put(&p,p.public.state==PET_SCHED_PROBE_FAILED?PET_SCHED_PROBE_FAULT:PET_SCHED_PROBE_OK);
}
pet_sched_probe_result_t pet_sched_probe_status(pet_sched_probe_owner_t owner,pet_sched_probe_status_t *out){
    if(!out)return PET_SCHED_PROBE_INVALID;
    probe_t p;if(!take(&p))return PET_SCHED_PROBE_BUSY;
    if(!live(owner.generation) || p.public.ticket.owner!=owner.generation)return put(&p,PET_SCHED_PROBE_STALE);
    update(&p);pet_sched_probe_status_t copy=p.public;
    pet_sched_probe_result_t r=put(&p,PET_SCHED_PROBE_OK);if(r==PET_SCHED_PROBE_OK)*out=copy;return r;
}
pet_sched_probe_result_t pet_sched_probe_arm(pet_sched_probe_ticket_t expected,uint32_t window_us,pet_sched_probe_ticket_t *out){
    if(!out || !window_us || window_us>PET_SCHED_TRACE_MAX_WINDOW_US)return PET_SCHED_PROBE_INVALID;
    probe_t p;if(!take(&p))return PET_SCHED_PROBE_BUSY;
    if(!matches(&p,expected))return put(&p,PET_SCHED_PROBE_STALE);
    if(p.public.state==PET_SCHED_PROBE_FAILED)return put(&p,PET_SCHED_PROBE_FAULT);
    if(p.public.state!=PET_SCHED_PROBE_IDLE || atomic_load_explicit(&marker_busy,memory_order_acquire))return put(&p,PET_SCHED_PROBE_BUSY);
    int64_t now=esp_timer_get_time();if(now<0)return put(&p,PET_SCHED_PROBE_FAULT);
    if(audio_busy())return put(&p,PET_SCHED_PROBE_AUDIO_BUSY);
    uint64_t r=revision();if(!r)return put(&p,PET_SCHED_PROBE_FAULT);
    p=(probe_t){.public={.ticket={expected.owner,r},.state=PET_SCHED_PROBE_ARMED},.armed_at=(uint64_t)now,.last_now=(uint64_t)now,.window_us=window_us};
    atomic_store_explicit(&marker_contended,0,memory_order_release);
    pet_resource_result_t resource=pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_NATIVE_PROBE),&p.resource_owner);
    if(resource==PET_RESOURCE_OK)resource=pet_resources_device_acquire(p.resource_owner,PET_RESOURCE_NATIVE_PROBE,NATIVE_DIAGNOSTIC_RESERVATION,&p.lease);
    if(resource!=PET_RESOURCE_OK){drain(&p,PET_SCHED_PROBE_RESOURCE_LOST);return put(&p,p.public.state==PET_SCHED_PROBE_FAILED?PET_SCHED_PROBE_FAULT:PET_SCHED_PROBE_RESOURCE_BUSY);}
    bool audio=audio_busy();
    if(audio || !live(expected.owner) || !pet_resources_device_current(p.lease)){
        drain(&p,audio?PET_SCHED_PROBE_AUDIO:PET_SCHED_PROBE_OWNER_LOST);
        return put(&p,p.public.state==PET_SCHED_PROBE_FAILED?PET_SCHED_PROBE_FAULT:audio?PET_SCHED_PROBE_AUDIO_BUSY:PET_SCHED_PROBE_STALE);
    }
    int64_t admitted=esp_timer_get_time();
    if(admitted<now || (uint64_t)admitted-p.armed_at>=PET_SCHED_PROBE_ARM_US){
        drain(&p,admitted<now?PET_SCHED_PROBE_CLOCK:PET_SCHED_PROBE_ARM_TIMEOUT);
        return put(&p,PET_SCHED_PROBE_FAULT);
    }
    pet_sched_probe_ticket_t ticket=p.public.ticket;pet_sched_probe_result_t result=put(&p,PET_SCHED_PROBE_OK);
    if(result==PET_SCHED_PROBE_OK)*out=ticket;
    return result;
}
pet_sched_probe_save_t pet_sched_probe_save_begin(uint32_t diagnostic_sequence){
    pet_sched_probe_save_t token={0};probe_t p;if(!take(&p))return token;
    update(&p);
    if(p.public.state==PET_SCHED_PROBE_ARMED){
        if(atomic_load_explicit(&marker_busy,memory_order_acquire))freeze(&p,PET_SCHED_PROBE_TRACE_ERROR,p.last_now);
        else{
            p.public.trace.generation=pet_sched_trace_begin(p.window_us);
            if(!p.public.trace.generation)freeze(&p,PET_SCHED_PROBE_TRACE_ERROR,p.last_now);
            else if(!pet_sched_trace_status(p.public.trace.generation,&p.public.trace))freeze(&p,PET_SCHED_PROBE_TRACE_ERROR,p.last_now);
            else{
                p.public.state=PET_SCHED_PROBE_CAPTURING;p.public.save_inflight=true;p.capture_at=p.public.trace.started_us;p.save_sequence=diagnostic_sequence;
                if(p.capture_at<p.last_now)p.completion_bad_clock=true;
                token=(pet_sched_probe_save_t){p.public.ticket,p.public.trace.generation};
                atomic_store_explicit(&published_trace,p.public.trace.generation,memory_order_release);
                pet_sched_trace_mark(PET_SCHED_MARK_SAVE_BEGIN,diagnostic_sequence);
                update(&p); /* fresh audio/owner/lease after starting the observer */
            }
        }
    }
    (void)put(&p,PET_SCHED_PROBE_OK);return token;
}
static void IRAM_ATTR emit_mark(pet_sched_marker_t marker,uint32_t value,uint32_t generation);
pet_sched_probe_result_t pet_sched_probe_save_end(pet_sched_probe_save_t token,bool ok){
    if(!token.trace_generation)return PET_SCHED_PROBE_INVALID;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&probe_mux);
    bool match=same(token.ticket,stored.public.ticket) && token.trace_generation==stored.public.trace.generation;
    bool duplicate=match && same(token.ticket,ended.ticket) && token.trace_generation==ended.trace_generation;
    uint32_t sequence=stored.save_sequence;
    if(match && !duplicate && stored.public.save_inflight){
        ended=token;end_pending=true;end_ok=ok;end_clock_bad=now<0;end_at=now<0?0:(uint64_t)now;
    }else if(!duplicate)match=false;
    portEXIT_CRITICAL(&probe_mux);
    if(match && !duplicate)emit_mark(PET_SCHED_MARK_SAVE_END,sequence,token.trace_generation);
    if(match)(void)pet_sched_probe_poll();
    return match?PET_SCHED_PROBE_OK:PET_SCHED_PROBE_STALE;
}
pet_sched_probe_result_t pet_sched_probe_cancel(pet_sched_probe_ticket_t ticket){
    probe_t p;if(!take(&p))return PET_SCHED_PROBE_BUSY;
    if(!matches(&p,ticket))return put(&p,PET_SCHED_PROBE_STALE);
    update(&p);
    if(!same(p.public.ticket,ticket))return put(&p,PET_SCHED_PROBE_STALE);
    freeze(&p,PET_SCHED_PROBE_CANCELLED,p.last_now);
    return put(&p,p.public.state==PET_SCHED_PROBE_FAILED?PET_SCHED_PROBE_FAULT:PET_SCHED_PROBE_OK);
}
pet_sched_probe_result_t pet_sched_probe_discard(pet_sched_probe_ticket_t ticket){
    probe_t p;if(!take(&p))return PET_SCHED_PROBE_BUSY;
    if(!matches(&p,ticket))return put(&p,PET_SCHED_PROBE_STALE);
    update(&p);
    if(!same(p.public.ticket,ticket))return put(&p,PET_SCHED_PROBE_STALE);
    if(p.public.state!=PET_SCHED_PROBE_FROZEN && p.public.state!=PET_SCHED_PROBE_DRAINING)return put(&p,PET_SCHED_PROBE_NOT_READY);
    drain(&p,PET_SCHED_PROBE_DISCARDED);return put(&p,p.public.state==PET_SCHED_PROBE_FAILED?PET_SCHED_PROBE_FAULT:PET_SCHED_PROBE_OK);
}
pet_sched_probe_result_t pet_sched_probe_read(pet_sched_probe_ticket_t ticket,uint32_t index,pet_sched_probe_page_t *out){
    if(!out)return PET_SCHED_PROBE_INVALID;
    probe_t p;if(!take(&p))return PET_SCHED_PROBE_BUSY;
    if(!matches(&p,ticket))return put(&p,PET_SCHED_PROBE_STALE);
    int64_t now=esp_timer_get_time();bool audio=audio_busy();advance(&p,now,audio);
    if(audio)return put(&p,PET_SCHED_PROBE_AUDIO_BUSY);
    if(!same(p.public.ticket,ticket))return put(&p,PET_SCHED_PROBE_STALE);
    if(p.public.state!=PET_SCHED_PROBE_FROZEN)return put(&p,PET_SCHED_PROBE_NOT_READY);
    if(index>p.public.trace.count)return put(&p,PET_SCHED_PROBE_INVALID);
    pet_sched_probe_page_t page={.status=p.public,.index=index,.next_index=index};
    while(page.next_index<p.public.trace.count && page.count<PET_SCHED_PROBE_PAGE_EVENTS){
        if(!pet_sched_trace_read(p.public.trace.generation,page.next_index,&page.events[page.count]))return put(&p,PET_SCHED_PROBE_FAULT);
        page.next_index++;page.count++;
    }
    page.done=page.next_index==p.public.trace.count;
    if(audio_busy())return put(&p,PET_SCHED_PROBE_AUDIO_BUSY);
    pet_sched_probe_result_t result=put(&p,PET_SCHED_PROBE_OK);if(result==PET_SCHED_PROBE_OK)*out=page;return result;
}
static void IRAM_ATTR emit_mark(pet_sched_marker_t marker,uint32_t value,uint32_t generation){
    if(!atomic_load_explicit(&published_trace,memory_order_acquire))return;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&marker_busy,&expected,1,memory_order_acquire,memory_order_relaxed)){
        atomic_store_explicit(&marker_contended,1,memory_order_release);return;
    }
    uint32_t active=atomic_load_explicit(&published_trace,memory_order_acquire);
    if(active && (!generation || generation==active))pet_sched_trace_mark(marker,value);
    atomic_store_explicit(&marker_busy,0,memory_order_release);
}
void IRAM_ATTR pet_sched_probe_ppa_mark(pet_sched_marker_t marker,uint32_t cookie){
    if(marker>=PET_SCHED_MARK_PPA_BEGIN && marker<=PET_SCHED_MARK_PPA_RETURN)emit_mark(marker,cookie,0);
}
#endif
