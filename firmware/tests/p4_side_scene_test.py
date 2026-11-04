"""Execute the shared continuous tile-world core; no audio or device required."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]

def test_generic_side_scene_core(tmp_path):
    executable = tmp_path / 'side-scene'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-Wno-misleading-indentation', '-fsanitize=address,undefined',
        '-I', str(ROOT/'main'), str(ROOT/'main/pet_p4_side_scene.c'),
        str(ROOT/'tests/p4_side_scene_test.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

def test_restored_checkpoint_enters_core_before_first_frame(tmp_path):
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    sync=code[code.index('static bool sync_side_runtime('):code.index('static bool parse_side_runtime(')]
    form_values=code[code.index('static void form_values('):code.index('static bool form_cancel(')]
    restore='\n'.join(function(code,n) for n in ('finish_component_restore','restore_component_runtime_from','restore_component_runtime'))
    source=tmp_path/'restore-side.c'
    source.write_text('''#include "pet_p4_side_scene.h"
#include "pet_p4_app_form.h"
#include "pet_p4_widget_trace.h"
#include "pet_p4_app_store.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
enum {MINIAPP_VAR_INT};
typedef struct {int32_t int_value;int type;} var_t;
typedef struct {bool side_enabled,slicing;unsigned save_version;char widget_id[48];pet_side_scene_t side;var_t vars[PET_FORM_VARS];int var_count;pet_form_t form;int side_vars[4],side_states[4],current_state;} miniapp_runtime_t;
static uint64_t g_viewport_epoch;
static int g_pressed_node;
static int saved_checkpoint;
static bool fail_read,fail_parse,fail_alloc;
static unsigned allocations,releases;
static void *heap_caps_malloc(size_t size,unsigned caps){assert(size==PET_APP_SAVE_BYTES&&caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(fail_alloc)return NULL;void *p=malloc(size);if(p)allocations++;return p;}
static void heap_caps_free(void *p){assert(p);releases++;free(p);}
static struct {bool occupied;} g_storage_lane;
static pet_resource_lease_t g_storage_resource_lease;
pet_app_store_status_t pet_app_store_load(const char *id,char out[PET_APP_SAVE_BYTES]){(void)id;(void)out;return fail_read?PET_APP_STORE_UNAVAILABLE:PET_APP_STORE_VALUE;}
pet_app_store_status_t pet_app_store_load_with_lease(const char *id,char out[PET_APP_SAVE_BYTES],pet_resource_lease_t lease){assert(lease.serial==7);return pet_app_store_load(id,out);}
static bool restore_app_runtime(miniapp_runtime_t*r){if(fail_parse)return false;r->vars[3].int_value=saved_checkpoint;return true;}
static bool restore_app_runtime_from(miniapp_runtime_t*r,bool cached){(void)cached;return restore_app_runtime(r);}
/* Side-scene saves must not enter the opt-in form refresh branch. */
static void refresh_view(miniapp_runtime_t*r){(void)r;assert(!"unexpected form refresh");}
'''+form_values+sync+restore+'''
int main(void){
 pet_side_config_t c={.width=20,.height=8,.tile_count=2,.actor_count=2,.lives=3,.run_speed=96,.acceleration=800,.friction=1000,.gravity=800,.jump_speed=280,.terminal_speed=480};
 c.tiles[1].flags=1;for(int i=120;i<160;i++)c.cells[i]=1;
 c.actors[0]=(pet_side_actor_def_t){.id="hero",.x=32,.y=80,.width=12,.height=16,.behavior=PET_SIDE_CONTROLLED};
 c.actors[1]=(pet_side_actor_def_t){.id="marker",.x=128,.y=80,.width=12,.height=16,.behavior=PET_SIDE_STATIC,.contact=PET_SIDE_CHECKPOINT};
 miniapp_runtime_t r={.side_enabled=true,.save_version=1,.side_vars={0,1,2,3},.side_states={1,3,4,2}};assert(pet_side_init(&r.side,&c));
 saved_checkpoint=2;fail_read=true;assert(!restore_component_runtime(&r)&&r.side.checkpoint==0);fail_read=false;
 fail_alloc=true;assert(!restore_component_runtime(&r)&&r.side.checkpoint==0);fail_alloc=false;
 fail_parse=true;assert(!restore_component_runtime(&r)&&r.side.checkpoint==0);fail_parse=false;
 assert(restore_component_runtime(&r));assert(r.side.phase==PET_SIDE_READY&&r.current_state==0&&r.side.checkpoint==2&&r.vars[3].int_value==2&&r.side.actors[0].x==128*256);
 pet_side_process(&r.side,20);sync_side_runtime(&r);assert(r.vars[3].int_value==2);
 saved_checkpoint=99;assert(restore_component_runtime(&r));assert(r.side.checkpoint==0&&r.vars[3].int_value==0&&r.side.actors[0].x==32*256);
 g_storage_lane.occupied=true;g_storage_resource_lease.serial=7;saved_checkpoint=2;
 assert(restore_component_runtime(&r)&&r.side.checkpoint==2);
 assert(allocations==releases);
 return 0;
}
''')
    executable=tmp_path/'restore-side'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(source),str(ROOT/'main/pet_p4_side_scene.c'),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
