"""Bounded mixed text/integer editor, sanitizer and native/preview parity."""
import copy,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def test_form_v2_native_core(tmp_path):
    exe=tmp_path/'form-v2'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-I'+str(ROOT/'firmware/main'),str(ROOT/'firmware/tests/p4_form_v2_core_test.c'),str(ROOT/'firmware/main/pet_p4_form_v2.c'),'-o',str(exe)],check=True,timeout=30)
    subprocess.run([str(exe)],check=True,timeout=10)

def test_form_v2_strict_shared_contract(tmp_path):
    sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
    from app_form_v2 import errors
    fixture=ROOT/'pc/skills/petui/contracts/app-form-v2.json';suite=json.loads(fixture.read_text())
    assert not errors(suite['runtime'],suite['buttons'])
    for case in suite['invalid']:
        runtime=copy.deepcopy(suite['runtime']);parent=runtime
        for key in case['path'][:-1]:parent=parent[key]
        parent[case['path'][-1]]=case['value'];assert errors(runtime,suite['buttons']),case
    source=tmp_path/'parse.c';exe=tmp_path/'parse';vendor=ROOT/'firmware/tests/vendor/cjson'
    source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pet_p4_form_v2_json.h"
#include "pet_p4_app_navigation.h"
int main(int argc,char**argv){
 assert(argc==2);FILE*file=fopen(argv[1],"rb");assert(file);char raw[32768];size_t n=fread(raw,1,sizeof(raw)-1,file);fclose(file);raw[n]=0;
 cJSON*suite=cJSON_Parse(raw);assert(suite);const cJSON*r=pet_form_get(suite,"runtime"),*buttons=pet_form_get(suite,"buttons");
 pet_form_t form;pet_form_text_config_t text;assert(pet_form2_parse(r,buttons,&form,&text));const cJSON*c;
 cJSON_ArrayForEach(c,pet_form_get(suite,"invalid")){
  cJSON*copy=cJSON_Duplicate(r,true),*parent=copy;const cJSON*path=pet_form_get(c,"path");int count=cJSON_GetArraySize(path);
  for(int i=0;i<count-1;i++){const cJSON*k=cJSON_GetArrayItem(path,i);parent=cJSON_IsString(k)?cJSON_GetObjectItemCaseSensitive(parent,k->valuestring):cJSON_GetArrayItem(parent,k->valueint);assert(parent);}
  const cJSON*k=cJSON_GetArrayItem(path,count-1);cJSON*value=cJSON_Duplicate(pet_form_get(c,"value"),true);
  if(cJSON_IsString(k)){cJSON_DeleteItemFromObjectCaseSensitive(parent,k->valuestring);cJSON_AddItemToObject(parent,k->valuestring,value);}else cJSON_ReplaceItemInArray(parent,k->valueint,value);
  pet_form_t old=form;pet_form_text_config_t old_text=text;
  if(pet_form2_parse(copy,buttons,&form,&text)){fprintf(stderr,"unexpected valid case: %s\n",cJSON_PrintUnformatted(c));abort();}
  assert(!memcmp(&old,&form,sizeof(form))&&!memcmp(&old_text,&text,sizeof(text)));cJSON_Delete(copy);
 }
 cJSON*bad=cJSON_Duplicate(buttons,true);cJSON_DeleteItemFromArray(bad,0);assert(!pet_form2_parse(r,bad,&form,&text));cJSON_Delete(bad);
 bad=cJSON_Duplicate(r,true);cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(bad,"form"),"engine","p4-app-form-v2");assert(!pet_form2_parse(bad,buttons,&form,&text));cJSON_Delete(bad);
 char pages[4][24]={{"edit"},{"home"}};
 cJSON_ArrayForEach(c,pet_form_get(suite,"cases")){
  pet_form_value_t values[8]={0};int i=0;const cJSON*v;int8_t page=0;pet_nav_history_t history={0};
  cJSON_ArrayForEach(v,pet_form_get(r,"vars")){const cJSON*value=pet_form_get(pet_form_get(c,"restore"),v->string);if(!value)value=pet_form_get(v,"init");
   if(cJSON_IsString(value))strcpy(values[i].text,value->valuestring);else values[i].number=value->valueint;i++;}
  assert(pet_form2_initialize(&form,&text,values));
  cJSON_ArrayForEach(v,pet_form_get(c,"actions")){
   if(!strcmp(v->valuestring,"@hide")){pet_form2_cancel(&form,&text,values);continue;}
   int op=pet_form2_operation(&form,&text,v->valuestring);if(op>=0){pet_form2_apply(&form,&text,values,page,op);continue;}
   const cJSON*t;cJSON_ArrayForEach(t,pet_form_get(r,"transitions"))if(!strcmp(pet_form_string(t,"on"),v->valuestring)){
    pet_nav_rule_t nav;assert(pet_nav_parse_rule(true,pages,2,t,&nav));if(!pet_nav_allowed(&history,page,&nav))continue;
    pet_form2_cancel(&form,&text,values);pet_nav_apply(&history,&page,&nav);break;}
  }
  for(i=0;i<8;i++){const cJSON*expected=cJSON_GetArrayItem(pet_form_get(c,"vars"),i);assert(cJSON_IsString(expected)?!strcmp(values[i].text,expected->valuestring):values[i].number==expected->valueint);}
  assert(!strcmp(pages[page],pet_form_string(c,"page")));for(i=0;i<form.count;i++){char display[121];assert(pet_form2_text(&form,&text,values,i,display));assert(!strcmp(display,cJSON_GetArrayItem(pet_form_get(c,"text"),i)->valuestring));}
 }
 pet_form_value_t fuzz[8]={0};strcpy(fuzz[0].text,"中");fuzz[2].number=2;assert(pet_form2_initialize(&form,&text,fuzz));
 uint32_t seed=7,hash=2166136261U;
 for(unsigned iteration=0;iteration<5000;iteration++){
  seed=seed*1664525U+1013904223U;pet_form2_apply(&form,&text,fuzz,0,(int)((seed>>16)%8));
  for(unsigned i=0;i<8;i++){
   if(i==0||i==1||i==7){const unsigned char*p=(const unsigned char*)fuzz[i].text;do{hash=(hash^*p)*16777619U;}while(*p++);}
   else{uint32_t value=(uint32_t)fuzz[i].number;for(unsigned b=0;b<4;b++)hash=(hash^((value>>(8*b))&255U))*16777619U;}
  }
 }
 printf("%u\n",hash);
 cJSON_Delete(suite);return 0;
}''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-Wno-misleading-indentation','-fsanitize=address,undefined','-I'+str(ROOT/'firmware/main'),'-I'+str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_form_v2.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True,timeout=30)
    native=subprocess.run([str(exe),str(fixture)],check=True,timeout=10,capture_output=True,text=True)
    javascript=r'''
import fs from 'node:fs';import {initialApp} from './pc/src/component-center/app-runtime.js';import {formApply} from './pc/src/component-center/app-form.js';
const r=JSON.parse(fs.readFileSync(process.argv[1])).runtime,s=initialApp(r),ops=['previous','next','decrease','increase','accept','cancel','append','erase'];
let seed=7,hash=2166136261;const encoder=new TextEncoder(),buffer=new ArrayBuffer(4),view=new DataView(buffer);
const feed=byte=>{hash=Math.imul(hash^byte,16777619)>>>0;};
for(let i=0;i<5000;i++){
 seed=(Math.imul(seed,1664525)+1013904223)>>>0;formApply(r,s,ops[(seed>>>16)%8]);
 for(const value of Object.values(s.vars)){if(typeof value==='string'){for(const b of encoder.encode(value))feed(b);feed(0);}else{view.setInt32(0,value,true);for(const b of new Uint8Array(buffer))feed(b);}}
}
console.log(hash);
'''
    wasm=subprocess.run(['node','--input-type=module','-e',javascript,str(fixture)],cwd=ROOT,check=True,capture_output=True,text=True,timeout=30)
    assert native.stdout.strip()==wasm.stdout.strip()

