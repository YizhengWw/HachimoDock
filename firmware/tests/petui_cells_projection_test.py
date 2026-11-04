"""Production CELLS projection keeps cursor, without hashing model cells into a key."""
import subprocess
from pathlib import Path
from p4_catalog_membership_reboot_test import function
ROOT=Path(__file__).resolve().parents[1]


def test_actual_cells_projection_is_grid_independent_but_preserves_cursor(tmp_path):
    source=(ROOT/'main/pet_p4_miniapp.c').read_text()
    start=source.index('      if(n->type==PET_SURFACE_NODE_CELLS){',source.index('static bool app_surface_locked('))
    end=source.index('\n#endif',start)
    projection=source[start:end]
    body=r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_surface.h"
#include "pet_p4_cells.h"
typedef struct{char name[32];int int_value;}var_t;
typedef struct{int var_count;var_t vars[2];pet_cells_t cells;}miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
'''+function(source,'find_var')+'''
static void project(pet_surface_node_t*n){
'''+projection+r'''
}
int main(void){
 g_runtime=(miniapp_runtime_t){.var_count=2,.vars={{.name="x",.int_value=2},{.name="y",.int_value=3}},.cells={.enabled=true,.cols=16,.rows=16,.bits=4}};
 pet_surface_node_t n={.type=PET_SURFACE_NODE_CELLS,.bound_value=123};strcpy(n.cursor_x,"x");strcpy(n.cursor_y,"y");project(&n);
 assert(n.cursor_col==2&&n.cursor_row==3&&n.bound_value==123);pet_surface_node_t before=n;
 // All 256 model cells may change without modifying node/key projection.
 for(unsigned i=0;i<PET_CELLS_MAX;i++)g_runtime.cells.values[i]=(uint8_t)(i%16);
 project(&n);assert(!memcmp(&before,&n,sizeof(n)));
 for(unsigned i=0;i<PET_CELLS_MAX;i++)g_runtime.cells.values[i]^=15;
 project(&n);assert(!memcmp(&before,&n,sizeof(n)));
 g_runtime.vars[0].int_value=-20;g_runtime.vars[1].int_value=300;project(&n);assert(n.cursor_col==-1&&n.cursor_row==255&&n.bound_value==123);
 n.cursor_x[0]=n.cursor_y[0]=0;project(&n);assert(n.cursor_col==-1&&n.cursor_row==-1);return 0;
}
'''
    src=tmp_path/'projection.c';exe=tmp_path/'projection';src.write_text(body)
    subprocess.run(['cc','-std=c11','-DPET_P4_CELLS_V1=1','-fsanitize=address,undefined','-I'+str(ROOT/'main'),'-I'+str(ROOT/'tests/vendor/cjson'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
