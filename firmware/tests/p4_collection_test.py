"""Native keyed-collection building block; no install/IO/resource authority."""
import json
import os
from pathlib import Path
import pytest
from p4_program_test import ROOT,WARNINGS,INCLUDES,FREESTANDING,run,wasm_tools,p4_compiler

CORE=ROOT/'main/pet_p4_collection.c'
TEST=ROOT/'tests/p4_collection_test.c'

def test_collection_stable_keys_bounded_pages_and_atomic_failures_native_sanitizers(tmp_path):
    executable=tmp_path/'collection'
    run([os.environ.get('CC','cc'),*WARNINGS,'-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',*INCLUDES,CORE,TEST,'-o',executable])
    result=json.loads(run([executable]))
    assert result['result']==0 and result['trials']==30000
    assert result['viewBytes']<=1800 and result['workspaceBytes']<=1900

def test_collection_same_c_fixed_wasm(tmp_path):
    tools=wasm_tools()
    if not tools:pytest.skip('requires existing local compiler/linker/node, never downloads')
    clang,node,linker=tools;objects=[]
    # Fixed C memory routines, not a provider/host import.
    memory=tmp_path/'memory.c'
    memory.write_text('''#include <stddef.h>\nvoid *memcpy(void*d,const void*s,size_t n){unsigned char*a=d;const unsigned char*b=s;for(size_t i=0;i<n;i++)a[i]=b[i];return d;}\nvoid *memset(void*d,int c,size_t n){unsigned char*a=d;for(size_t i=0;i<n;i++)a[i]=(unsigned char)c;return d;}\nint memcmp(const void*a,const void*b,size_t n){const unsigned char*x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return x[i]<y[i]?-1:1;return 0;}\nsize_t strlen(const char*s){size_t n=0;while(s[n])n++;return n;}\nint strcmp(const char*a,const char*b){for(;*a&&*a==*b;a++,b++){}return (unsigned char)*a-(unsigned char)*b;}\nchar *strcpy(char*d,const char*s){char*out=d;do{*d++=*s;}while(*s++);return out;}\n''')
    for index,source in enumerate([CORE,TEST,memory]):
        obj=tmp_path/f'{index}.o'
        run([clang,'--target=wasm32',*WARNINGS,'-O2',*FREESTANDING,'-nostdlib','-DPET_COLLECTION_WASM',
             '-I',ROOT.parent/'pc/scripts/game-wasm',*INCLUDES,'-c',source,'-o',obj]);objects.append(obj)
    binary=tmp_path/'collection.wasm';flavor=['-flavor','wasm'] if Path(linker).name=='rust-lld' else []
    run([linker,*flavor,'--no-entry','--export=pet_collection_selftest','--initial-memory=131072','--max-memory=131072','-z','stack-size=16384',*objects,'-o',binary])
    assert run([node,'-e',"const fs=require('node:fs'),m=new WebAssembly.Module(fs.readFileSync(process.argv[1]));if(WebAssembly.Module.imports(m).length)throw Error('imports');console.log(new WebAssembly.Instance(m,{}).exports.pet_collection_selftest());",binary])=='0'

def test_collection_p4_static_object_does_not_allocate_or_use_providers(tmp_path):
    compiler=p4_compiler()
    if not compiler:pytest.skip('requires existing P4 compiler, never invokes PlatformIO')
    obj=tmp_path/'collection.o'
    run([compiler,*WARNINGS,'-Os','-march=rv32imafc_zicsr_zifencei_xesppie','-mabi=ilp32f',*FREESTANDING,'-fstack-usage',*INCLUDES,'-c',CORE,'-o',obj])
    prefix=str(compiler).removesuffix('gcc')
    imports={line.split()[-1] for line in run([prefix+'nm','-u',obj]).splitlines()}
    assert imports<={'memcpy','memset','strcmp','strlen'}
    for row in obj.with_suffix('.su').read_text().splitlines():
        _,amount,kind=row.split('\t');assert kind=='static' and int(amount)<=256

