"""Production runtime task lock and snapshot functions under real contention.

The FreeRTOS mutex API is backed by a pthread recursive mutex for this host
test. This proves exclusion/lifetime guards, not P4 interrupt latency or the
target scheduler's priority inheritance.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_runtime_task_mutex_contended_snapshots_and_fail_closed_guards(tmp_path):
    code = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    header = (ROOT / 'main/pet_p4_miniapp.h').read_text()
    lock = code[code.index('static StaticSemaphore_t g_runtime_mutex_storage;'):
                code.index('static pet_storage_lane_t g_storage_lane;')]
    sprites = header[header.index('typedef struct {\n  char id[PET_P4_MINIAPP_SPRITE_ID_MAX]'):
                     header.index('typedef struct {\n  bool active;')]
    image_copy = code[code.index('static bool surface_images_locked('):
                      code.index('bool pet_p4_miniapp_render_snapshot(')]
    getters = image_copy + code[code.index('bool pet_p4_miniapp_get_surface_images('):
                   code.index('bool pet_p4_miniapp_sync_stats(')]
    activation = code[code.index('static pet_p4_miniapp_activation_result_t activate_catalog_index_prepared('):
                      code.index('static bool activate_catalog_index(size_t index)')]
    publish = activation[activation.index('miniapp_runtime_lock();'):activation.index('pet_widget_trace_scope_end(&trace_publish);')]
    assert 'g_runtime = *next;' in publish
    assert 'portENTER_CRITICAL(&g_runtime_lock)' not in code
    # Four new scopes capture/restore/checkpoint/observe program state. The
    # actual parent sanitizer suite exercises them; do not drop the census or
    # accept arbitrary extra locks merely because the schema grew.
    from p4_render_trace_test import function
    optional=function(code,'preserve_unchanged_package')
    assert optional.count('miniapp_runtime_lock();')==optional.count('miniapp_runtime_unlock();')==2
    # Keep the reviewed baseline census; account only for these four named
    # new scopes rather than permitting arbitrary extra model locks.
    census=code.replace(optional,'',1)
    for name in ('form3_workspace_begin','save_component_program','save_app_runtime'):
        scope=function(code,name)
        assert scope.count('miniapp_runtime_lock();')==1
        if name=='form3_workspace_begin':
            assert function(code,'form3_workspace_end').count('miniapp_runtime_unlock();')==1
        else:
            assert scope.count('miniapp_runtime_unlock();')==1
            guard=scope.split('miniapp_runtime_lock();',1)[1].split('miniapp_runtime_unlock();',1)[0]
            assert all(op not in guard for op in ('heap_caps_','fopen(','persist_','enqueue('))
        census=census.replace(scope,'',1)
    install=function(code,'pet_p4_miniapp_install_commit')
    prepare=install.split('pet_cext_publish_ticket_t media_ticket={0};',1)[1].split('if(ext_swap&&!miniapp_ext_prepare(',1)[0]
    assert prepare.count('miniapp_runtime_lock();')==prepare.count('miniapp_runtime_unlock();')==1
    assert 'const bool ext_swap=' in prepare
    assert all(op not in prepare for op in ('heap_caps_','fopen(','pet_cmedia_','write_'))
    census=census.replace(prepare,'',1)
    # Cursor transactions add exactly two validation/publication scopes and one
    # queue publication scope. IO remains outside each named model-lock span.
    for name,count in (('catalog_select_id_durable',2),('catalog_select_id_queued',1)):
        scope=function(code,name)
        assert scope.count('miniapp_runtime_lock();')==scope.count('miniapp_runtime_unlock();')==count
        for section in scope.split('miniapp_runtime_lock();')[1:]:
            locked=section.split('miniapp_runtime_unlock();',1)[0]
            assert all(op not in locked for op in ('catalog_cursor_write(','storage_scope_','fopen(','malloc(','g_catalog_cursor_schedule('))
        census=census.replace(scope,'',1)
    # 58: the original sfx scope only copies cue bits. No other scope grows.
    assert census.count('miniapp_runtime_lock();') == 58
    for section in optional.split('miniapp_runtime_lock();')[1:]:
        locked=section.split('miniapp_runtime_unlock();',1)[0]
        assert all(operation not in locked for operation in
            ('pet_file_exact(','pet_storage_physical_','fopen(','fread(','fclose(','malloc(',
             'background_','pet_component_program_','write_','persist_'))
    assert '#if PET_P4_STORAGE_PRESERVE_UNCHANGED\nstatic bool unchanged_storage_current' in code
    for name in ('save_component_program','restore_component_runtime','pet_p4_miniapp_program_status'):
        body=code.split(name+'(',1)[1].split('\n}',1)[0]
        assert 'miniapp_runtime_lock();' in body and 'miniapp_runtime_unlock();' in body
    frame = code.split('bool pet_p4_miniapp_render_snapshot(',1)[1].split('bool pet_p4_miniapp_side_snapshot(',1)[0]
    assert frame.count('miniapp_runtime_lock();')==1 and frame.count('miniapp_runtime_unlock();')==1
    assert frame.index('surface_images_locked(')<frame.index('miniapp_runtime_unlock();')<frame.index('pet_p4_widget_data_view(')
    assert 'portENTER_CRITICAL(&g_clock_lock)' in code
    init = code[code.index('esp_err_t pet_p4_miniapp_init(void)'):]
    assert init.index('miniapp_runtime_lock_init()') < init.index('pet_p4_media_init()')
    source = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct {pthread_mutex_t mutex;} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define configASSERT(condition) ((void)0)
static _Thread_local bool in_isr,in_critical;
static bool fail_init,fail_take,fail_give;
static atomic_uint waiting,progress,reader_done;
static atomic_bool stop_ticker;
static unsigned creates;
static int xPortInIsrContext(void){return in_isr;}
static bool xPortCanYield(void){return !in_isr&&!in_critical;}
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *s){
 if(fail_init)return NULL;
 pthread_mutexattr_t attr;assert(!pthread_mutexattr_init(&attr));
 assert(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
 assert(!pthread_mutex_init(&s->mutex,&attr));assert(!pthread_mutexattr_destroy(&attr));creates++;return s;
}
static int xSemaphoreTakeRecursive(SemaphoreHandle_t s,unsigned timeout){
 assert(timeout==portMAX_DELAY&&!in_isr&&!in_critical);if(fail_take)return 0;
 atomic_fetch_add(&waiting,1);int result=pthread_mutex_lock(&s->mutex);atomic_fetch_sub(&waiting,1);return result==0;
}
static int xSemaphoreGiveRecursive(SemaphoreHandle_t s){if(fail_give)return 0;return pthread_mutex_unlock(&s->mutex)==0;}
static _Noreturn void rejected(void){_Exit(79);}
#define abort rejected
''' + lock + r'''
#undef abort
#define PET_P4_MINIAPP_SPRITE_MAX 4
#define PET_P4_MINIAPP_SPRITE_ID_MAX 16
#define PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX (128*1024)
''' + sprites + r'''
/* The actual sprite pack layout is used. Extra bounded runtime bytes ensure
 * the copied object spans the same large-data class as the target runtime. */
typedef struct {pet_p4_miniapp_sprite_pack_t sprites;uint8_t other[80*1024];} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
static uint32_t g_image_revision=1;
static size_t g_catalog_selected;
static struct {bool published;} g_activation;
static void publish_next(miniapp_runtime_t *next,size_t index,uint64_t token){
''' + publish + r'''
}
''' + getters + r'''
static void pause_briefly(void){struct timespec delay={0,1000000};nanosleep(&delay,NULL);}
static void wait_at_least(atomic_uint *value,unsigned minimum){
 for(unsigned i=0;i<3000;i++){if(atomic_load(value)>=minimum)return;pause_briefly();}
 assert(!"thread did not progress within bounded wait");
}
static void check(const pet_p4_miniapp_sprite_pack_t *p){
 assert(p->count==1&&p->data_length==PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX);
 uint8_t value=(uint8_t)p->items[0].frames;assert(value==1||value==2);
 for(size_t i=0;i<p->data_length;i++)assert(p->pixels[i]==value);
 assert(p->items[0].frame_width==value*8&&p->items[0].data_length==p->data_length);
}
static void prepare(miniapp_runtime_t *r,uint8_t value){
 memset(r,value,sizeof(*r));r->sprites.count=1;r->sprites.data_length=PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX;
 r->sprites.items[0].frames=value;r->sprites.items[0].frame_width=value*8;
 r->sprites.items[0].data_length=r->sprites.data_length;
}
static void *ticker(void *arg){(void)arg;while(!atomic_load(&stop_ticker)){atomic_fetch_add(&progress,1);pause_briefly();}return NULL;}
static void *one_reader(void *arg){(void)arg;pet_p4_miniapp_sprite_pack_t *p=malloc(sizeof(*p));assert(p);assert(pet_p4_miniapp_get_sprites(p));check(p);free(p);atomic_store(&reader_done,1);return NULL;}
static void *many_reads(void *arg){(void)arg;pet_p4_miniapp_sprite_pack_t *p=malloc(sizeof(*p));assert(p);uint32_t revision=0;
 for(unsigned i=0;i<300;i++){assert(pet_p4_miniapp_get_sprites(p));check(p);if(pet_p4_miniapp_get_surface_images(p,&revision))check(p);}
 free(p);return NULL;
}
int main(int argc,char **argv){
 if(argc>1){
  if(!strcmp(argv[1],"uninitialized")){miniapp_runtime_lock();return 1;}
  if(!strcmp(argv[1],"init_failure")){fail_init=true;assert(!miniapp_runtime_lock_init());miniapp_runtime_lock();return 1;}
  if(!strcmp(argv[1],"init_isr")){in_isr=true;assert(!miniapp_runtime_lock_init());return 0;}
  if(!strcmp(argv[1],"init_critical")){in_critical=true;assert(!miniapp_runtime_lock_init());return 0;}
  assert(miniapp_runtime_lock_init());
  if(!strcmp(argv[1],"isr"))in_isr=true;
  if(!strcmp(argv[1],"critical"))in_critical=true;
  if(!strcmp(argv[1],"take_failure"))fail_take=true;
  if(!strcmp(argv[1],"foreign_unlock")){miniapp_runtime_unlock();return 1;}
  miniapp_runtime_lock();if(!strcmp(argv[1],"give_failure"))fail_give=true;
  miniapp_runtime_unlock();return 1;
 }
 assert(miniapp_runtime_lock_init()&&miniapp_runtime_lock_init()&&creates==1);
 miniapp_runtime_lock();miniapp_runtime_lock();miniapp_runtime_unlock();miniapp_runtime_unlock();
 miniapp_runtime_t *next=malloc(sizeof(*next));assert(next);prepare(next,1);publish_next(next,4,1);assert(g_activation.published&&g_catalog_selected==4);
 pthread_t independent,blocked;assert(!pthread_create(&independent,NULL,ticker,NULL));
 miniapp_runtime_lock();assert(!pthread_create(&blocked,NULL,one_reader,NULL));wait_at_least(&waiting,1);
 unsigned before=atomic_load(&progress);wait_at_least(&progress,before+3);
 assert(!atomic_load(&reader_done));miniapp_runtime_unlock();assert(!pthread_join(blocked,NULL));assert(atomic_load(&reader_done));
 pthread_t readers[2];for(unsigned i=0;i<2;i++)assert(!pthread_create(&readers[i],NULL,many_reads,NULL));
 for(unsigned i=0;i<500;i++){prepare(next,(i&1)+1);publish_next(next,4,1);}
 for(unsigned i=0;i<2;i++)assert(!pthread_join(readers[i],NULL));
 atomic_store(&stop_ticker,true);assert(!pthread_join(independent,NULL));free(next);
 assert(!waiting&&progress>3);return 0;
}
'''
    src = tmp_path / 'runtime-lock.c'
    src.write_text(source)
    exe = tmp_path / 'runtime-lock'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', '-pthread', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
    for case in ('uninitialized', 'init_failure', 'isr', 'critical', 'take_failure', 'give_failure', 'foreign_unlock'):
        result = subprocess.run([str(exe), case], timeout=5)
        assert result.returncode == 79, case
    for case in ('init_isr', 'init_critical'):
        subprocess.run([str(exe), case], check=True, timeout=5)
