"""Real v2 publication; deterministic revocation during the out-of-lock copy."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
FLAGS=['-DPET_P4_PROGRAM_V2=1','-DPET_COMPONENT_PROGRAM_ENABLED=1',
       '-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1','-DPET_P4_TEXT_TABLE=1','-DPET_P4_APP_DIRTY_CACHE=1']

def test_double_buffer_publication_and_snapshot(tmp_path):
    stub=tmp_path/'freertos/FreeRTOS.h';stub.parent.mkdir()
    stub.write_text('#pragma once\n#include <pthread.h>\nvoid test_mux_enter(pthread_mutex_t*);\nvoid test_mux_exit(pthread_mutex_t*);\ntypedef pthread_mutex_t portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER\n#define portENTER_CRITICAL(m) test_mux_enter(m)\n#define portEXIT_CRITICAL(m) test_mux_exit(m)\n')
    binary=tmp_path/'publication-v2'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-fsanitize=address,undefined','-pthread',*FLAGS,'-I',str(tmp_path),'-I',str(ROOT/'main'),
                    *[str(ROOT/'main'/f'pet_p4_program_{n}.c') for n in ('core','image','runtime')],
                    str(ROOT/'main/pet_p4_resources.c'),str(ROOT/'tests/p4_program_publication_v2_test.c'),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True,check=True)
    assert '512 gate unchanged' in result.stdout and not result.stderr
