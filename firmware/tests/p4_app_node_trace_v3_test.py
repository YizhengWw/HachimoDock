"""Trace every admitted surface node without truncating candidate evidence."""
from pathlib import Path
import subprocess
import pytest
ROOT=Path(__file__).resolve().parents[1]
@pytest.mark.parametrize("v3",[0,1])
def test_all_nodes_fit_the_complete_numeric_trace(tmp_path,v3):
    src=tmp_path/"trace.c"
    src.write_text(r'''
#include "pet_p4_app_draw_trace.h"
#include <assert.h>
#include <string.h>
int main(void){
 pet_app_draw_trace_t trace={0};
 /* Upper admitted nodes must count in quiet/overlap diagnostics as well as logs. */
 trace.nodes_us[31]=7;trace.nodes_us[32]=11;trace.nodes_us[PET_APP_TRACE_NODES-1]=13;
 assert(pet_app_draw_trace_nodes_total(&trace)==31);
 trace.nodes_us[0]=UINT32_MAX;assert(pet_app_draw_trace_nodes_total(&trace)==UINT32_MAX);
 assert(!pet_app_draw_trace_nodes_total(NULL));
 trace=(pet_app_draw_trace_t){0};
 for(unsigned i=0;i<PET_APP_TRACE_NODES;i++){trace.nodes_us[i]=UINT32_MAX;trace.nodes_mask|=UINT64_C(1)<<i;}
 char line[PET_P4_QUOTA_V3?1536:1152];
 assert(pet_app_draw_trace_format(&trace,UINT32_MAX,line,sizeof(line)));
 assert(strstr(line,PET_P4_QUOTA_V3?" n63_us=4294967295":" n47_us=4294967295"));
 char tiny[20];assert(!pet_app_draw_trace_format(&trace,1,tiny,sizeof(tiny)));
 return 0;
}
''')
    exe=tmp_path/"trace"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",f"-DPET_P4_QUOTA_V3={v3}","-DPET_P4_APP_NODE_TRACE=1","-DPET_P4_APP_PHASE_TRACE=1","-I",str(ROOT/"main"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


def test_renderer_overlap_uses_the_complete_node_total():
    source=(ROOT/"main/pet_p4_renderer.c").read_text()
    assert "uint32_t nodes_us=pet_app_draw_trace_nodes_total(&g_render_trace.app_nodes);" in source
