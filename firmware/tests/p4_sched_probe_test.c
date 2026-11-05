/* Actual adapter, actual trace and actual broker; only platform/audio/physical
 * SDK calls are stubs. Interleavings never touch hardware, files or user data. */
#include "pet_p4_sched_probe.h"
#include "pet_p4_resources_device.h"
#include "freertos/task.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Include the unchanged production unit solely so exhaustion tests can seed
 * private counters in this test translation unit. No product seeding API. */
#include "pet_p4_sched_probe.c"

_Thread_local unsigned probe_test_core,probe_test_irq,probe_test_lock_depth;
static _Thread_local unsigned blocking_site;
TaskHandle_t volatile pxCurrentTCBs[2];
static int main_task,save_task,protocol_task,other_task;
static atomic_llong clock_us=1000000;
static atomic_bool flags[5];
static atomic_uint reads[5],finish_calls,physical_calls;
static int acquire_audio=-1,begin_audio=-1;
static bool begin_fail,finish_fail;
static pthread_mutex_t barrier=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool entered,released;
static pet_sched_probe_owner_t owner;
static pet_sched_probe_ticket_t capture;
static pet_sched_probe_save_t save;
static pet_sched_probe_result_t thread_result;
static void outside(void){assert(!probe_test_lock_depth);}
static void pause_at(unsigned site){
    if(blocking_site!=site)return;
    outside();assert(!pthread_mutex_lock(&barrier));entered=true;pthread_cond_broadcast(&changed);
    while(!released)pthread_cond_wait(&changed,&barrier);
    pthread_mutex_unlock(&barrier);blocking_site=0;
}
static void wait_entered(void){pthread_mutex_lock(&barrier);while(!entered)pthread_cond_wait(&changed,&barrier);pthread_mutex_unlock(&barrier);}
static void release_thread(void){pthread_mutex_lock(&barrier);released=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&barrier);}
int64_t esp_timer_get_time(void){outside();pause_at(3);return atomic_load(&clock_us);}
static bool flag(unsigned i){outside();atomic_fetch_add(&reads[i],1);if(i==4)pause_at(2);return atomic_load(&flags[i]);}
bool pet_p4_audio_active(void){return flag(0);}
bool pet_p4_audio_playback_active(void){return flag(1);}
bool pet_p4_audio_conversation_active(void){return flag(2);}
bool pet_p4_audio_music_active(void){return flag(3);}
bool pet_p4_audio_stream_playing(void){return flag(4);}
pet_resource_result_t probe_real_acquire(pet_resource_owner_t,pet_resource_kind_t,uint32_t,pet_resource_lease_t *);
pet_resource_finish_t probe_real_finish(pet_resource_lease_t);
uint32_t probe_real_trace_begin(uint32_t);
pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t o,pet_resource_kind_t k,uint32_t u,pet_resource_lease_t *out){
    outside();pet_resource_result_t r=probe_real_acquire(o,k,u,out);pause_at(1);
    if(acquire_audio>=0)atomic_store(&flags[acquire_audio],true);
    return r;
}
pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t l){
    outside();atomic_fetch_add(&finish_calls,1);return finish_fail?PET_RESOURCE_FINISH_STALE:probe_real_finish(l);
}
uint32_t pet_sched_trace_begin(uint32_t us){
    outside();uint32_t g=begin_fail?0:probe_real_trace_begin(us);
    if(begin_audio>=0)atomic_store(&flags[begin_audio],true);
    return g;
}
typedef struct spiffs_t spiffs;
void __real_spiffs_api_lock(spiffs *fs){(void)fs;outside();atomic_fetch_add(&physical_calls,1);}
void __real_spiffs_api_unlock(spiffs *fs){(void)fs;outside();}
BaseType_t __real_xTaskPriorityInherit(TaskHandle_t h){(void)h;outside();return 1;}
BaseType_t __real_xTaskPriorityDisinherit(TaskHandle_t h){(void)h;outside();return 0;}
void __real_vTaskSwitchContext(void){outside();}
void __wrap_spiffs_api_lock(spiffs *);
void __wrap_spiffs_api_unlock(spiffs *);
static unsigned leases(void){pet_resource_usage_t u;pet_resources_device_usage(&u);assert(u.leases[PET_RESOURCE_NATIVE_PROBE]<=1);return u.leases[PET_RESOURCE_NATIVE_PROBE];}
static pet_sched_probe_status_t status(void){pet_sched_probe_status_t s;assert(pet_sched_probe_status(owner,&s)==PET_SCHED_PROBE_OK);return s;}
static void setup(void){
    pxCurrentTCBs[0]=&main_task;pxCurrentTCBs[1]=&other_task;
    assert(pet_sched_probe_init(NULL,&save_task,&protocol_task)==PET_SCHED_PROBE_INVALID);
    assert(pet_sched_probe_init(&main_task,&save_task,&protocol_task)==PET_SCHED_PROBE_OK);
    assert(pet_sched_probe_owner_open(&owner)==PET_SCHED_PROBE_OK);
    assert(status().state==PET_SCHED_PROBE_IDLE && !leases());
}
static void arm(void){pet_sched_probe_status_t s=status();capture=(pet_sched_probe_ticket_t){0};assert(pet_sched_probe_arm(s.ticket,600000,&capture)==PET_SCHED_PROBE_OK);assert(capture.revision>s.ticket.revision && leases()==1);}
static void start(void){arm();pxCurrentTCBs[0]=&save_task;save=pet_sched_probe_save_begin(4);assert(save.trace_generation);assert(status().save_inflight);}
static void finish_save(void){atomic_fetch_add(&clock_us,100);__wrap_spiffs_api_lock(NULL);__wrap_spiffs_api_unlock(NULL);assert(pet_sched_probe_save_end(save,true)==PET_SCHED_PROBE_OK);}
static void basic(void){
    pet_sched_probe_status_t before=status();arm();
    pet_sched_probe_ticket_t ignored={0};assert(pet_sched_probe_arm(before.ticket,600000,&ignored)==PET_SCHED_PROBE_STALE);
    pet_resource_owner_t competitor={0};pet_resource_lease_t lease={0};
    assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_NATIVE_PROBE),&competitor)==PET_RESOURCE_OK);
    assert(pet_resources_device_acquire(competitor,PET_RESOURCE_NATIVE_PROBE,1,&lease)==PET_RESOURCE_BUSY);pet_resources_device_revoke(competitor);
    save=pet_sched_probe_save_begin(4);assert(save.trace_generation);assert(!pet_sched_probe_save_begin(5).trace_generation);
    for(unsigned i=0;i<40;i++)pet_sched_probe_ppa_mark(PET_SCHED_MARK_PPA_BEGIN,i);
    finish_save();pet_sched_probe_status_t s=status();assert(s.state==PET_SCHED_PROBE_FROZEN && s.reason==PET_SCHED_PROBE_COMPLETE && !s.save_inflight && s.save_ok && leases()==1);
    assert(pet_sched_probe_save_end(save,false)==PET_SCHED_PROBE_OK);assert(status().save_ok);
    pet_sched_probe_page_t a,b;assert(pet_sched_probe_read(capture,0,&a)==PET_SCHED_PROBE_OK);assert(a.count==16 && !a.done);
    assert(pet_sched_probe_read(capture,0,&b)==PET_SCHED_PROBE_OK);assert(!memcmp(&a,&b,sizeof(a)));
    unsigned count=a.count;
    while(!a.done){assert(pet_sched_probe_read(capture,a.next_index,&a)==PET_SCHED_PROBE_OK);count+=a.count;}
    assert(count==s.trace.count);assert(pet_sched_probe_read(capture,count+1,&a)==PET_SCHED_PROBE_INVALID);
    assert(!pet_sched_probe_save_begin(9).trace_generation);assert(status().trace.count==count);
    assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_OK);assert(!leases() && atomic_load(&finish_calls)==1);
    pet_sched_probe_save_t old=save;pet_sched_probe_ticket_t old_capture=capture;arm();
    assert(pet_sched_probe_save_end(old,true)==PET_SCHED_PROBE_STALE);assert(pet_sched_probe_read(old_capture,0,&a)==PET_SCHED_PROBE_STALE);
    assert(pet_sched_probe_cancel(capture)==PET_SCHED_PROBE_OK);assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_OK);
    pet_sched_probe_owner_t old_owner=owner;assert(pet_sched_probe_owner_revoke(owner)==PET_SCHED_PROBE_OK);
    owner=(pet_sched_probe_owner_t){0};assert(pet_sched_probe_owner_open(&owner)==PET_SCHED_PROBE_OK);assert(owner.generation>old_owner.generation);
    assert(pet_sched_probe_status(old_owner,&s)==PET_SCHED_PROBE_STALE);assert(!leases());
}
static void audio_arm(bool second,unsigned selected){
    pet_sched_probe_ticket_t expected=status().ticket,out={0};
    for(unsigned i=0;i<5;i++)atomic_store(&reads[i],0);
    if(second)acquire_audio=(int)selected;else atomic_store(&flags[selected],true);
    assert(pet_sched_probe_arm(expected,600000,&out)==PET_SCHED_PROBE_AUDIO_BUSY && !out.owner && !leases());
    for(unsigned i=0;i<5;i++)assert(atomic_load(&reads[i])==(second?2U:1U));
    assert(atomic_load(&finish_calls)==(second?1U:0U));
}
static void audio_save(bool second,unsigned selected){
    arm();if(second)begin_audio=(int)selected;else atomic_store(&flags[selected],true);
    save=pet_sched_probe_save_begin(4);pet_sched_probe_status_t s=status();assert(s.state==PET_SCHED_PROBE_FROZEN && s.reason==PET_SCHED_PROBE_AUDIO && leases()==1);
    assert((save.trace_generation!=0)==second);assert(s.save_inflight==second);
    atomic_store(&flags[selected],false);begin_audio=-1;
    assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_OK);
    if(second){assert(leases()==1 && !atomic_load(&finish_calls));finish_save();}
    assert(!leases() && atomic_load(&finish_calls)==1);
}
static void cancel_ttl(void){
    start();assert(pet_sched_probe_cancel(capture)==PET_SCHED_PROBE_OK);
    assert(status().state==PET_SCHED_PROBE_FROZEN && status().save_inflight);
    atomic_fetch_add(&clock_us,PET_SCHED_PROBE_FROZEN_US);assert(pet_sched_probe_poll()==PET_SCHED_PROBE_OK);
    assert(status().state==PET_SCHED_PROBE_DRAINING && leases()==1 && !atomic_load(&finish_calls));
    pet_sched_probe_save_t wrong=save;wrong.trace_generation++;assert(pet_sched_probe_save_end(wrong,true)==PET_SCHED_PROBE_STALE);
    finish_save();assert(!leases() && atomic_load(&finish_calls)==1 && atomic_load(&physical_calls)==1);
    assert(!pet_sched_probe_save_begin(8).trace_generation);
}
static void arm_ttl(void){
    arm();atomic_fetch_add(&clock_us,PET_SCHED_PROBE_ARM_US);assert(pet_sched_probe_poll()==PET_SCHED_PROBE_OK);
    assert(status().reason==PET_SCHED_PROBE_ARM_TIMEOUT && status().state==PET_SCHED_PROBE_FROZEN && leases()==1);
    assert(!pet_sched_probe_save_begin(5).trace_generation);
    atomic_fetch_add(&clock_us,PET_SCHED_PROBE_FROZEN_US);assert(pet_sched_probe_poll()==PET_SCHED_PROBE_OK);
    assert(status().state==PET_SCHED_PROBE_IDLE && !leases());
}
static void clock_failure(bool negative){
    start();atomic_store(&clock_us,negative?-1:999999);assert(pet_sched_probe_poll()==PET_SCHED_PROBE_OK);
    assert(status().reason==PET_SCHED_PROBE_CLOCK && status().save_inflight);
    assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_OK);assert(leases()==1);
    atomic_store(&clock_us,2000000);finish_save();assert(!leases() && atomic_load(&finish_calls)==1);
}
static void finish_failure(void){
    arm();begin_fail=true;assert(!pet_sched_probe_save_begin(4).trace_generation);assert(status().reason==PET_SCHED_PROBE_TRACE_ERROR);
    finish_fail=true;assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_FAULT);assert(leases()==1 && atomic_load(&finish_calls)==1);
    for(unsigned i=0;i<5;i++)assert(pet_sched_probe_poll()==PET_SCHED_PROBE_FAULT);
    assert(status().state==PET_SCHED_PROBE_FAILED && atomic_load(&finish_calls)==1);
    pet_sched_probe_ticket_t ignored={0};assert(pet_sched_probe_arm(capture,600000,&ignored)!=PET_SCHED_PROBE_OK);
}
static void owner_loss(void){
    start();assert(pet_sched_probe_owner_revoke(owner)==PET_SCHED_PROBE_OK);assert(leases()==1);
    pet_sched_probe_status_t s;assert(pet_sched_probe_status(owner,&s)==PET_SCHED_PROBE_STALE);
    pet_sched_probe_owner_t replacement={0};assert(pet_sched_probe_owner_open(&replacement)==PET_SCHED_PROBE_BUSY);
    finish_save();assert(!leases());assert(pet_sched_probe_owner_open(&replacement)==PET_SCHED_PROBE_OK);
    assert(pet_sched_probe_save_end(save,true)==PET_SCHED_PROBE_STALE);
}
static void *blocked_arm(void *unused){(void)unused;blocking_site=1;pet_sched_probe_ticket_t out={0};thread_result=pet_sched_probe_arm(capture,600000,&out);assert(!out.owner);return NULL;}
static void *blocked_status(void *unused){(void)unused;blocking_site=2;pet_sched_probe_status_t s;thread_result=pet_sched_probe_status(owner,&s);return NULL;}
static void *blocked_status_clock(void *unused){(void)unused;blocking_site=3;pet_sched_probe_status_t s;thread_result=pet_sched_probe_status(owner,&s);return NULL;}
static void *blocked_marker(void *unused){(void)unused;blocking_site=3;pet_sched_probe_ppa_mark(PET_SCHED_MARK_PPA_CALLBACK,23);return NULL;}
static void revoke_during_arm(void){
    capture=status().ticket;pthread_t t;assert(!pthread_create(&t,NULL,blocked_arm,NULL));wait_entered();
    assert(leases()==1);pet_sched_probe_status_t s;assert(pet_sched_probe_status(owner,&s)==PET_SCHED_PROBE_BUSY);
    assert(pet_sched_probe_cancel(capture)==PET_SCHED_PROBE_BUSY);
    assert(pet_sched_probe_owner_revoke(owner)==PET_SCHED_PROBE_OK);release_thread();assert(!pthread_join(t,NULL));
    assert(thread_result==PET_SCHED_PROBE_STALE && !leases() && atomic_load(&finish_calls)==1);
}
static void end_during_claim(void){
    start();pthread_t t;assert(!pthread_create(&t,NULL,blocked_status,NULL));wait_entered();
    assert(pet_sched_probe_save_end(save,true)==PET_SCHED_PROBE_OK);assert(leases()==1 && !atomic_load(&finish_calls));
    release_thread();assert(!pthread_join(t,NULL));assert(thread_result==PET_SCHED_PROBE_OK);
    assert(pet_sched_probe_poll()==PET_SCHED_PROBE_OK);assert(!status().save_inflight && status().state==PET_SCHED_PROBE_FROZEN);
    assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_OK);assert(!leases() && atomic_load(&finish_calls)==1);
}
static void marker_drain(void){
    start();finish_save();assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_OK);start();
    pthread_t t;assert(!pthread_create(&t,NULL,blocked_marker,NULL));wait_entered();
    pet_sched_probe_ppa_mark(PET_SCHED_MARK_PPA_RETURN,23); /* contention is explicit */
    assert(pet_sched_probe_cancel(capture)==PET_SCHED_PROBE_OK);assert(status().marker_contended);
    assert(pet_sched_probe_save_end(save,true)==PET_SCHED_PROBE_OK);assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_OK);
    assert(leases()==1 && status().state==PET_SCHED_PROBE_DRAINING);release_thread();assert(!pthread_join(t,NULL));
    assert(pet_sched_probe_poll()==PET_SCHED_PROBE_OK);assert(!leases() && atomic_load(&finish_calls)==2);
}
static void earlier_completion_than_control(void){
    start();pthread_t t;assert(!pthread_create(&t,NULL,blocked_status_clock,NULL));wait_entered();
    uint64_t completed=(uint64_t)atomic_load(&clock_us);
    assert(pet_sched_probe_save_end(save,true)==PET_SCHED_PROBE_OK);
    atomic_fetch_add(&clock_us,200);release_thread();assert(!pthread_join(t,NULL));
    assert(thread_result==PET_SCHED_PROBE_OK);assert(pet_sched_probe_poll()==PET_SCHED_PROBE_OK);
    pet_sched_probe_status_t s=status();assert(s.reason==PET_SCHED_PROBE_COMPLETE && s.save_completed_us==completed && !s.save_inflight);
    assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_OK);assert(!leases());
}
static void complete_then_audio(void){
    start();atomic_store(&flags[1],true);assert(pet_sched_probe_save_end(save,true)==PET_SCHED_PROBE_OK);
    pet_sched_probe_status_t s=status();assert(s.reason==PET_SCHED_PROBE_COMPLETE && s.save_ok && !s.save_inflight);
    pet_sched_probe_page_t page;assert(pet_sched_probe_read(capture,0,&page)==PET_SCHED_PROBE_AUDIO_BUSY);
    atomic_store(&flags[1],false);assert(pet_sched_probe_read(capture,0,&page)==PET_SCHED_PROBE_OK);
    assert(page.status.reason==PET_SCHED_PROBE_COMPLETE);assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_OK);
}
static void owner_exhaustion(void){
    pet_sched_probe_owner_t old=owner;assert(pet_sched_probe_owner_revoke(owner)==PET_SCHED_PROBE_OK);
    next_owner=UINT64_MAX-1;owner=(pet_sched_probe_owner_t){0};
    assert(pet_sched_probe_owner_open(&owner)==PET_SCHED_PROBE_OK && owner.generation==UINT64_MAX);
    pet_sched_probe_status_t s;assert(pet_sched_probe_status(old,&s)==PET_SCHED_PROBE_STALE);
    assert(pet_sched_probe_status((pet_sched_probe_owner_t){0},&s)==PET_SCHED_PROBE_STALE);
    assert(pet_sched_probe_owner_revoke(owner)==PET_SCHED_PROBE_OK);
    pet_sched_probe_owner_t rejected={0};assert(pet_sched_probe_owner_open(&rejected)!=PET_SCHED_PROBE_OK);
    assert(!rejected.generation && next_owner==UINT64_MAX && !leases());
    assert(pet_sched_probe_status(owner,&s)==PET_SCHED_PROBE_STALE);
}
static void revision_exhaustion_idle(void){
    pet_sched_probe_ticket_t expected=status().ticket,out={0};next_revision=UINT64_MAX;
    assert(pet_sched_probe_arm(expected,600000,&out)==PET_SCHED_PROBE_FAULT);
    assert(!out.owner && !out.revision && next_revision==UINT64_MAX && !leases() && !atomic_load(&finish_calls));
    expected.revision=0;assert(pet_sched_probe_arm(expected,600000,&out)==PET_SCHED_PROBE_STALE);
}
static void revision_exhaustion_inflight(void){
    pet_sched_probe_ticket_t old=status().ticket;next_revision=UINT64_MAX-1;start();
    assert(capture.revision==UINT64_MAX && next_revision==UINT64_MAX && leases()==1);
    assert(pet_sched_probe_cancel(old)==PET_SCHED_PROBE_STALE);
    assert(pet_sched_probe_cancel((pet_sched_probe_ticket_t){owner.generation,0})==PET_SCHED_PROBE_STALE);
    assert(pet_sched_probe_cancel(capture)==PET_SCHED_PROBE_OK);
    assert(pet_sched_probe_discard(capture)==PET_SCHED_PROBE_OK);
    assert(status().state==PET_SCHED_PROBE_DRAINING && leases()==1 && !atomic_load(&finish_calls));
    finish_save();assert(!leases() && atomic_load(&finish_calls)==1);
    assert(status().state==PET_SCHED_PROBE_FAILED && next_revision==UINT64_MAX);
    for(unsigned i=0;i<3;i++)assert(pet_sched_probe_poll()==PET_SCHED_PROBE_FAULT);
    pet_sched_probe_ticket_t out={0};assert(pet_sched_probe_arm(capture,600000,&out)==PET_SCHED_PROBE_FAULT);
    assert(!out.revision && atomic_load(&finish_calls)==1 && !pet_sched_probe_save_begin(9).trace_generation);
}
int main(int argc,char **argv){
    assert(argc>=2);setup();const char *test=argv[1];unsigned selected=argc>2?(unsigned)atoi(argv[2]):0;assert(selected<5);
    if(!strcmp(test,"basic"))basic();
    else if(!strcmp(test,"audio_first"))audio_arm(false,selected);
    else if(!strcmp(test,"audio_second"))audio_arm(true,selected);
    else if(!strcmp(test,"save_audio_first"))audio_save(false,selected);
    else if(!strcmp(test,"save_audio_second"))audio_save(true,selected);
    else if(!strcmp(test,"cancel_ttl"))cancel_ttl();
    else if(!strcmp(test,"arm_ttl"))arm_ttl();
    else if(!strcmp(test,"negative_clock"))clock_failure(true);
    else if(!strcmp(test,"backwards_clock"))clock_failure(false);
    else if(!strcmp(test,"finish_failure"))finish_failure();
    else if(!strcmp(test,"owner_loss"))owner_loss();
    else if(!strcmp(test,"revoke_during_arm"))revoke_during_arm();
    else if(!strcmp(test,"end_during_claim"))end_during_claim();
    else if(!strcmp(test,"marker_drain"))marker_drain();
    else if(!strcmp(test,"earlier_completion"))earlier_completion_than_control();
    else if(!strcmp(test,"complete_then_audio"))complete_then_audio();
    else if(!strcmp(test,"owner_exhaustion"))owner_exhaustion();
    else if(!strcmp(test,"revision_exhaustion_idle"))revision_exhaustion_idle();
    else if(!strcmp(test,"revision_exhaustion_inflight"))revision_exhaustion_inflight();
    else assert(false);
    assert(!probe_test_lock_depth && !probe_test_irq);return 0;
}
