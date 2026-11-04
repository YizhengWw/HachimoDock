"""p4-component-program-v2: shared contract through the real descriptor parser, pure runtime and cells core.

Both flag states are compiled: the v1 build must refuse every v2 package, the v2 build keeps v1 images valid
with their v1 budgets. Offline only; no lease, device or sound."""
from pathlib import Path
import copy
import json
import re
import subprocess
import pytest

ROOT=Path(__file__).resolve().parents[1]
SKILL=ROOT.parent/'pc/skills/petui'
SUITE=json.loads((SKILL/'contracts/component-program-v2.json').read_text())
V1=json.loads((SKILL/'contracts/component-program-v1.json').read_text())
V2_FLAGS=['-DPET_P4_PROGRAM_V2=1','-DPET_P4_TEXT_TABLE=1','-DPET_P4_APP_DIRTY_CACHE=1','-DPET_P4_CELLS_V1=1']
SOURCES=('component_program','program_core','program_image','program_runtime','program_timers','program_host','program_group','program_scheduler','event_bus','cells','component_program_session')

def patched(value,patch):
    value=copy.deepcopy(value);o=value;parts=patch['path'].strip('/').split('/')
    for k in parts[:-1]:o=o[int(k)] if isinstance(o,list) else o[k]
    o[int(parts[-1]) if isinstance(o,list) else parts[-1]]=patch['value'];return value

@pytest.fixture(scope='module')
def native(tmp_path_factory):
    directory=tmp_path_factory.mktemp('program-v2');vendor=ROOT/'tests/vendor/cjson';bins={}
    for v2 in (False,True):
        binary=directory/f'program-{int(v2)}'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation','-fsanitize=address,undefined',
            '-DPET_COMPONENT_PROGRAM_ENABLED=1',*(V2_FLAGS if v2 else []),'-I',str(ROOT/'main'),'-I',str(vendor),str(ROOT/'tests/p4_program_v2_test.c'),
            *[str(ROOT/'main'/f'pet_p4_{n}.c') for n in SOURCES],str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
        bins[v2]=binary
    def run(payload,mode='parse',v2=True):
        result=subprocess.run([str(bins[v2]),mode],input=json.dumps(payload),text=True,capture_output=True)
        assert result.returncode==0 and not result.stderr,(result.returncode,result.stderr);return json.loads(result.stdout)
    return run

def test_v2_contract_parses_only_on_a_v2_build_and_rejections_are_refused(native):
    r=SUITE['runtime'];assert SUITE['profile']=='p4-component-program-v2'
    assert native(r)=={'ok':True,'enabled':True,'v2':True}
    assert not native(r,v2=False)['ok'],'a v1 build must refuse p4-component-program-v2'
    for patch in SUITE['reject']:
        if patch.get('scope')=='package':continue  # whole-package validators, not the descriptor parser
        assert not native(patched(r,patch))['ok'],patch['name']
    # The v1 contract stays valid on both builds, with its v1 image budgets and op table.
    assert native(V1['runtime'])=={'ok':True,'enabled':True,'v2':False} and native(V1['runtime'],v2=False)['ok']
    for patch in SUITE['v1ImageReject']:
        assert not native(patched(V1['runtime'],patch))['ok'],patch['name']
    # A v2 engine with a v1 image is fine (same budgets as v1); a v2 image under the v1 engine is not.
    up=copy.deepcopy(V1['runtime']);up['program']['engine']='p4-component-program-v2';assert native(up)['v2']
    down=copy.deepcopy(r);down['program']['engine']='p4-component-program-v1';assert not native(down)['ok']
    # v2 image budgets: 1024 instructions, 16384 fuel, 256 cells.
    big=copy.deepcopy(r);big['program']['image']['code']=[['HALT',0,0,0,0]]*1024;big['program']['image']['cells']=256;big['program']['image']['fuel']=16384
    big['cells']={'engine':'p4-cells-v1','cols':16,'rows':16,'bits':2};assert native(big)['ok']
    big['program']['image']['code'].append(['HALT',0,0,0,0]);assert not native(big)['ok']

def test_cases_run_through_the_product_runtime_and_mirror_the_grid(native):
    for case in SUITE['cases']:
        out=native({'runtime':SUITE['runtime'],'events':case['events']},mode='run')
        assert out['ok'],(case['name'],out)
        assert {k:out['outputs'][k] for k in case['expected']['outputs']}==case['expected']['outputs'],case['name']
        assert out['cells']==case['expected']['cells'] and out['grid']==case['expected']['grid'],case['name']
        assert out['fuel']==16384
    # The v1 contract program still commits on the v2 build.
    out=native({'runtime':V1['runtime'],'events':[{'action':'add'},{'action':'add'}]},mode='run');assert out['ok'] and out['outputs']['count']==4

def test_image_wire_v2_header_store_ceiling_and_fuel_table(tmp_path):
    code=r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_program_image.h"
