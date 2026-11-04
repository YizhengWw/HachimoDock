#include "pet_p4_sched_trace.h"
#if PET_P4_SCHED_TRACE_ENABLE
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <limits.h>

#ifndef PET_SCHED_TRACE_HOST_TEST
#include "esp_idf_version.h"
#if !defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_FREERTOS_SMP)
#error "scheduler observer requires the audited ESP32-P4 legacy FreeRTOS ABI"
#endif
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5,5,1) && ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5,5,4)
#error "audit wrap call coverage and ISR ABI before enabling on another SDK"
#endif
_Static_assert(sizeof(void *)==4,"audited 32-bit task handle ABI only");
#endif
_Static_assert(configNUMBER_OF_CORES==2,"audited dual-core ABI only");
_Static_assert(ATOMIC_INT_LOCK_FREE==2 && sizeof(unsigned)==4,"no libatomic/hidden lock in hooks");
_Static_assert(sizeof(BaseType_t)==4,"audited priority result ABI only");
_Static_assert(sizeof(pet_sched_event_t)==16,"fixed numeric ring record budget");
_Static_assert((PET_SCHED_TRACE_CAPACITY&(PET_SCHED_TRACE_CAPACITY-1U))==0,"ring power of two");

/* These exported SDK objects/functions have been checked against both SDK
 * sources. Only compare TCB pointers; do not reproduce a private TCB prefix.
 * portasm calls void vTaskSwitchContext(void), not the SMP-kernel core-id ABI. */
extern TaskHandle_t volatile pxCurrentTCBs[configNUMBER_OF_CORES];
typedef struct spiffs_t spiffs;
void __real_spiffs_api_lock(spiffs *fs);
void __real_spiffs_api_unlock(spiffs *fs);
BaseType_t __real_xTaskPriorityInherit(TaskHandle_t holder);
BaseType_t __real_xTaskPriorityDisinherit(TaskHandle_t holder);
void __real_vTaskSwitchContext(void);

static DRAM_ATTR portMUX_TYPE trace_mux=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR atomic_uint active_generation;
static DRAM_ATTR void *task_handles[4];
static DRAM_ATTR pet_sched_event_t events[PET_SCHED_TRACE_CAPACITY];
static DRAM_ATTR pet_sched_trace_status_t status;
static DRAM_ATTR uint32_t next_slot;
static DRAM_ATTR pet_sched_actor_summary_t summaries[3];
static DRAM_ATTR uint32_t phase_started[3];

static void IRAM_ATTR increment(uint32_t *value){if(*value<UINT32_MAX)(*value)++;}
static void IRAM_ATTR interval(uint32_t elapsed,uint64_t *total,uint32_t *maximum,uint32_t *pairs){
    *total+=elapsed;if(elapsed>*maximum)*maximum=elapsed;increment(pairs);
}
/* Called only under trace_mux, with a valid bounded window clock. No task or
 * kernel APIs here. Different registered actors have independent phases. */
static void IRAM_ATTR summarize(uint8_t who,uint8_t kind,uint32_t now,uint32_t value){
    if(who<PET_SCHED_TASK_MAIN||who>PET_SCHED_TASK_PROTOCOL)return;
    unsigned at=who-1U;pet_sched_actor_summary_t *s=&summaries[at];
    if(kind==PET_SCHED_INHERIT_RETURN){increment(value?&s->inherit_nonzero:&s->inherit_zero);return;}
    if(kind==PET_SCHED_DISINHERIT_RETURN){increment(value?&s->disinherit_nonzero:&s->disinherit_zero);return;}
    if(kind<PET_SCHED_FS_WAIT||kind>PET_SCHED_FS_RELEASE_RETURN)return;
    bool ordered=now>=phase_started[at];
    if(kind==PET_SCHED_FS_WAIT){
        if(s->phase)increment(&s->unpaired);
        s->phase=1;
    }else if(kind==PET_SCHED_FS_ACQUIRED){
        if(s->phase==1&&ordered)interval(now-phase_started[at],&s->wait_total_us,&s->wait_max_us,&s->wait_pairs);
        else increment(&s->unpaired);
        s->phase=2;
    }else if(kind==PET_SCHED_FS_RELEASE_BEGIN){
        if(s->phase==2&&ordered)interval(now-phase_started[at],&s->hold_before_release_total_us,&s->hold_before_release_max_us,&s->hold_pairs);
        else increment(&s->unpaired);
        s->phase=3;
    }else{
        if(s->phase==3&&ordered)interval(now-phase_started[at],&s->release_total_us,&s->release_max_us,&s->release_pairs);
        else increment(&s->unpaired);
        s->phase=0;
    }
    phase_started[at]=now;
}

/* Only trace metadata is touched while trace_mux is held. In particular, never
 * call __real_*, a priority getter, task lookup, allocation, or an output API.
 * The clock is the SDK's IRAM systimer getter (no RTOS lock/allocation). It is
 * sampled before trace_mux; local IRQ masking keeps core/TCB/time consistent. */
