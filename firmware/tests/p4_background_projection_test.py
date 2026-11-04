"""Actual publication projection helper, isolated from scheduling/admission.
No program is pumped here; registry observation and mutex are local adapters.
"""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]

def test_background_seed_refreshes_first_published_view_without_pumping(tmp_path):
    source=function((ROOT/'main/pet_p4_miniapp_background.inc').read_text(),'background_seed_locked')
    apply=function((ROOT/'main/pet_p4_miniapp.c').read_text(),'component_program_apply_seed')
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_program_registry.h"
#define portMAX_DELAY 1
typedef struct {bool active;char widget_id[48];pet_component_program_config_t program;
 uint64_t program_content[3];pet_program_snapshot_t program_seed;bool program_seed_ready,program_background_owned;
 struct {int32_t int_value;} vars[8];int32_t visible;} miniapp_runtime_t;
static pet_program_registry_t g_background_programs;
static int g_background_mutex=1;static bool g_background_ready=true,locked,compatible=true;
static unsigned snapshots,refreshes;
static void xSemaphoreTake(int m,int timeout){assert(m==1&&timeout==1&&!locked);locked=true;}
static void xSemaphoreGive(int m){assert(m==1&&locked);locked=false;}
bool pet_program_registry_find(pet_program_registry_t *r,const char *id,pet_program_registration_t *out){
 assert(locked&&r==&g_background_programs&&!strcmp(id,"fixture"));*out=(pet_program_registration_t){.registry=&g_background_programs,.slot=0,.serial=1};return true;
}
bool pet_program_registry_snapshot(pet_program_registry_t *r,pet_program_registration_t h,pet_program_snapshot_t *out){
 assert(locked&&r==&g_background_programs&&h.slot==0);snapshots++;*out=(pet_program_snapshot_t){.schema=1,.revision=7,.cells=1,.outputs={9}};return true;
}
bool pet_component_program_seed_valid(const pet_component_program_config_t *c,const pet_program_snapshot_t *s){(void)c;return compatible&&s->schema==1;}
static void refresh_view(miniapp_runtime_t *r){assert(locked);refreshes++;r->visible=r->vars[0].int_value;}
'''+apply+source+r'''
int main(void){
 miniapp_runtime_t r={.active=true,.program.enabled=true,.program_content={1,2,3}};strcpy(r.widget_id,"fixture");
 memset(r.program.projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(r.program.projection.target));r.program.projection.target[0]=0;
 g_background_programs.entries[0].config=r.program;memcpy(g_background_programs.entries[0].content,(uint64_t[]){1,2,3},sizeof(uint64_t)*3);
 bool found=false;assert(background_seed_locked(&r,&found)&&found);
 assert(r.visible==9&&r.vars[0].int_value==9&&r.program_seed.revision==7&&r.program_background_owned&&refreshes==1&&snapshots==1&&!locked);
 r.visible=-4;compatible=false;assert(!background_seed_locked(&r,&found)&&found&&r.visible==-4&&refreshes==1);
 compatible=true;r.program_content[0]=99;assert(!background_seed_locked(&r,&found)&&found&&r.visible==-4&&refreshes==1);
 return 0;
}
'''
    path=tmp_path/'projection.c';path.write_text(code);binary=tmp_path/'projection'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
      '-fsanitize=address,undefined','-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
      '-DPET_COMPONENT_BACKGROUND_ENABLED=1','-I',str(ROOT/'main'),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)


def test_background_continuous_projection_imports_cells_and_preserves_scalar_changes(tmp_path):
    source=function((ROOT/'main/pet_p4_miniapp_background.inc').read_text(),'background_project_locked')
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_program_registry.h"
#include "pet_p4_cells.h"
#define portMAX_DELAY 1
#define MINIAPP_VAR_INT 1
typedef struct {unsigned var_count;struct {int type;int32_t int_value;} vars[8];
 pet_program_snapshot_t program_seed;bool program_seed_ready,program_needs_start,program_background_owned,program_suspended;
 pet_cells_t cells;} miniapp_runtime_t;
static pet_program_registry_t g_background_programs;
static int g_background_mutex=1;static bool locked,matching=true;
static unsigned refreshes,g_viewport_epoch;static pet_program_snapshot_t committed;
static int32_t scalar;
static void xSemaphoreTake(int m,int t){assert(m==1&&t==1&&!locked);locked=true;}
static void xSemaphoreGive(int m){assert(m==1&&locked);locked=false;}
static bool background_matches_under_registry(const miniapp_runtime_t*r,pet_program_registration_t*h){(void)r;assert(locked);h->slot=0;return matching;}
static void component_program_values(const miniapp_runtime_t*r,int32_t out[8]){for(unsigned i=0;i<8;i++)out[i]=r->vars[i].int_value;}
bool pet_program_registry_view(pet_program_registry_t*r,pet_program_registration_t h,const int32_t base[8],int32_t out[8],pet_program_snapshot_t*s){
 assert(locked&&r==&g_background_programs&&h.slot==0);memcpy(out,base,8*sizeof(*out));out[0]=scalar;*s=committed;return true;
}
static void refresh_view(miniapp_runtime_t*r){assert(locked&&r->program_background_owned);refreshes++;}
'''+source+r'''
int main(void){
 miniapp_runtime_t r={.var_count=1,.program_needs_start=true};r.vars[0].type=MINIAPP_VAR_INT;
 assert(pet_cells_init(&r.cells,2,2,2));committed.cells=4;committed.values[0]=1;
 assert(background_project_locked(&r)&&r.cells.values[0]==1&&refreshes==1&&g_viewport_epoch==1&&!locked);
 assert(background_project_locked(&r)&&refreshes==1); /* exact unchanged frame */
 committed.values[1]=3;committed.revision++;
 assert(background_project_locked(&r)&&r.cells.values[1]==3&&refreshes==2); /* cell-only commit */
 scalar=7;assert(background_project_locked(&r)&&r.vars[0].int_value==7&&refreshes==3); /* import must not clear scalar changed */
 matching=false;committed.values[0]=2;scalar=9;
 assert(!background_project_locked(&r)&&r.cells.values[0]==1&&r.vars[0].int_value==7&&refreshes==3&&!locked);
 return 0;
}
'''
    path=tmp_path/'continuous.c';path.write_text(code);binary=tmp_path/'continuous'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
      '-fsanitize=address,undefined','-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
      '-DPET_COMPONENT_BACKGROUND_ENABLED=1','-DPET_P4_CELLS_V1=1','-DPET_P4_PROGRAM_V2=1',
      '-DPET_P4_TEXT_TABLE=1','-DPET_P4_APP_DIRTY_CACHE=1','-I',str(ROOT/'main'),str(path),str(ROOT/'main/pet_p4_cells.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