#include "pet_p4_program_store.h"
#include "pet_p4_resources.h"
static uint32_t crc(const uint8_t *b,size_t n){uint32_t c=0xffffffffu;for(size_t i=0;i<n;i++){if(i>=20&&i<24)continue;c^=b[i];for(int k=0;k<8;k++)c=(c>>1)^((c&1)?0xedb88320u:0);}return ~c;}
static void put32(uint8_t *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
int main(void){
  static uint8_t wire[24+8];memset(wire,0,sizeof wire);memcpy(wire,"P4P1",4);wire[4]=VERSION;wire[6]=24;wire[8]=1;wire[10]=64;
  wire[12]=CELLS&255;wire[13]=CELLS>>8;put32(wire+16,7);put32(wire+20,crc(wire,sizeof wire));
  static pet_program_image_t image;pet_program_image_result_t r=pet_program_image_decode(&image,wire,sizeof wire);
  assert(r==EXPECT);
  if(r==PET_PROGRAM_IMAGE_OK){assert(image.cells==CELLS);
    static pet_program_snapshot_t s;s.schema=7;s.cells=image.cells;static uint8_t out[PET_PROGRAM_STORE_BYTES];
    /* Legacy buffers stay bounded; wide P4S2 requires the explicit max size. */
    assert((pet_program_store_encode(out,sizeof out,&s)==PET_PROGRAM_STORE_OK)==(CELLS<=64));
    static uint8_t wide[PET_PROGRAM_STORE_MAX_BYTES];
    assert(pet_program_store_encode(wide,sizeof wide,&s)==PET_PROGRAM_STORE_OK);
    assert(pet_program_store_size(&s)==(CELLS>64?PET_PROGRAM_STORE_V2_BYTES:PET_PROGRAM_STORE_BYTES));}
  assert(pet_resources_limit(PET_RESOURCE_PROGRAM).units==UNITS);
  assert(PET_PROGRAM_QUANTUM==64);
  return 0;}
'''
    src=tmp_path/'wire.c';src.write_text(code)
    base=['-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1']
    cases=[([],1,64,'PET_PROGRAM_IMAGE_OK',4096),([],2,64,'PET_PROGRAM_IMAGE_BAD_HEADER',4096),([],1,65,'PET_PROGRAM_IMAGE_BAD_CODE',4096),
           (V2_FLAGS,1,64,'PET_PROGRAM_IMAGE_OK',16384),(V2_FLAGS,2,256,'PET_PROGRAM_IMAGE_OK',16384),(V2_FLAGS,2,257,'PET_PROGRAM_IMAGE_BAD_HEADER',16384),(V2_FLAGS,1,200,'PET_PROGRAM_IMAGE_BAD_CODE',16384)]
    for flags,version,cells,expect,units in cases:
        exe=tmp_path/'wire'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',*base,*flags,f'-DVERSION={version}',f'-DCELLS={cells}',f'-DEXPECT={expect}',f'-DUNITS={units}','-I',str(ROOT/'main'),str(src),
            *[str(ROOT/'main'/f'pet_p4_{n}.c') for n in ('program_core','program_image','program_store','resources')],'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)

def test_flag_gates_and_capability_report():
    features=(ROOT/'main/pet_p4_component_features.h').read_text();protocol=(ROOT/'main/pet_p4_protocol.c').read_text();core=(ROOT/'main/pet_p4_program_core.h').read_text()
    assert '#ifndef PET_P4_PROGRAM_V2\n#define PET_P4_PROGRAM_V2 0\n#endif' in features
    assert '#if PET_P4_PROGRAM_V2 && !PET_COMPONENT_PROGRAM_ENABLED\n#error' in features and '#if PET_P4_PROGRAM_V2 && !PET_P4_TEXT_TABLE\n#error' in features
    assert '#if PET_P4_CELLS_V1 && PET_COMPONENT_PROGRAM_ENABLED && !PET_P4_PROGRAM_V2\n#error' in features
    block=protocol[protocol.index('#if PET_P4_PROGRAM_V2\n  /* v2 supersedes v1'):];block=block[:block.index('#else')]
    assert 'cJSON_AddStringToObject(capabilities,"widgetProgram",PET_COMPONENT_PROGRAM_ENGINE_V2);' in block and '"widgetPrograms"' in block
    assert re.search(r'#if PET_P4_PROGRAM_V2\n.*?#define PET_PROGRAM_CELLS 256U\n#define PET_PROGRAM_CODE 1024U\n#if PET_P4_PUBLIC_PROGRAM_PROFILE\n#define PET_PROGRAM_FUEL 4096U\n#else\n#define PET_PROGRAM_FUEL 16384U\n#endif',core,re.S)
    assert '#define PET_PROGRAM_QUANTUM 64U' in core
    assert SUITE['limits']['v2']=={'code':1024,'fuel':16384,'cells':256,'quantum':64,'ops':SUITE['limits']['v1']['ops']+['TEXT','CELLGET','CELLSET'],'imageVersion':2}
