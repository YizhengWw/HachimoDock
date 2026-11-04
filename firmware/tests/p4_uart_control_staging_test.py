"""Deferred host behavior checks: actual staged queue/pacing/prefix, no device.

Authored during hardware observation; do not run until the parent releases it.
No production modules/scripts are executed, and no SDK/native build is invoked.
"""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CC = os.environ.get('CC', 'cc')
COMMON = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
          '-fsanitize=address,undefined', '-fno-omit-frame-pointer']


def _run(program, tmp_path, name, flags):
    source = tmp_path / (name + '.c')
    binary = tmp_path / name
    source.write_text(program)
    subprocess.run([CC, *COMMON, *flags, '-I', str(ROOT / 'main'),
                    '-I', str(tmp_path), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_copy_boundaries_guards_and_every_source_alignment(tmp_path):
    program = r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_uart_control_staging.h"
int main(void) {
  _Alignas(64) unsigned char input[384], stage[320];
  for (size_t i=0;i<sizeof(input);i++) input[i]=(unsigned char)(i*37U+11U);
  const size_t sizes[]={0,1,21,43,63,64,65,235,255,256};
  for(size_t offset=0;offset<64;offset++) for(size_t j=0;j<sizeof(sizes)/sizeof(*sizes);j++) {
    size_t n=sizes[j]; memset(stage,0xa5,sizeof(stage));
    assert(pet_uart_control_stage_copy(stage,(const char*)input+offset,n)==(const char*)stage);
    assert(!memcmp(stage,input+offset,n));
    for(size_t i=n;i<sizeof(stage);i++) assert(stage[i]==0xa5);
  }
  memset(stage,0xa5,sizeof(stage));
  assert(!pet_uart_control_stage_copy(stage,(const char*)input,257));
  assert(!pet_uart_control_stage_copy(stage+1,(const char*)input,21));
  assert(!pet_uart_control_stage_copy(NULL,(const char*)input,21));
  assert(!pet_uart_control_stage_copy(stage,NULL,1));
  assert(pet_uart_control_stage_copy(stage,NULL,0)==(const char*)stage);
  for(size_t i=0;i<sizeof(stage);i++) assert(stage[i]==0xa5);
  return 0;
}
'''
    for burst in (64, 256):
        _run(program, tmp_path, f'copy-{burst}',
             ['-DPET_P4_UART_PACED_REFERENCE=1', f'-DPET_P4_UART_PACED_BYTES={burst}'])


def test_actual_control_queue_pacing_prefix_partials_and_unchanged_routes(tmp_path):
    source = (ROOT / 'main/pet_p4_main.c').read_text()
    # Extract the actual queue + staging + paced writer + prefix adapter.
    # Behavior is asserted on real function bodies, not inferred from regexes.
    production = 'static void uart_queue_all(' + source.split(
        'static void uart_queue_all(', 1)[1].split('static int transport_log_vprintf(', 1)[0]
    (tmp_path / 'production_control_queue.inc').write_text(production)
    program = (ROOT / 'tests/p4_uart_control_staging_fixture.c').read_text()
    for reference, burst, public in ((0, 64, 0), (1, 64, 0), (1, 256, 0), (0, 64, 1)):
        for integrity in (0, 1):
            _run(program, tmp_path, f'queue-{reference}-{burst}-{public}-{integrity}', [
                f'-DPET_P4_UART_PACED_REFERENCE={reference}',
                f'-DPET_P4_UART_PACED_BYTES={burst}',
                f'-DPET_P4_UART_CONTROL_PACING={public}',
                f'-DPET_P4_SERIAL_CONTROL_INTEGRITY={integrity}'])
