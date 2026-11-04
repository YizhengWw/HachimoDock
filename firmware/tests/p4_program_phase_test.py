"""Numeric observations preserve overflow/invalid clocks without grants or IO."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_program_phase_saturates_and_retains_invalid_clock(tmp_path):
    source=tmp_path/'phase.c';binary=tmp_path/'phase'
    source.write_text(r'''
#include <assert.h>
#include "pet_p4_program_phase.h"
int main(void){
 pet_program_phase_t p={0};
 assert(sizeof(p)<=40);
 pet_program_phase_add(&p,&p.scheduler_us,100,350);assert(p.scheduler_us==250);
 pet_program_phase_add(&p,&p.scheduler_us,INT64_C(1),INT64_MAX);assert(p.scheduler_us==UINT32_MAX);
 pet_program_phase_add(&p,&p.scheduler_us,1,2);assert(p.scheduler_us==UINT32_MAX);
 pet_program_phase_add(&p,&p.collect_us,-1,10);assert(p.invalid_clock==1&&!p.collect_us);
 pet_program_phase_add(&p,&p.collect_us,10,9);assert(p.invalid_clock==2&&!p.collect_us);
 p.invalid_clock=UINT8_MAX;pet_program_phase_add(&p,&p.capture_us,-1,0);
 assert(p.invalid_clock==UINT8_MAX&&!p.capture_us);return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
