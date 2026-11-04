"""Actual cooperative gate: cancellation/reentry/duplex policy, no device IO."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_actual_native_window_is_binary_owner_checked_and_never_blocks_compute(tmp_path):
    headers=tmp_path/'freertos';headers.mkdir()
    (tmp_path/'esp_attr.h').write_text('#define DRAM_ATTR\n')
    (headers/'FreeRTOS.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
typedef uint32_t TickType_t;
typedef struct{pthread_mutex_t mutex;pthread_cond_t changed;unsigned available;}StaticSemaphore_t;
typedef StaticSemaphore_t* SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
''')
    (headers/'semphr.h').write_text('''#pragma once
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t*);
int xSemaphoreGive(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t,TickType_t);
''')
    (headers/'task.h').write_text('''#pragma once
typedef void* TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
''')
    code=r'''
#include <assert.h>
#include <stdatomic.h>
#include <sched.h>
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pet_p4_native_work_window.c"
static _Thread_local void *current_task;
static _Atomic unsigned entered,waiting,finish;
TaskHandle_t xTaskGetCurrentTaskHandle(void){return current_task;}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t*s){
 assert(!pthread_mutex_init(&s->mutex,NULL));assert(!pthread_cond_init(&s->changed,NULL));s->available=0;return s;
}
int xSemaphoreGive(SemaphoreHandle_t s){
 assert(!pthread_mutex_lock(&s->mutex));int ok=!s->available;
 if(ok){s->available=1;assert(!pthread_cond_signal(&s->changed));}
 assert(!pthread_mutex_unlock(&s->mutex));return ok;
}
int xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks){
 assert(ticks==0||ticks==portMAX_DELAY);assert(!pthread_mutex_lock(&s->mutex));
 while(!s->available){if(!ticks){pthread_mutex_unlock(&s->mutex);return 0;}
   atomic_store(&waiting,1);assert(!pthread_cond_wait(&s->changed,&s->mutex));}
 s->available=0;assert(!pthread_mutex_unlock(&s->mutex));return 1;
}
static void* control(void *unused){
 (void)unused;current_task=(void*)2;assert(pet_native_work_window_control_begin());atomic_store(&entered,1);
 assert(!pet_native_work_window_try_begin()&&!pet_native_work_window_control_begin());
 while(!atomic_load(&finish))sched_yield();assert(pet_native_work_window_end());return NULL;
}
int main(void){
 current_task=(void*)1;assert(!pet_native_work_window_try_begin()&&!pet_native_work_window_end());
 assert(pet_native_work_window_init()&&!pet_native_work_window_init());
 current_task=NULL;assert(!pet_native_work_window_try_begin()&&!pet_native_work_window_control_begin());
 current_task=(void*)1;assert(pet_native_work_window_try_begin());
 assert(!pet_native_work_window_try_begin()&&!pet_native_work_window_control_begin());
 current_task=(void*)3;assert(!pet_native_work_window_try_begin()&&!pet_native_work_window_end());
 current_task=(void*)1;pthread_t peer;assert(!pthread_create(&peer,NULL,control,NULL));
 while(!atomic_load(&waiting))sched_yield();assert(!atomic_load(&entered));assert(pet_native_work_window_end());
 while(!atomic_load(&entered))sched_yield();assert(!pet_native_work_window_try_begin()&&!pet_native_work_window_end());
 atomic_store(&finish,1);assert(!pthread_join(peer,NULL));
 assert(pet_native_work_window_try_begin()&&pet_native_work_window_end()&&!pet_native_work_window_end());
 return 0;
}
'''
    source=tmp_path/'window.c';source.write_text(code);binary=tmp_path/'window'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pthread',
        '-fsanitize=address,undefined','-DPET_P4_NATIVE_WORK_WINDOW=1','-I',str(tmp_path),'-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_window_default_object_empty_and_real_audio_path_bypasses_it(tmp_path):
    from p4_control_integrity_test import _c_function as function
    main=(ROOT/'main/pet_p4_main.c').read_text()
    body=function(main,'transport_send_line_impl')
    assert body.index('if(drain){work_window=')<body.index('xSemaphoreTake(g_transport_mutex')
    assert body.index('xSemaphoreGive(g_transport_mutex)')<body.index('pet_native_work_window_end()')
    assert 'transport_send_line_impl(line, ctx, false)' in function(main,'transport_send_audio_line')
    component=function((ROOT/'main/pet_p4_miniapp.c').read_text(),'pet_p4_miniapp_process')
    assert component.index('pet_p4_media_tick(')<component.index('pet_native_work_window_try_begin(')
    assert component.index('pet_native_work_window_end(')<component.index('pet_side_process(')
    obj=tmp_path/'closed.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-c',str(ROOT/'main/pet_p4_native_work_window.c'),'-o',str(obj)],check=True)
    symbols=subprocess.run(['nm','-g',str(obj)],capture_output=True,text=True,check=True).stdout
    assert 'pet_native_work_window_' not in symbols
