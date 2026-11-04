"""Versioned wide program snapshots through the real codecs and runtime, offline only."""
from pathlib import Path
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ['-DPET_P4_PROGRAM_V2=1', '-DPET_COMPONENT_PROGRAM_ENABLED=1',
         '-DPET_P4_TEXT_TABLE=1', '-DPET_P4_APP_DIRTY_CACHE=1']


def test_versioned_wide_snapshots_and_atomic_restore(tmp_path):
    binary = tmp_path / 'wide-store'
    persistence=tmp_path/'persistence.c'
    persistence.write_text(r'''
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
static bool oom;static unsigned live_alloc;
void persistence_alloc_fail(bool fail){oom=fail;}
unsigned persistence_live_alloc(void){return live_alloc;}
static void *test_alloc(size_t n){if(oom)return NULL;void *p=malloc(n);if(p)live_alloc++;return p;}
static void test_free(void*p){if(p){assert(live_alloc);live_alloc--;}free(p);}
#define malloc test_alloc
#define free test_free
#include "pet_p4_program_persistence.c"
''')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    *FLAGS, '-I', str(ROOT / 'main'),
                    *[str(ROOT / 'main' / f'pet_p4_program_{n}.c') for n in
                      ('core', 'image', 'runtime', 'store')],str(persistence),
                    str(ROOT / 'tests/p4_program_store_v2_test.c'), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
    assert 'atomic commit/rollback OK' in result.stdout
    assert not result.stderr
    for version, cells, slots in [(1, 64, 64), (2, 256, 256)]:
        wire = bytearray(32 + slots * 4 + 32)
        struct.pack_into('<4sBBHIII', wire, 0, f'P4S{version}'.encode(), version, 0, 32,
                         len(wire), 17, 2**32 - 2)
        if version == 1:
            wire[20:23] = bytes([cells, 8, 4])
        else:
            struct.pack_into('<HBB', wire, 20, cells, 8, 4)
        struct.pack_into(f'<{cells}i', wire, 32,
                         *[-2**31 + i if i % 2 else 2**31 - 1 - i for i in range(cells)])
        struct.pack_into('<8i', wire, 32 + slots * 4, *range(-4, 4))
        struct.pack_into('<I', wire, 28, zlib.crc32(wire[:28] + wire[32:]))
        actual = subprocess.run([str(binary), f'v{version}'], capture_output=True, check=True)
        assert actual.stdout == wire


def test_codec_and_persistence_have_no_format_sized_stack_frames(tmp_path):
    for name in ('store','persistence'):
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                        '-Wframe-larger-than=512','-fstack-usage',*FLAGS,'-I',str(ROOT/'main'),
                        '-c',str(ROOT/'main'/f'pet_p4_program_{name}.c'),'-o',str(tmp_path/f'{name}.o')],check=True)
        usage=(tmp_path/f'{name}.su').read_text()
        for line in usage.splitlines():
            assert int(line.split('\t')[1])<=512,line


def test_wide_snapshot_through_actual_ab_store_restart_and_checkpoint(tmp_path):
    from petui_app_store_test import _run_private_save_generations
    _run_private_save_generations(tmp_path, False, wide_program=True)
