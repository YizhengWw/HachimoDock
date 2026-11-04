"""Legacy PC editor now executes the same existing C form state machine.

Pure synthetic fixture; no installed components, saves, devices or providers.
"""
import os

from p4_program_test import ROOT, WARNINGS, run

REPO=ROOT.parent


def test_legacy_product_artifacts_reproducible_and_mirrors_complete():
    run([os.environ.get('PET_FRAMEWORK_NODE','node'), REPO/'pc/scripts/build-legacy-form-runtime.mjs', '--check'])


def test_legacy_bridge_native_abi_matches_real_parser_core_and_product_wasm(tmp_path):
    source=tmp_path/'legacy.c';executable=tmp_path/'legacy'
    source.write_text(r'''
#include "pet_p4_app_form_json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void f1_begin(unsigned,unsigned,unsigned,unsigned);
void f1_field(unsigned,unsigned,unsigned,int32_t,int32_t,int32_t,unsigned);
void f1_option(unsigned,unsigned,int32_t);int f1_commit(void);
int32_t *f1_values(void);int f1_initialize(void),f1_cancel(void),f1_allowed(int,int),f1_apply(int,int),f1_choice_index(unsigned);
int32_t f1_display_value(unsigned);
int main(int argc,char **argv){
  assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);char raw[32768];size_t bytes=fread(raw,1,sizeof(raw)-1,file);fclose(file);raw[bytes]=0;
  cJSON *suite=cJSON_Parse(raw);assert(suite);const cJSON *r=pet_form_get(suite,"runtime");pet_form_t form;
  assert(pet_form_parse(r,pet_form_get(suite,"buttons"),&form));
  f1_begin(form.count,form.page,form.focus_var,form.active_var);
  for(unsigned i=0;i<form.count;i++){
    const pet_form_field_t *f=&form.fields[i];f1_field(i,f->var,f->draft_var,f->option_count?0:f->min,f->option_count?0:f->max,f->option_count?0:f->step,f->option_count);
    for(unsigned j=0;j<f->option_count;j++)f1_option(i,j,f->values[j]);
  }
  assert(f1_commit());int32_t expected[8]={0},*values=f1_values();unsigned slot=0;const cJSON *v;
  cJSON_ArrayForEach(v,pet_form_get(r,"vars")){expected[slot]=pet_form_get(v,"init")->valueint;values[slot]=expected[slot];slot++;}
  pet_form_initialize(&form,expected);f1_initialize();assert(!memcmp(values,expected,sizeof(expected)));
  uint32_t seed=7,hash=2166136261U;
  for(unsigned iteration=0;iteration<5000;iteration++){
    seed=seed*1664525U+1013904223U;int op=(int)((seed>>16)%6);
    assert((bool)f1_allowed(0,op)==pet_form_allowed(&form,expected,0,op));
    assert((bool)f1_apply(0,op)==pet_form_apply(&form,expected,0,op));
    assert(!memcmp(values,expected,sizeof(expected)));
    for(unsigned i=0;i<form.count;i++){
      const pet_form_field_t *f=&form.fields[i];int32_t shown=expected[expected[form.active_var]==1?f->draft_var:f->var];
      assert(f1_display_value(i)==shown&&f1_choice_index(i)==pet_form_choice(f,shown));
    }
    for(unsigned i=0;i<8;i++)for(unsigned j=0;j<4;j++)hash=(hash^(((uint32_t)values[i]>>(8*j))&255U))*16777619U;
  }
  printf("%u\n",hash);cJSON_Delete(suite);return 0;
}
''')
    vendor=ROOT/'tests/vendor/cjson'
    run(['cc', *WARNINGS, '-Wno-deprecated-declarations', '-Wno-misleading-indentation',
         '-fsanitize=address,undefined','-I',ROOT/'main','-I',vendor,source,
         REPO/'pc/scripts/game-wasm/form_v1_bridge.c',vendor/'cJSON.c','-lm','-o',executable])
    fixture=REPO/'pc/skills/petui/contracts/app-form-v1.json'
    native=run([executable,fixture])
    script=r'''
import fs from 'node:fs';
import {initialApp} from './pc/src/component-center/app-runtime.js';
import {formApply} from './pc/src/component-center/app-form.js';
const r=JSON.parse(fs.readFileSync(process.argv[1])).runtime,s=initialApp(r),ops=['previous','next','decrease','increase','accept','cancel'];
let seed=7,hash=2166136261;
for(let i=0;i<5000;i++){
 seed=(Math.imul(seed,1664525)+1013904223)>>>0;formApply(r,s,ops[(seed>>>16)%6]);
 const values=Object.values(s.vars);while(values.length<8)values.push(0);
 for(const value of values)for(let b=0;b<4;b++)hash=Math.imul(hash^((value>>>(8*b))&255),16777619)>>>0;
}
console.log(hash);
'''
    import subprocess
    result=subprocess.run([os.environ.get('PET_FRAMEWORK_NODE','node'),'--input-type=module','-e',script,str(fixture)],
                          cwd=REPO,check=True,capture_output=True,text=True,timeout=30)
    assert not result.stderr.strip() and result.stdout.strip()==native
