"""Finite actual native sampler with fake clocks, not a P4 timing claim."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_actual_sampler_is_finite_once_validates_clock_and_rejection(tmp_path):
    (tmp_path/'esp_private').mkdir();(tmp_path/'freertos').mkdir()
    (tmp_path/'esp_attr.h').write_text('#define DRAM_ATTR\n')
    (tmp_path/'esp_cpu.h').write_text('#include <stdint.h>\nuint32_t esp_cpu_get_cycle_count(void);\n')
    (tmp_path/'esp_timer.h').write_text('#include <stdint.h>\nint64_t esp_timer_get_time(void);\n')
    (tmp_path/'esp_private/esp_clk.h').write_text('#include <stdint.h>\nuint32_t esp_clk_cpu_freq(void);\n')
    (tmp_path/'freertos/FreeRTOS.h').write_text('#define pdMS_TO_TICKS(ms) (ms)\n')
    (tmp_path/'freertos/task.h').write_text('void vTaskDelay(unsigned ticks);\n')
    source=tmp_path/'cost.c';binary=tmp_path/'cost'
    source.write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include "pet_p4_reset_cost.c"
static unsigned mode,enters,leaves,yields,clock_calls,timer_calls;
static uint32_t cycles;static int64_t stamp=100;
uint32_t esp_cpu_get_cycle_count(void){return cycles+=10;}
int64_t esp_timer_get_time(void){timer_calls++;if(mode==2&&timer_calls==2)return -1;return ++stamp;}
uint32_t esp_clk_cpu_freq(void){clock_calls++;return mode==3&&clock_calls>1?200000000:360000000;}
void vTaskDelay(unsigned ticks){assert(ticks==1&&enters%4==0&&enters<32);yields++;}
pet_reset_phase_scope_t pet_reset_phase_enter(pet_reset_phase_t phase){assert(phase==PET_PHASE_BOOT);enters++;return (pet_reset_phase_scope_t){.active=mode!=1};}
void pet_reset_phase_leave(pet_reset_phase_scope_t *scope){assert(scope->active);leaves++;if(mode!=5)scope->active=false;}
int main(int argc,char **argv){assert(argc==2);mode=(unsigned)atoi(argv[1]);if(mode==4)cycles=UINT32_MAX-2;
 pet_reset_cost_t report={0};assert(!pet_reset_cost_observe(&report));
 pet_reset_cost_boot();assert(pet_reset_cost_observe(&report));assert(!pet_reset_cost_observe(NULL));
 if(mode==0||mode==4){assert(report.valid&&report.samples==32&&report.clock_hz==360000000);
  assert(report.total_cycles==320&&report.min_cycles==10&&report.max_cycles==10&&yields==7&&leaves==32);
 }else assert(!report.valid);
 if(mode==1)assert(enters==1&&leaves==0&&!report.samples&&!yields);
 if(mode==2||mode==5)assert(enters==1&&leaves==1&&!report.samples&&!yields);
 unsigned before=enters;pet_reset_cost_boot();assert(enters==before);return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-DPET_P4_RESET_PHASE_TRACE=1','-DPET_P4_RESET_PHASE_BENCHMARK=1','-I',str(tmp_path),
      '-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    for mode in range(6):subprocess.run([str(binary),str(mode)],check=True,timeout=10)

def test_sampler_default_off_no_imports_or_device_side_effects(tmp_path):
    obj=tmp_path/'off.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),
      '-c',str(ROOT/'main/pet_p4_reset_cost.c'),'-o',str(obj)],check=True)
    assert not subprocess.run(['nm','-u',str(obj)],check=True,capture_output=True,text=True).stdout.strip()
    code=(ROOT/'main/pet_p4_reset_cost.c').read_text()
    for effect in ('malloc(','printf(','fopen(','nvs_','wdt_','audio_','REG_WRITE','vTaskPrioritySet','esp_pm_'):
        assert effect not in code
    assert 'i<32' in code and 'i+1<32' in code