typedef struct { TaskHandle_t task; uint32_t generation; } observation_t;
static uint8_t IRAM_ATTR classify(TaskHandle_t task){
    for(unsigned i=1;i<4;i++)if(task && task_handles[i]==task)return (uint8_t)i;
    return PET_SCHED_TASK_OTHER;
}
static void IRAM_ATTR close_locked(uint64_t now,uint32_t flag){
    status.flags|=flag;status.active=false;status.ended_us=now;
    atomic_store_explicit(&active_generation,0,memory_order_release);
}
static observation_t IRAM_ATTR observe(uint8_t kind,TaskHandle_t actor,
                                       TaskHandle_t peer,uint32_t value,
                                       uint16_t marker,uint32_t expected){
    observation_t result={0};
    uint32_t generation=atomic_load_explicit(&active_generation,memory_order_acquire);
    if(!generation || (expected && generation!=expected))return result;
    UBaseType_t irq=portSET_INTERRUPT_MASK_FROM_ISR();
    unsigned core=(unsigned)portGET_CORE_ID();
    TaskHandle_t current=core<2?pxCurrentTCBs[core]:NULL;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL_ISR(&trace_mux);
    if(generation==atomic_load_explicit(&active_generation,memory_order_relaxed)){
        if(core>=2)close_locked(status.started_us,PET_SCHED_FLAG_BAD_CORE);
        else if(now<0 || (uint64_t)now<status.started_us)close_locked(status.started_us,PET_SCHED_FLAG_BAD_CLOCK);
        else if((uint64_t)now-status.started_us>=status.duration_us)close_locked((uint64_t)now,PET_SCHED_FLAG_EXPIRED);
        else{
            result.task=actor?actor:current;result.generation=generation;
            uint8_t who=classify(result.task),other=classify(peer);
            /* Unrelated mutex activity need not fill the ring. FS and switches
             * retain OTHER, including other-to-other selections. */
            bool relevant=(kind!=PET_SCHED_INHERIT_RETURN && kind!=PET_SCHED_DISINHERIT_RETURN) || who || other;
            if(kind && relevant){
                pet_sched_event_t event={.elapsed_us=(uint32_t)((uint64_t)now-status.started_us),
                    .value=value,.marker=marker,.kind=kind,.core=(uint8_t)core,.actor=who,.peer=other};
                summarize(who,kind,event.elapsed_us,value);
                events[next_slot]=event;next_slot=(next_slot+1U)&(PET_SCHED_TRACE_CAPACITY-1U);
                if(status.count<PET_SCHED_TRACE_CAPACITY)status.count++;
                else{status.flags|=PET_SCHED_FLAG_OVERFLOW;if(status.overwritten<UINT32_MAX)status.overwritten++;}
            }
        }
    }
    portEXIT_CRITICAL_ISR(&trace_mux);
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    return result;
}

