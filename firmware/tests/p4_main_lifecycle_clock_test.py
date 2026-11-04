"""Actual main-loop sampling order against the real bounded timer core.
Simulate a new serialized lifecycle admitted while main waits for its mutex.
No RTOS/device/audio or user files. Actual clock regression still cancels.
"""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_main_samples_clock_after_owner_lock_not_before_new_timer_admission(tmp_path):
    source=(ROOT/'main/pet_p4_main.c').read_text()
    prefix=source.split('  pet_fs_activity_enable_observation();',1)[1].split('    pet_p4_miniapp_process(',1)[0]
    # Startup announcements may sample their own scheduling timestamp before
    # entering the loop. Only the ACTUAL serialized model turn is under test;
    # retain its one post-lock sample and the same real timer/regression cases.
    prefix='  while (true) {'+prefix.split('  while (true) {',1)[1]
    assert prefix.count('esp_timer_get_time()')==1
    assert prefix.index('xSemaphoreTake(g_state_mutex, portMAX_DELAY)')<prefix.index('unsigned long long now_ms')<prefix.index('pet_p4_diagnostics_process(now_ms, &g_state)')
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "pet_p4_program_timers.h"
#define portMAX_DELAY 1
typedef struct {unsigned unused;} pet_p4_view_model_t;
static uint64_t clock_ms=100,observed;static bool held;static unsigned processes;
static int g_state_mutex=1,g_state;
static pet_program_timers_t timer;
static int64_t esp_timer_get_time(void){return (int64_t)(clock_ms*1000);}
static void xSemaphoreTake(int mutex,int timeout){
 assert(mutex==1&&timeout==1&&!held);held=true;
 // Represents a different lifecycle command completing while main waited.
 clock_ms=200;pet_program_timer_spec_t spec={.id=1,.delay_ms=10,.interval_ms=100};
 assert(pet_program_timers_start(&timer,1,clock_ms,&spec,1)==PET_TIMERS_OK);
}
static void observe(uint64_t now){assert(held&&now==200);processes++;}
static void pet_p4_diagnostics_process(uint64_t now,int *s){assert(s==&g_state);observe(now);}
static void pet_p4_ota_process(uint64_t now){observe(now);}
static void pet_p4_asset_transfer_process(int *s,uint64_t now){assert(s==&g_state);observe(now);}
static void pet_p4_native_usb_process(uint64_t now){observe(now);}
static pet_program_runtime_result_t enqueue(void *ctx,uint64_t generation,uint64_t sequence,const pet_program_event_t *event){
 (void)ctx;(void)generation;(void)sequence;(void)event;assert(!"not due yet");return PET_RUNTIME_OK;
}
static void one_actual_turn(void){
'''+prefix+r'''
    observed=now_ms;
    pet_program_timer_diagnostic_t d={0};
    assert(pet_program_timers_tick(&timer,1,now_ms,1,enqueue,NULL,&d)==PET_TIMERS_IDLE);
    assert(!timer.cancelled&&!d.offered);
    (void)render_frame;(void)render_transfer_frame;(void)firmware_transfer_active;(void)keep_lcd_awake;(void)render_view;
    held=false;break;
  }
}
int main(void){
 one_actual_turn();assert(observed==200&&processes==4&&!held&&!timer.cancelled);
 pet_program_timer_diagnostic_t d={0};
 assert(pet_program_timers_tick(&timer,1,199,2,enqueue,NULL,&d)==PET_TIMERS_CLOCK_ERROR&&timer.cancelled);
 return 0;
}
'''
    path=tmp_path/'clock.c';path.write_text(code);binary=tmp_path/'clock'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-I',str(ROOT/'main'),str(path),str(ROOT/'main/pet_p4_program_timers.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
