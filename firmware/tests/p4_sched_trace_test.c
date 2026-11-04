#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_sched_trace.h"
#include "freertos/task.h"

_Thread_local unsigned sched_test_core,sched_test_irq,sched_test_trace_depth;
TaskHandle_t volatile pxCurrentTCBs[2];
static atomic_llong clock_us;
static int main_task,save_task,protocol_task,other_task;
static unsigned locks,unlocks,inherits,disinherits,switches;
static uint32_t outer_generation,new_generation;
static unsigned scenario;
static BaseType_t disinherit_result=-3;
static TaskHandle_t next_task;
typedef struct spiffs_t spiffs;
void __wrap_spiffs_api_lock(spiffs *);
void __wrap_spiffs_api_unlock(spiffs *);
BaseType_t __wrap_xTaskPriorityInherit(TaskHandle_t);
BaseType_t __wrap_xTaskPriorityDisinherit(TaskHandle_t);
void __wrap_vTaskSwitchContext(void);
int64_t esp_timer_get_time(void){assert(!sched_test_trace_depth);return atomic_load(&clock_us);}
static void outside(void){assert(!sched_test_trace_depth);}
void __real_spiffs_api_lock(spiffs *fs){
    (void)fs;outside();locks++;
    if(scenario==1){
        assert(__wrap_xTaskPriorityInherit(&save_task)==7);
        atomic_fetch_add(&clock_us,20);
        /* A blocked task can resume on another core. */
        pxCurrentTCBs[1]=pxCurrentTCBs[0];sched_test_core=1;
    }else if(scenario==2){
        assert(pet_sched_trace_stop(outer_generation));
        new_generation=pet_sched_trace_begin(1000);assert(new_generation && new_generation!=outer_generation);
    }else if(scenario==3){atomic_fetch_add(&clock_us,1000);}
}
void __real_spiffs_api_unlock(spiffs *fs){(void)fs;outside();unlocks++;}
BaseType_t __real_xTaskPriorityInherit(TaskHandle_t holder){(void)holder;outside();inherits++;return 7;}
BaseType_t __real_xTaskPriorityDisinherit(TaskHandle_t holder){(void)holder;outside();disinherits++;return disinherit_result;}
void __real_vTaskSwitchContext(void){outside();switches++;pxCurrentTCBs[sched_test_core]=next_task;}
static pet_sched_trace_status_t stopped(uint32_t generation){
    pet_sched_trace_status_t info;assert(pet_sched_trace_stop(generation));assert(pet_sched_trace_status(generation,&info));assert(!info.active);return info;
}
static pet_sched_event_t event(uint32_t generation,unsigned index){
    pet_sched_event_t out;memset(&out,0xa5,sizeof(out));assert(pet_sched_trace_read(generation,index,&out));
    assert(out.core<2 && out.actor<=3 && out.peer<=3 && !out.reserved[0] && !out.reserved[1]);return out;
}
static void setup(void){
    atomic_store(&clock_us,1000);pxCurrentTCBs[0]=&main_task;pxCurrentTCBs[1]=&other_task;sched_test_core=0;
    assert(!pet_sched_trace_begin(1));
    assert(!pet_sched_trace_register(PET_SCHED_TASK_OTHER,&other_task));
    assert(!pet_sched_trace_register(PET_SCHED_TASK_MAIN,NULL));
    assert(pet_sched_trace_register(PET_SCHED_TASK_MAIN,&main_task));
    assert(!pet_sched_trace_register(PET_SCHED_TASK_SAVE,&main_task));
    assert(pet_sched_trace_register(PET_SCHED_TASK_SAVE,&save_task));
    assert(pet_sched_trace_register(PET_SCHED_TASK_PROTOCOL,&protocol_task));
    assert(!pet_sched_trace_begin(0));assert(!pet_sched_trace_begin(PET_SCHED_TRACE_MAX_WINDOW_US+1));
}
static void lifecycle(void){
    uint32_t g=pet_sched_trace_begin(1000);assert(g);
    assert(!pet_sched_trace_begin(1000));assert(!pet_sched_trace_register(PET_SCHED_TASK_MAIN,&other_task));
    assert(!pet_sched_trace_stop(g+1));
    pet_sched_event_t out;assert(!pet_sched_trace_read(g,0,&out));
    pet_sched_trace_mark(PET_SCHED_MARK_PPA_BEGIN,1328);
    pxCurrentTCBs[0]=&protocol_task;scenario=1;__wrap_spiffs_api_lock(NULL);
    assert(!sched_test_irq);assert(__wrap_xTaskPriorityDisinherit(&save_task)==-3);
    __wrap_spiffs_api_unlock(NULL);scenario=0;
    next_task=&main_task;__wrap_vTaskSwitchContext();
    __wrap_vTaskSwitchContext(); /* unchanged selection is not another switch */
    pet_sched_trace_mark(PET_SCHED_MARK_PPA_RETURN,1328);
    pet_sched_trace_status_t info=stopped(g);assert(info.count==9 && info.flags==PET_SCHED_FLAG_PARTIAL_WRAP_COVERAGE);
    assert(event(g,0).kind==PET_SCHED_MARK && event(g,0).value==1328);
    assert(event(g,1).kind==PET_SCHED_FS_WAIT && event(g,1).actor==PET_SCHED_TASK_PROTOCOL);
    assert(event(g,2).kind==PET_SCHED_INHERIT_RETURN && event(g,2).peer==PET_SCHED_TASK_SAVE && event(g,2).value==7);
    assert(event(g,3).kind==PET_SCHED_FS_ACQUIRED && event(g,3).core==1 && event(g,3).elapsed_us==20);
    assert(event(g,4).value==(uint32_t)-3);
    assert(event(g,7).kind==PET_SCHED_SWITCH_SELECT && event(g,7).actor==PET_SCHED_TASK_PROTOCOL && event(g,7).peer==PET_SCHED_TASK_MAIN);
    assert(!pet_sched_trace_read(g,info.count,&out));assert(!pet_sched_trace_read(g+1,0,&out));
    pet_sched_trace_mark(PET_SCHED_MARK_SAVE_BEGIN,9);assert(stopped(g).count==info.count);
    assert(locks==1 && unlocks==1 && inherits==1 && disinherits==1 && switches==2);
}
static void boundaries(void){
    sched_test_core=0;pxCurrentTCBs[0]=&main_task;
    outer_generation=pet_sched_trace_begin(1000);scenario=2;__wrap_spiffs_api_lock(NULL);scenario=0;
    /* Late acquire from the old physical operation cannot pollute a new window. */
    assert(stopped(new_generation).count==0);
    uint32_t g=pet_sched_trace_begin(1000);scenario=3;__wrap_spiffs_api_lock(NULL);scenario=0;
    pet_sched_trace_status_t info=stopped(g);assert(info.count==1 && (info.flags&PET_SCHED_FLAG_EXPIRED));
    g=pet_sched_trace_begin(1000);atomic_store(&clock_us,-1);pet_sched_trace_mark(PET_SCHED_MARK_PPA_BEGIN,0);
    info=stopped(g);assert(info.count==0 && (info.flags&PET_SCHED_FLAG_BAD_CLOCK));
    assert(!pet_sched_trace_begin(1000));atomic_store(&clock_us,3000);
    g=pet_sched_trace_begin(1000);atomic_store(&clock_us,2999);pet_sched_trace_mark(PET_SCHED_MARK_PPA_RETURN,0);
    assert(stopped(g).flags&PET_SCHED_FLAG_BAD_CLOCK);
    atomic_store(&clock_us,4000);g=pet_sched_trace_begin(1000);sched_test_core=2;
    pet_sched_trace_mark(PET_SCHED_MARK_SAVE_BEGIN,0);sched_test_core=0;assert(stopped(g).flags&PET_SCHED_FLAG_BAD_CORE);
    g=pet_sched_trace_begin(1000);
    pet_sched_trace_mark((pet_sched_marker_t)0,0);pet_sched_trace_mark((pet_sched_marker_t)8,0);
    for(unsigned i=0;i<PET_SCHED_TRACE_CAPACITY+17;i++)pet_sched_trace_mark(PET_SCHED_MARK_SAVE_BEGIN,i);
    info=stopped(g);assert(info.count==PET_SCHED_TRACE_CAPACITY && info.overwritten==17 && (info.flags&PET_SCHED_FLAG_OVERFLOW));
    assert(event(g,0).value==17 && event(g,info.count-1).value==PET_SCHED_TRACE_CAPACITY+16);
}
static void *writer(void *arg){
    sched_test_core=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<300;i++)pet_sched_trace_mark(PET_SCHED_MARK_PPA_CALLBACK,1000U*sched_test_core+i);
    assert(!sched_test_irq && !sched_test_trace_depth);return NULL;
}
static void concurrency(void){
    pxCurrentTCBs[0]=&main_task;pxCurrentTCBs[1]=&save_task;
    uint32_t g=pet_sched_trace_begin(1000);pthread_t a,b;
    assert(!pthread_create(&a,NULL,writer,(void *)(uintptr_t)0));assert(!pthread_create(&b,NULL,writer,(void *)(uintptr_t)1));
    assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
    pet_sched_trace_status_t info=stopped(g);assert(info.count==600 && !(info.flags&PET_SCHED_FLAG_OVERFLOW));
    unsigned seen[2]={0};for(unsigned i=0;i<info.count;i++){pet_sched_event_t e=event(g,i);seen[e.core]++;assert(e.actor==(e.core?PET_SCHED_TASK_SAVE:PET_SCHED_TASK_MAIN));}
    assert(seen[0]==300 && seen[1]==300);
}
static void summaries_survive_raw_overflow(void){
    sched_test_core=0;pxCurrentTCBs[0]=&protocol_task;scenario=0;
    atomic_store(&clock_us,10000);uint32_t g=pet_sched_trace_begin(600000);assert(g);
    pet_sched_actor_summary_t out;memset(&out,0xa5,sizeof(out));
    assert(!pet_sched_trace_summary(g,PET_SCHED_TASK_PROTOCOL,&out)&&out.phase==0xa5);
    disinherit_result=0;
    for(unsigned i=0;i<1600;i++){
        atomic_fetch_add(&clock_us,5);__wrap_spiffs_api_lock(NULL);
        atomic_fetch_add(&clock_us,7);__wrap_spiffs_api_unlock(NULL);
        assert(__wrap_xTaskPriorityDisinherit(&save_task)==0);
    }
    disinherit_result=-3;pet_sched_trace_status_t info=stopped(g);
    assert(info.flags&PET_SCHED_FLAG_OVERFLOW);assert(info.overwritten==8000-PET_SCHED_TRACE_CAPACITY);
    assert(pet_sched_trace_summary(g,PET_SCHED_TASK_PROTOCOL,&out));
    assert(out.wait_pairs==1600&&out.hold_pairs==1600&&out.release_pairs==1600);
    assert(!out.wait_total_us&&!out.wait_max_us&&out.hold_before_release_total_us==11200&&out.hold_before_release_max_us==7);
    assert(!out.release_total_us&&!out.release_max_us&&!out.unpaired&&!out.phase);
    assert(out.disinherit_zero==1600&&!out.disinherit_nonzero&&!out.inherit_nonzero);
    pet_sched_actor_summary_t before=out;
    assert(!pet_sched_trace_summary(g,PET_SCHED_TASK_OTHER,&out)&&!memcmp(&out,&before,sizeof(out)));
    assert(!pet_sched_trace_summary(g+1,PET_SCHED_TASK_PROTOCOL,&out)&&!memcmp(&out,&before,sizeof(out)));
    g=pet_sched_trace_begin(1000);__wrap_spiffs_api_lock(NULL);info=stopped(g);
    assert(!(info.flags&PET_SCHED_FLAG_OVERFLOW));assert(pet_sched_trace_summary(g,PET_SCHED_TASK_PROTOCOL,&out));
    assert(out.wait_pairs==1&&!out.hold_pairs&&!out.hold_before_release_total_us&&out.phase==2&&!out.disinherit_zero);
    g=pet_sched_trace_begin(1000);__wrap_spiffs_api_unlock(NULL);(void)stopped(g);
    assert(pet_sched_trace_summary(g,PET_SCHED_TASK_PROTOCOL,&out));
    assert(out.unpaired==1&&!out.hold_pairs&&out.release_pairs==1&&!out.phase);
    /* OTHER from two distinct tasks cannot become one invented wait/hold. */
    g=pet_sched_trace_begin(1000);pxCurrentTCBs[0]=&other_task;__wrap_spiffs_api_lock(NULL);
    pxCurrentTCBs[0]=(TaskHandle_t)(uintptr_t)42;__wrap_spiffs_api_unlock(NULL);(void)stopped(g);
    assert(pet_sched_trace_summary(g,PET_SCHED_TASK_PROTOCOL,&out));assert(!out.wait_pairs&&!out.unpaired&&!out.phase);
}
int main(void){setup();lifecycle();boundaries();concurrency();summaries_survive_raw_overflow();return 0;}