def test_form_v2_artifacts_and_javascript_parity():
    subprocess.run(['node','pc/scripts/build-form-runtime.mjs','--check'],cwd=ROOT,check=True,timeout=30)
    subprocess.run(['node','--test','--test-concurrency=1','pc/src/component-center/app-form-v2.test.js'],cwd=ROOT,check=True,timeout=30)

def test_form_v2_full_install_validation_and_shared_smoke(tmp_path):
    sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
    from validate_generated_widget import validate_runtime,validate_buttons,DEFAULT_CAPABILITIES
    from bounded_app import smoke,initial,action
    suite=json.loads((ROOT/'pc/skills/petui/contracts/app-form-v2.json').read_text());r=suite['runtime'];buttons=suite['buttons']
    caps=dict(DEFAULT_CAPABILITIES,widgetAppFormV2='p4-app-form-v2');failures=[]
    actions=validate_runtime(r,'tool',tmp_path,caps,failures);validate_buttons(buttons,actions,caps,r,failures);assert not failures
    failures=[];validate_runtime(r,'tool',tmp_path,DEFAULT_CAPABILITIES,failures);assert any('表单' in e for e in failures)
    result=smoke(r,buttons);assert result['ok'] and result['mode']=='shared-native-mixed-form' and not result['deviceVerified']
    s=action(r,initial(r),'form.accept');s=action(r,s,'text.append');assert s['vars']['title']=='中' and s['vars']['titleDraft']=='中a'

def test_form_v2_cross_object_storage_and_stack_bounds(tmp_path):
    from p4_program_test import p4_compiler,run
    import pytest
    compiler=p4_compiler()
    if not compiler:pytest.skip('Existing P4 compiler required; no toolchain is downloaded')
    obj=tmp_path/'typed-form.o'
    run([compiler,'-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Os','-ffreestanding','-fno-builtin','-fstack-usage','-march=rv32imafc_zicsr_zifencei_xesppie','-mabi=ilp32f','-I',ROOT/'pc/scripts/game-wasm','-I',ROOT/'firmware/main','-c',ROOT/'firmware/main/pet_p4_form_v2.c','-o',obj])
    prefix=str(compiler).removesuffix('gcc');sizes=run([prefix+'size',obj]).splitlines()[-1].split();assert int(sizes[1])==int(sizes[2])==0
    for line in obj.with_suffix('.su').read_text().splitlines():
        _,amount,kind=line.split('\t');assert kind=='static' and int(amount)<=1024,line
    imports={line.split()[-1] for line in run([prefix+'nm','-u',obj]).splitlines()}
    assert imports<={'memcpy','memset','memcmp','strcmp','__divdi3','__moddi3','__udivdi3','__umoddi3'}
