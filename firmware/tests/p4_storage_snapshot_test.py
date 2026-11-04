"""Actual read-only telemetry/cache boundaries. No device, sound or resets."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_actual_snapshot_retains_valid_statistics_on_failure_clock_and_bounds(tmp_path):
    source=tmp_path/'snapshot.c';binary=tmp_path/'snapshot'
    source.write_text(r'''
#include "pet_p4_storage_snapshot.h"
#include <assert.h>
#include <string.h>
int main(void){
 pet_storage_snapshot_t s={0};
 assert(!pet_storage_snapshot_publish(NULL,100,50,1,0));
 assert(!pet_storage_snapshot_observe(s,10).value.available);
 assert(!pet_storage_snapshot_publish(&s,0,0,10,0)&&!s.available&&s.refresh_failed);
 assert(pet_storage_snapshot_publish(&s,1000,600,100,0));
 assert(s.available&&!s.refresh_failed&&s.used==600);
 for(unsigned i=0;i<4;i++){
  pet_storage_snapshot_t old=s;
  assert(!pet_storage_snapshot_publish(&s,i==0?UINT64_MAX:1000,i==1?1001:0,i==2?99:100,i==3?7:0));
  assert(s.total==old.total&&s.used==old.used&&s.sampled_ms==old.sampled_ms&&s.available&&s.refresh_failed);
  assert(pet_storage_snapshot_observe(s,101).stale);
 }
 assert(pet_storage_snapshot_publish(&s,1000,600,100,0));
 pet_storage_snapshot_view_t v=pet_storage_snapshot_observe(s,99);assert(v.stale&&v.clock_invalid&&!v.age_ms);
 v=pet_storage_snapshot_observe(s,3100);assert(!v.stale&&v.age_ms==3000);
 v=pet_storage_snapshot_observe(s,3101);assert(v.stale&&!v.clock_invalid&&v.age_ms==3001);
 assert(pet_storage_snapshot_publish(&s,UINT32_MAX,UINT32_MAX,UINT64_MAX,0));
 v=pet_storage_snapshot_observe(s,UINT64_MAX);assert(!v.stale&&!v.age_ms);
 assert(!pet_storage_snapshot_publish(&s,100,1,0,0)&&s.used==UINT32_MAX);
 return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-fstrict-aliasing',
        '-fsanitize=address,undefined','-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_actual_query_cache_branch_cannot_call_filesystem_and_old_fields_retained(tmp_path):
    text=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    start=text.index('  pet_storage_snapshot_t copied;');end=text.index('  add_request_id(payload, request);',start)
    branch=text[start:end]
    # Preprocess the ACTUAL conditional source. Keep declaration of the
    # forbidden original FS provider only as an unresolved symbol: if cached
    # code calls it the native link fails, rather than returning fake success.
    source=tmp_path/'query.c';binary=tmp_path/'query'
    source.write_text(r'''
#include "pet_p4_storage_snapshot.h"
#include <assert.h>
#include <stddef.h>
static int held,g_storage_snapshot_mux;
static pet_storage_snapshot_t g_storage_snapshot={.sampled_ms=10,.total=100,.used=70,.available=true};
#define portENTER_CRITICAL(p) do{assert((p)==&g_storage_snapshot_mux&&!held);held=1;}while(0)
#define portEXIT_CRITICAL(p) do{assert((p)==&g_storage_snapshot_mux&&held);held=0;}while(0)
static int64_t esp_timer_get_time(void){assert(!held);return 12000;}
extern int esp_spiffs_info(const char*,size_t*,size_t*);
int main(void){size_t storage_total=0,storage_used=0;
#if PET_P4_DIAGNOSTIC_STORAGE_CACHED
'''+branch+r'''
 assert(!held&&storage_total==100&&storage_used==70&&storage_view.age_ms==2&&!storage_view.stale);return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DPET_P4_DIAGNOSTIC_STORAGE_CACHED=1',
        '-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
    for key in ('totalBytes','usedBytes','statisticsAvailable','statisticsStale','statisticsAgeMs','statisticsRefreshError'):
        assert f'"{key}"' in text

def test_refresh_and_fixed_housekeeping_have_no_model_store_lock_or_new_task():
    from p4_control_integrity_test import _c_function
    diagnostics=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    refresh=_c_function(diagnostics,'pet_p4_diagnostics_storage_refresh')
    assert refresh.index('esp_spiffs_info(')<refresh.index('portENTER_CRITICAL(')
    for bad in ('xSemaphoreTake','g_state_mutex','g_diagnostics_mutex','xTaskCreate','vTaskDelay','fsync(','fopen(','calloc(','malloc('):
        assert bad not in refresh
    for flag in ('pet_p4_audio_active()','pet_p4_audio_playback_active()','pet_p4_audio_conversation_active()','pet_p4_audio_music_active()'):
        assert flag in refresh
    store=(ROOT/'main/pet_p4_app_store.c').read_text()
    worker=_c_function(store,'save_worker')
    assert worker.index('vTaskDelay(pdMS_TO_TICKS(1000))')<worker.index('flush_one();')<worker.index('pet_app_store_housekeeping();')
    assert 'xSemaphoreTake' not in worker and 'portENTER_CRITICAL' not in worker
    assert '__attribute__((weak)) void pet_app_store_housekeeping(void){}' in store
    main=(ROOT/'main/pet_p4_main.c').read_text()
    assert 'void pet_app_store_housekeeping(void){pet_p4_diagnostics_storage_refresh();}' in main

def test_actual_refresh_provider_runs_outside_lock_and_defers_during_audio(tmp_path):
    from p4_control_integrity_test import _c_function
    body=_c_function((ROOT/'main/pet_p4_diagnostics.c').read_text(),'pet_p4_diagnostics_storage_refresh')
    source=tmp_path/'refresh.c';binary=tmp_path/'refresh'
    source.write_text(r'''
#include "pet_p4_storage_snapshot.h"
#include <assert.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_ERR_INVALID_STATE 12
static int held,g_storage_snapshot_mux,calls,error;
static bool audio;
static int64_t tick=100000;
static pet_storage_snapshot_t g_storage_snapshot;
#define portENTER_CRITICAL(p) do{assert((p)==&g_storage_snapshot_mux&&!held);held=1;}while(0)
#define portEXIT_CRITICAL(p) do{assert((p)==&g_storage_snapshot_mux&&held);held=0;}while(0)
static bool pet_p4_audio_active(void){return audio;}
static bool pet_p4_audio_playback_active(void){return false;}
static bool pet_p4_audio_conversation_active(void){return false;}
static bool pet_p4_audio_music_active(void){return false;}
static int64_t esp_timer_get_time(void){assert(!held);return tick;}
static esp_err_t esp_spiffs_info(const char *name,size_t *total,size_t *used){assert(!held&&name);calls++;*total=1000;*used=600;return error;}
'''+body+r'''
int main(void){
 pet_p4_diagnostics_storage_refresh();assert(calls==1&&!held&&g_storage_snapshot.available&&g_storage_snapshot.used==600);
 audio=true;pet_p4_diagnostics_storage_refresh();assert(calls==1&&!held);
 audio=false;tick+=1000;error=7;pet_p4_diagnostics_storage_refresh();assert(calls==2&&g_storage_snapshot.refresh_failed&&g_storage_snapshot.used==600&&g_storage_snapshot.sampled_ms==100);
 error=0;tick=-1;pet_p4_diagnostics_storage_refresh();assert(calls==3&&g_storage_snapshot.refresh_failed&&g_storage_snapshot.used==600);
 tick=102000;pet_p4_diagnostics_storage_refresh();assert(calls==4&&!g_storage_snapshot.refresh_failed&&g_storage_snapshot.sampled_ms==102&&!held);return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-DPET_P4_DIAGNOSTIC_STORAGE_CACHED=1','-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_actual_worker_calls_housekeeping_only_after_flush_and_default_off(tmp_path):
    from p4_control_integrity_test import _c_function
    body=_c_function((ROOT/'main/pet_p4_app_store.c').read_text(),'save_worker')
    source=tmp_path/'worker.c'
    source.write_text(r'''
#include <assert.h>
#include <setjmp.h>
static jmp_buf done;
static unsigned sleeps,flushed,maintained;
#define pdMS_TO_TICKS(n) (n)
static void vTaskDelay(unsigned ticks){assert(ticks==1000);if(++sleeps==2)longjmp(done,1);}
static void flush_one(void){assert(sleeps==1&&!maintained);flushed++;}
#if PET_P4_DIAGNOSTIC_STORAGE_CACHED
static void pet_app_store_housekeeping(void){assert(sleeps==1&&flushed==1&&!maintained);maintained++;}
#endif
'''+body+r'''
int main(void){if(!setjmp(done))save_worker(0);assert(sleeps==2&&flushed==1&&maintained==PET_P4_DIAGNOSTIC_STORAGE_CACHED);return 0;}
''')
    for flag in (0,1):
        binary=tmp_path/f'worker-{flag}'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',f'-DPET_P4_DIAGNOSTIC_STORAGE_CACHED={flag}',
            '-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

def test_cached_profile_is_private_default_off_and_adds_no_capability_grant():
    import configparser
    c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
    assert '#define PET_P4_DIAGNOSTIC_STORAGE_CACHED 0' in (ROOT/'main/pet_p4_storage_snapshot.h').read_text()
    for section in c.sections():
        flags=c[section].get('build_flags','')
        if section=='env:esp32_p4_storage_query_probe':
            assert c[section]['extends']=='env:esp32_p4_watchdog_phase_probe'
            assert flags.splitlines()==['','${env:esp32_p4_watchdog_phase_probe.build_flags}','-DPET_P4_DIAGNOSTIC_STORAGE_CACHED=1']
        else:assert 'PET_P4_DIAGNOSTIC_STORAGE_CACHED=1' not in flags