def test_collection_product_artifacts_reproducible_and_no_device_capability(tmp_path):
    run([os.environ.get('PET_FRAMEWORK_NODE','node'),ROOT.parent/'pc/scripts/build-collection-runtime.mjs','--check'])
    protocol=(ROOT/'main/pet_p4_protocol.c').read_text()
    assert 'widgetCollection' not in protocol
    for name in ('pet_p4_miniapp.c','pet_p4_main.c','pet_p4_protocol.c'):
        assert 'pet_collection_project(' not in (ROOT/'main'/name).read_text()


def test_collection_native_executes_existing_pc_service_shared_contract(tmp_path):
    source=tmp_path/'contract.c';binary=tmp_path/'contract'
    source.write_text(r'''
#include "pet_p4_collection.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static pet_collection_state_t state,before;
static pet_collection_view_t view,prior;
static pet_collection_workspace_t work;
static pet_collection_row_t rows[128];
static const cJSON *get(const cJSON*v,const char*k){return cJSON_GetObjectItemCaseSensitive(v,k);}
static const char *text(const cJSON*v,const char*k){const cJSON*s=get(v,k);assert(cJSON_IsString(s));return s->valuestring;}
static unsigned number(const cJSON*v,const char*k){const cJSON*n=get(v,k);assert(cJSON_IsNumber(n));return (unsigned)n->valueint;}
int main(int argc,char**argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);static char raw[16384];size_t n=fread(raw,1,sizeof(raw)-1,f);assert(feof(f));fclose(f);raw[n]=0;
 cJSON*suite=cJSON_Parse(raw);assert(suite);const cJSON*initial=get(suite,"initialState"),*current=get(suite,"rows");
 state.offset=(uint16_t)number(initial,"offset");const cJSON*selected=get(initial,"selected");if(cJSON_IsString(selected))strcpy(state.selected,selected->valuestring);
 const char*operations[]={"status","page_next","page_previous","up","down","clear_selection","row_0","row_1","row_2","row_3","row_4","row_5","row_6","row_7"};
 const cJSON*step;cJSON_ArrayForEach(step,get(suite,"steps")){
  const cJSON*candidate=get(step,"rows");if(!candidate)candidate=current;
  int count=cJSON_GetArraySize(candidate);assert(cJSON_IsArray(candidate)&&count>=0&&count<=128);
  for(int i=0;i<count;i++){const cJSON*r=cJSON_GetArrayItem(candidate,i);rows[i]=(pet_collection_row_t){text(r,"key"),text(r,"label"),text(r,"detail")};}
  const char*op=text(step,"on");unsigned operation=0;for(;operation<14&&strcmp(op,operations[operation]);operation++){}assert(operation<14);
  before=state;prior=view;pet_collection_result_t result=pet_collection_project(rows,(unsigned)count,number(suite,"pageSize"),(pet_collection_operation_t)operation,&state,&view,&work);
  if(get(step,"error")){
   assert(result==(!strcmp(text(step,"error"),"no_row")?PET_COLLECTION_NO_ROW:PET_COLLECTION_BAD_DATA));
   assert(!memcmp(&before,&state,sizeof(state))&&!memcmp(&prior,&view,sizeof(view)));continue;
  }
  assert(result==PET_COLLECTION_OK);current=candidate;
  const cJSON*expected=get(step,"expected"),*saved=get(expected,"state"),*key=get(saved,"selected");
  assert(state.offset==number(saved,"offset")&&view.offset==number(expected,"offset"));
  assert(cJSON_IsNull(key)?!state.selected[0]:!strcmp(state.selected,key->valuestring));
  assert(view.selected_index==get(expected,"selectedIndex")->valueint);
  const cJSON*labels=get(expected,"labels");assert(cJSON_GetArraySize(labels)==view.visible);
  for(unsigned i=0;i<view.visible;i++)assert(!strcmp(view.rows[i].label,cJSON_GetArrayItem(labels,(int)i)->valuestring));
 }
 cJSON_Delete(suite);return 0;
}
''')
    vendor=ROOT/'tests/vendor/cjson'
    run(['cc',*WARNINGS,'-Wno-deprecated-declarations','-fsanitize=address,undefined',
         *INCLUDES,'-I',vendor,CORE,source,vendor/'cJSON.c','-lm','-o',binary])
    run([binary,ROOT.parent/'pc/skills/petui/contracts/collection-model-v1.json'])
