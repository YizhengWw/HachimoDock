"""Exercise the real miniapp snapshot capture under wide VM budgets without hardware."""
from pathlib import Path
import subprocess

import pytest
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('wide', [0, 1])
def test_capture_uses_complete_snapshot_and_refuses_allocation_failure(tmp_path, wide):
    source = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    capture = function(source, 'save_component_program_snapshot')
    save = function(source, 'save_component_program')
    harness = r'''
#include "pet_p4_program_runtime.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static struct {bool active;struct {bool enabled;struct {unsigned cells;} image;} program;char widget_id[48];} g_runtime;
static struct {unsigned revision;} g_component_program;
static unsigned g_image_revision=1;
static bool g_component_program_initialized=true,fail_alloc;
static unsigned locks,allocations,frees,enqueues;
static void miniapp_runtime_lock(void){assert(!locks);locks++;}
static void miniapp_runtime_unlock(void){assert(locks==1);locks--;}
static void copy_utf8(char *out,size_t cap,const char *text){assert(strlen(text)<cap);strcpy(out,text);}
#if PET_P4_PROGRAM_V2
static void *heap_caps_malloc(size_t bytes,int caps){assert(!locks&&bytes==sizeof(pet_program_snapshot_t));assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(fail_alloc)return NULL;allocations++;return malloc(bytes);}
static void heap_caps_free(void *ptr){assert(ptr&&!locks);frees++;free(ptr);}
#endif
static bool component_program_snapshot_locked(pet_program_snapshot_t *out){assert(locks==1);memset(out,0,sizeof(*out));out->schema=1;out->cells=PET_PROGRAM_CELLS;out->values[PET_PROGRAM_CELLS-1]=12345;return true;}
static bool pet_program_persistence_enqueue(const char *id,const pet_program_snapshot_t *snapshot){assert(!locks&&!strcmp(id,"wide-capture"));assert(snapshot->cells==PET_PROGRAM_CELLS&&snapshot->values[PET_PROGRAM_CELLS-1]==12345);enqueues++;return true;}
'''+capture+'\n'+save+r'''
int main(void){
 strcpy(g_runtime.widget_id,"wide-capture");g_component_program.revision=1;
 assert(save_component_program());assert(enqueues==0&&!locks);
 g_runtime.active=g_runtime.program.enabled=true;g_runtime.program.image.cells=PET_PROGRAM_CELLS;
#if PET_P4_PROGRAM_V2
 fail_alloc=true;assert(!save_component_program());assert(enqueues==0&&allocations==0&&frees==0&&!locks);fail_alloc=false;
#endif
 assert(save_component_program());assert(enqueues==1&&!locks);
#if PET_P4_PROGRAM_V2
 assert(allocations==1&&frees==1);
#else
 (void)fail_alloc;(void)allocations;(void)frees;
#endif
 return 0;
}
'''
    path = tmp_path / 'capture.c'
    path.write_text(harness)
    binary = tmp_path / 'capture'
    flags = [f'-DPET_P4_PROGRAM_V2={wide}']
    if wide:
        flags += ['-DPET_COMPONENT_PROGRAM_ENABLED=1', '-DPET_P4_TEXT_TABLE=1', '-DPET_P4_APP_DIRTY_CACHE=1']
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                    '-fsanitize=address,undefined', *flags, '-I', str(ROOT / 'main'), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