bool IRAM_ATTR pet_sched_trace_register(pet_sched_task_t task,void *handle){
    if(task<PET_SCHED_TASK_MAIN || task>PET_SCHED_TASK_PROTOCOL || !handle)return false;
    bool ok=false;
    portENTER_CRITICAL_ISR(&trace_mux);
    if(!status.active){
        ok=true;for(unsigned i=1;i<4;i++)if(i!=(unsigned)task && task_handles[i]==handle)ok=false;
        if(ok)task_handles[task]=handle;
    }
    portEXIT_CRITICAL_ISR(&trace_mux);return ok;
}
uint32_t IRAM_ATTR pet_sched_trace_begin(uint32_t duration_us){
    if(!duration_us || duration_us>PET_SCHED_TRACE_MAX_WINDOW_US)return 0;
    int64_t now=esp_timer_get_time();if(now<0)return 0;
    uint32_t generation=0;
    portENTER_CRITICAL_ISR(&trace_mux);
    if(!status.active && task_handles[1] && task_handles[2] && task_handles[3] && status.generation<UINT32_MAX){
        generation=status.generation+1U;
        /* No ring clearing/copying in this critical section. Old slots are
         * unreachable until overwritten by this generation's counted events. */
        status=(pet_sched_trace_status_t){.generation=generation,.duration_us=duration_us,
            .flags=PET_SCHED_FLAG_PARTIAL_WRAP_COVERAGE,.started_us=(uint64_t)now,.active=true};
        /* Three small fixed records only, not the 16 KiB raw ring. */
        for(unsigned i=0;i<3;i++){summaries[i]=(pet_sched_actor_summary_t){0};phase_started[i]=0;}
        next_slot=0;atomic_store_explicit(&active_generation,generation,memory_order_release);
    }
    portEXIT_CRITICAL_ISR(&trace_mux);return generation;
}
bool IRAM_ATTR pet_sched_trace_stop(uint32_t generation){
    int64_t now=esp_timer_get_time();bool ok=false;
    portENTER_CRITICAL_ISR(&trace_mux);
    if(generation && status.generation==generation){
        ok=true;
        if(status.active){
            uint32_t flag=0;uint64_t end=status.started_us;
            if(now<0 || (uint64_t)now<status.started_us)flag=PET_SCHED_FLAG_BAD_CLOCK;
            else{end=(uint64_t)now;if(end-status.started_us>=status.duration_us)flag=PET_SCHED_FLAG_EXPIRED;}
            close_locked(end,flag);
        }
    }
    portEXIT_CRITICAL_ISR(&trace_mux);return ok;
}
void IRAM_ATTR pet_sched_trace_mark(pet_sched_marker_t marker,uint32_t tag){
    if(marker<PET_SCHED_MARK_PPA_BEGIN || marker>PET_SCHED_MARK_PROGRAM_END)return;
    (void)observe(PET_SCHED_MARK,NULL,NULL,tag,(uint16_t)marker,0);
}
bool IRAM_ATTR pet_sched_trace_status(uint32_t generation,pet_sched_trace_status_t *out){
    if(!out || !generation)return false;
    bool ok=false;
    portENTER_CRITICAL_ISR(&trace_mux);
    if(status.generation==generation){*out=status;ok=true;}
    portEXIT_CRITICAL_ISR(&trace_mux);return ok;
}
bool IRAM_ATTR pet_sched_trace_read(uint32_t generation,uint32_t index,pet_sched_event_t *out){
    if(!out || !generation)return false;
    bool ok=false;
    portENTER_CRITICAL_ISR(&trace_mux);
    if(status.generation==generation && !status.active && index<status.count){
        uint32_t first=status.count==PET_SCHED_TRACE_CAPACITY?next_slot:0;
        *out=events[(first+index)&(PET_SCHED_TRACE_CAPACITY-1U)];ok=true;
    }
    portEXIT_CRITICAL_ISR(&trace_mux);return ok;
}
bool IRAM_ATTR pet_sched_trace_summary(uint32_t generation,pet_sched_task_t actor,pet_sched_actor_summary_t *out){
    if(!out||!generation||actor<PET_SCHED_TASK_MAIN||actor>PET_SCHED_TASK_PROTOCOL)return false;
    bool ok=false;portENTER_CRITICAL_ISR(&trace_mux);
    if(status.generation==generation&&!status.active){*out=summaries[actor-1U];ok=true;}
    portEXIT_CRITICAL_ISR(&trace_mux);return ok;
}

void IRAM_ATTR __wrap_spiffs_api_lock(spiffs *fs){
    observation_t entry=observe(PET_SCHED_FS_WAIT,NULL,NULL,0,0,0);
    __real_spiffs_api_lock(fs);
    if(entry.generation)(void)observe(PET_SCHED_FS_ACQUIRED,entry.task,NULL,0,0,entry.generation);
}
void IRAM_ATTR __wrap_spiffs_api_unlock(spiffs *fs){
    observation_t entry=observe(PET_SCHED_FS_RELEASE_BEGIN,NULL,NULL,0,0,0);
    __real_spiffs_api_unlock(fs);
    if(entry.generation)(void)observe(PET_SCHED_FS_RELEASE_RETURN,entry.task,NULL,0,0,entry.generation);
}
BaseType_t IRAM_ATTR __wrap_xTaskPriorityInherit(TaskHandle_t holder){
    observation_t entry=observe(0,NULL,NULL,0,0,0);
    BaseType_t result=__real_xTaskPriorityInherit(holder);
    if(entry.generation)(void)observe(PET_SCHED_INHERIT_RETURN,entry.task,holder,(uint32_t)result,0,entry.generation);
    return result;
}
BaseType_t IRAM_ATTR __wrap_xTaskPriorityDisinherit(TaskHandle_t holder){
    observation_t entry=observe(0,NULL,NULL,0,0,0);
    BaseType_t result=__real_xTaskPriorityDisinherit(holder);
    if(entry.generation)(void)observe(PET_SCHED_DISINHERIT_RETURN,entry.task,holder,(uint32_t)result,0,entry.generation);
    return result;
}
void IRAM_ATTR __wrap_vTaskSwitchContext(void){
    observation_t entry=observe(0,NULL,NULL,0,0,0);
    __real_vTaskSwitchContext();
    if(entry.generation){
        UBaseType_t irq=portSET_INTERRUPT_MASK_FROM_ISR();
        unsigned core=(unsigned)portGET_CORE_ID();
        TaskHandle_t selected=core<2?pxCurrentTCBs[core]:NULL;
        if(core>=2 || selected!=entry.task)(void)observe(PET_SCHED_SWITCH_SELECT,entry.task,selected,0,0,entry.generation);
        portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    }
}
#endif
