"""Actual observer + query JSON prefix, deterministic 256-cell content evidence.
No serial, VM execution, saving or hardware; allocation/clock/owner adapters only.
"""
from pathlib import Path
import json,re,struct,subprocess
ROOT=Path(__file__).resolve().parents[1]


def test_actual_observer_correlates_query_and_hashes_full_256_cells(tmp_path):
    header=(ROOT/'main/pet_p4_miniapp.h').read_text()
    observation=re.search(r'typedef struct \{\n  char widget_id\[48\];\n  uint32_t instance_generation.*?\} pet_p4_miniapp_observation_t;',header,re.S).group(0)
    protocol=(ROOT/'main/pet_p4_protocol.c').read_text()
    start=protocol.index('static void send_miniapp_state(')
    prefix=protocol[start:protocol.index('  bool active =',start)]+'  response=payload;\n}\n'
    code=r'''
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "pet_p4_cells.h"
#include "pet_p4_program_runtime.h"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MINIAPP_VAR_INT 1
typedef struct {bool active,app_host;char widget_id[48],state[24],page[24],data_source[48];uint32_t revision;int data_page,data;}pet_p4_miniapp_view_t;
static struct {
 bool active,save_page;char widget_id[48];unsigned save_version,save_vars,var_count;
 struct{char name[16],string_value[128];int type;int32_t int_value;}vars[3];
 pet_p4_miniapp_view_t view;pet_cells_t cells;struct{bool enabled;}program;
}g_runtime;
static uint32_t g_image_revision=7;
static struct {uint32_t revision;}g_component_program={7};
static bool g_component_program_initialized=true,oom,snapshot_fail;
static unsigned depth,allocations;
static pet_program_snapshot_t committed;
static void miniapp_runtime_lock(void){assert(!depth);depth=1;}
static void miniapp_runtime_unlock(void){assert(depth);depth=0;}
static void *heap_caps_calloc(size_t n,size_t s,int caps){assert(!depth&&caps==3);if(oom)return NULL;void*p=calloc(n,s);if(p)allocations++;return p;}
static void heap_caps_free(void*p){assert(!depth);if(p)allocations--;free(p);}
static void copy_utf8(char*out,size_t n,const char*s){snprintf(out,n,"%s",s);}
static bool component_program_snapshot_locked(pet_program_snapshot_t*out){assert(depth);if(snapshot_fail)return false;*out=committed;return true;}
static uint64_t esp_timer_get_time(void){return 0;}
static void pet_app_host_view(pet_p4_miniapp_view_t*v,uint64_t now){(void)v;(void)now;assert(!depth);}
static void pet_p4_widget_data_view(const char*s,int page,uint64_t now,int*out){(void)s;(void)page;(void)now;(void)out;assert(!depth);}
'''+observation+'\n#include "pet_p4_miniapp_observation.inc"\n'+r'''
typedef void(*pet_p4_send_line_fn)(const char*,void*);
static cJSON *response;
static const char*json_string(const cJSON*p,const char*k){const cJSON*v=cJSON_GetObjectItemCaseSensitive(p,k);return cJSON_IsString(v)?v->valuestring:"";}
static const char*pet_p4_boot_identity(void){return "0123456789abcdef";}
'''+prefix+r'''
int main(void){
 g_runtime.active=g_runtime.view.active=true;strcpy(g_runtime.widget_id,"fixture");strcpy(g_runtime.view.widget_id,"fixture");strcpy(g_runtime.view.state,"ready");strcpy(g_runtime.view.page,"main");g_runtime.view.revision=4;
 g_runtime.save_version=1;g_runtime.save_vars=3;g_runtime.var_count=3;
 strcpy(g_runtime.vars[0].name,"count");g_runtime.vars[0].type=1;g_runtime.vars[0].int_value=-7;
 strcpy(g_runtime.vars[1].name,"label");strcpy(g_runtime.vars[1].string_value,"saved");
 g_runtime.program.enabled=true;committed.schema=9;committed.cells=256;committed.revision=21;
 assert(pet_cells_init(&g_runtime.cells,16,16,2));
 for(unsigned i=0;i<256;i++){g_runtime.cells.values[i]=i%4;committed.values[i]=(int)i-128;}
 for(unsigned i=0;i<8;i++)committed.outputs[i]=(int)i-4;
 pet_p4_miniapp_observation_t a,b;pet_p4_miniapp_view_t view;
 assert(pet_p4_miniapp_observe(&a,&view)&&a.has_vm&&a.vm_cells==256&&a.cells_count==256&&a.instance_generation==7&&!allocations);
 g_runtime.vars[2].int_value=999;g_image_revision=8;g_component_program.revision=8;committed.revision++;
 assert(pet_p4_miniapp_observe(&b,&view)&&b.instance_generation==8&&b.persist_digest==a.persist_digest&&b.cells_digest==a.cells_digest&&b.vm_digest==a.vm_digest);
 g_runtime.cells.values[255]^=1;assert(pet_p4_miniapp_observe(&b,&view)&&b.cells_digest!=a.cells_digest);g_runtime.cells.values[255]^=1;
 committed.values[255]++;assert(pet_p4_miniapp_observe(&b,&view)&&b.vm_digest!=a.vm_digest);committed.values[255]--;
 strcpy(g_runtime.vars[1].string_value,"changed");assert(pet_p4_miniapp_observe(&b,&view)&&b.persist_digest!=a.persist_digest);strcpy(g_runtime.vars[1].string_value,"saved");
 snapshot_fail=true;assert(pet_p4_miniapp_observe(&b,&view)&&!b.has_vm);snapshot_fail=false;
 oom=true;assert(!pet_p4_miniapp_observe(&b,&view)&&!allocations&&!depth);oom=false;
 cJSON*request=cJSON_Parse("{\"requestId\":\"query-exact-1\"}");assert(request);send_miniapp_state(NULL,NULL,request);cJSON_Delete(request);
 assert(!strcmp(json_string(response,"requestId"),"query-exact-1"));assert(!strcmp(json_string(response,"bootId"),"0123456789abcdef"));
 assert(!strcmp(json_string(response,"instanceGeneration"),"00000008")&&!strcmp(json_string(response,"durability"),"not_observed"));
 assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response,"observationAvailable")));
 char*text=cJSON_PrintUnformatted(response);puts(text);cJSON_free(text);cJSON_Delete(response);assert(!allocations&&!depth);return 0;
}
'''
    # The new serializer prefix does not use the legacy sender/view alias yet.
    source=tmp_path/'observation.c';source.write_text(code);binary=tmp_path/'observation'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-parameter',
                    '-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined',
                    '-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_P4_PROGRAM_V2=1','-DPET_P4_TEXT_TABLE=1',
                    '-DPET_P4_APP_DIRTY_CACHE=1','-DPET_P4_CELLS_V1=1',
                    '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(source),
                    str(ROOT/'main/pet_p4_cells.c'),str(ROOT/'tests/vendor/cjson/cJSON.c'),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True,check=True,timeout=10)
    assert not result.stderr
    value=json.loads(result.stdout)
    def digest(blob):
        h=2166136261
        for b in blob:h=((h^b)*16777619)&0xffffffff
        return f'{h:08x}'
    le=lambda v:struct.pack('<I',v&0xffffffff)
    assert value['cellsDigest']['hash']==digest(le(16)+le(16)+le(2)+bytes(i%4 for i in range(256)))
    assert value['vmDigest']['hash']==digest(le(9)+le(256)+b''.join(le(i-128) for i in range(256))+b''.join(le(i-4) for i in range(8)))
    assert value['persistProjectionDigest']==digest(le(1)+le(3)+b'ready\0\0count\0\x01'+le(-7)+b'label\0\x02saved\0')
    assert 'pet_p4_miniapp_observation.inc' in (ROOT/'main/pet_p4_miniapp.c').read_text()
    assert 'send_miniapp_state(send_line, ctx, payload);' in protocol
