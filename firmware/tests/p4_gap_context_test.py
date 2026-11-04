"""R133 gap-peak context: main-loop wait/held/component maxima retained for the lifetime gap peak."""
from pathlib import Path
import configparser
import subprocess

ROOT=Path(__file__).resolve().parents[1]
FLAGS=['-DPET_P4_GAP_PEAK_CONTEXT=1','-DPET_P4_FRAME_IO_OVERLAP=1','-DPET_P4_RETAIN_PEAK_PHASES=1','-DPET_P4_APP_PHASE_TRACE=1','-DPET_P4_APP_NODE_TRACE=1']


def test_gap_peak_context_follows_the_lifetime_gap_maximum_only(tmp_path):
    code=r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_render_lifetime.h"
int main(void){
 pet_render_lifetime_t life;memset(&life,0,sizeof(life));
 pet_render_gap_context_t c={.frame=10,.wait_max_us=1000,.held_max_us=2000,.miniapp_max_us=500,.held_total_us=9000,.iterations=40,.lane0_phase=0x10203,.lane1_phase=0};
 pet_render_gap_context_record(&life,45000,&c);pet_render_lifetime_record(&life,12000,45000,10,2,0,12000,66667,100000);
 assert(life.gap_peak.gap_us==45000&&life.gap_peak.frame==10&&life.gap_peak.wait_max_us==1000&&life.gap_peak.lane0_phase==0x10203);
 c.frame=11;c.wait_max_us=30000;c.held_max_us=100;
 pet_render_gap_context_record(&life,40000,&c);pet_render_lifetime_record(&life,12000,40000,11,2,0,12000,66667,100000);
 assert(life.gap_peak.frame==10&&life.gap_peak.wait_max_us==1000);
 pet_render_gap_context_record(&life,83898,&c);pet_render_lifetime_record(&life,12000,83898,11,2,0,12000,66667,100000);
 assert(life.gap_peak.gap_us==83898&&life.gap_peak.frame==11&&life.gap_peak.wait_max_us==30000&&life.max_gap_us==83898);
 return 0;
}
'''
    src=tmp_path/'gap.c';exe=src.with_suffix('');src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*FLAGS,'-I',str(ROOT/'main'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=60)


def test_main_loop_instrumentation_is_flag_gated_and_env_adds_nothing_else(tmp_path):
    main=(ROOT/'main/pet_p4_main.c').read_text();renderer=(ROOT/'main/pet_p4_renderer.c').read_text()
    loop=main[main.index('  while (true) {\n    bool render_frame = false;'):main.index('pet_p4_renderer_note_loop(')]
    assert loop.count('xSemaphoreTake(g_state_mutex, portMAX_DELAY);')==1 and loop.count('xSemaphoreGive(g_state_mutex);')==1
    assert loop.index('loop_wait_started_us = pet_p4_gap_clock_us();')<loop.index('xSemaphoreTake(g_state_mutex, portMAX_DELAY);')<loop.index('loop_held_started_us = pet_p4_gap_clock_us();')
    assert loop.index('loop_miniapp_started_us = pet_p4_gap_clock_us();')<loop.index('pet_p4_miniapp_process(now_ms')<loop.index('loop_miniapp_us = (uint32_t)')
    assert loop.count('esp_timer_get_time()')==1
    assert loop.index('xSemaphoreGive(g_state_mutex);')<main.index('pet_p4_renderer_note_loop(loop_wait_us,')<main.index('pet_p4_renderer_render(g_render_state, &render_view, now_ms)')
    assert main.count('#if PET_P4_GAP_PEAK_CONTEXT')==6
    assert renderer.count('pet_render_gap_context_record(&g_metrics.lifetime,g_render_trace.gap_us,&context);')==1
    assert renderer.index('pet_render_gap_context_record(')<renderer.index('pet_render_lifetime_record(&g_metrics.lifetime,total_us,g_render_trace.gap_us,')
    assert renderer.count('memset(&g_loop_context,0,sizeof(g_loop_context));')==1
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_gap_context_parent':
            assert config[section]['extends']=='env:esp32_p4_storage_core_parent' and config[section]['custom_p4_project_version']=='0.7.103-p4-r133-gapctx'
            assert 'PET_P4_GAP_PEAK_CONTEXT=1' in flags and not any(v in flags for v in ('TASK_PRIORITY','F_CPU','BUDGET_US','WATCHDOG','AFFINITY'))
        else:assert 'PET_P4_GAP_PEAK_CONTEXT=1' not in flags
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_render_lifetime.h"\nint main(void){return 0;}\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_GAP_PEAK_CONTEXT=1','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'Gap peak context requires' in result.stderr
    assert subprocess.run(['cc','-std=c11','-fsyntax-only','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True).returncode==0
