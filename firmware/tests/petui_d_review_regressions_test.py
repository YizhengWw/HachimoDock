"""Actual bounded C/WASM regressions for review D03/D04/D07. No device/IO providers."""
import json
import subprocess
from pathlib import Path
from petui_input_policy_test import ROOT, guard_support, policy_harness_prefix
from p4_catalog_membership_reboot_test import function


def test_notify_full_live_table_refuses_without_forgetting_cooldowns(tmp_path):
    src=tmp_path/'notify.c';exe=tmp_path/'notify'
    src.write_text(r'''
#include <assert.h>
#include "pet_p4_notify.h"
int main(void){pet_notify_limiter_t l={0};char id[48];
 for(int i=0;i<PET_NOTIFY_SLOTS;i++){snprintf(id,sizeof(id),"c%d",i);assert(pet_notify_admit(&l,id,i));}
 pet_notify_limiter_t before=l;
 assert(!pet_notify_admit(&l,"overflow",8)&&!memcmp(&before,&l,sizeof(l)));
 assert(!pet_notify_admit(&l,"c0",9));
 assert(!pet_notify_admit(&l,"c7",0)); /* rollback cannot bypass cooldown */
 assert(!pet_notify_admit(&l,"overflow",29999));
 assert(pet_notify_admit(&l,"overflow",30000));
 assert(!pet_notify_admit(&l,"c0",30000)); /* no expired slot left yet */
 assert(pet_notify_admit(&l,"c0",30001));
 return 0;}
''')
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I'+str(ROOT/'firmware/main'),'-I'+str(ROOT/'firmware/tests/vendor/cjson'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


def test_pinyin_native_digit_cycle_and_wasm_preview_agree(tmp_path):
    src=tmp_path/'pinyin.c';exe=tmp_path/'pinyin'
    src.write_text(r'''
#include <assert.h>
#include "pet_p4_form_v3.h"
int main(void){pet_form3_t f={.enabled=true,.count=1,.page=0,.focus_var=0,.active_var=1,.key_var=4,.cursor_var=5,.buffer_var=6,.label_var=7,.candidates_var=8,
 .fields={{.id="name",.var=2,.draft_var=3,.mode=PET_FORM3_PINYIN,.max_bytes=63}}};
 for(unsigned i=0;i<PET_FORM3_ACTIONS;i++)snprintf(f.actions[i],48,"a%u",i);
 pet_form3_value_t v[PET_FORM3_VARS]={0};assert(pet_form3_initialize(&f,v));assert(pet_form3_apply(&f,v,0,PET_FORM_ACCEPT));
 for(int d=2;d<=9;d++){assert(v[4].number==d);assert(pet_form3_allowed(&f,v,0,PET_FORM_APPEND));assert(pet_form3_apply(&f,v,0,PET_FORM_INCREASE));}
 assert(v[4].number==2);assert(pet_form3_apply(&f,v,0,PET_FORM_DECREASE));assert(v[4].number==9);
 assert(pet_form3_apply(&f,v,0,PET_FORM_APPEND));assert(!strcmp(v[6].text,"9"));
 assert(pet_form3_apply(&f,v,0,PET_FORM_DECREASE));assert(v[4].number==8);
 assert(pet_form3_apply(&f,v,0,PET_FORM_APPEND));assert(!strcmp(v[6].text,"98"));
 uint16_t out[PET_FORM3_CANDIDATES_MAX];assert(pet_form3_candidates("96",out,PET_FORM3_CANDIDATES_MAX)>0);
 assert(pet_form3_candidates("94664",out,PET_FORM3_CANDIDATES_MAX)>0);return 0;}
''')
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I'+str(ROOT/'firmware/main'),str(src),str(ROOT/'firmware/main/pet_p4_form_v2.c'),str(ROOT/'firmware/main/pet_p4_form_v3.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    script=r'''
import fs from 'node:fs';import assert from 'node:assert/strict';
import {textForm} from './pc/skills/petui/scripts/form-v3-core.mjs';
const r=JSON.parse(fs.readFileSync('pc/skills/petui/contracts/app-form-v3.json')).runtime;
r.form.fields[0].keyboard='pinyin';const state={page:r.form.page,vars:Object.fromEntries(Object.entries(r.vars).map(([k,v])=>[k,v.init]))};
textForm(r,state,'initialize');textForm(r,state,'apply','accept');
const key=r.form.keyboard.key_var,buffer=r.form.keyboard.buffer_var;
for(let d=2;d<=9;d++){assert.equal(state.vars[key],d);assert(textForm(r,state,'allowed','append'));textForm(r,state,'apply','increase');}
assert.equal(state.vars[key],2);textForm(r,state,'apply','decrease');assert.equal(state.vars[key],9);
textForm(r,state,'apply','append');assert.equal(state.vars[buffer],'9');
'''
    subprocess.run(['node','--input-type=module','-e',script],cwd=ROOT,check=True)
    import sys
    sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
    import app_form_v3 as model
    r=json.loads((ROOT/'pc/skills/petui/contracts/app-form-v3.json').read_text())['runtime']
    r['form']['fields'][0]['keyboard']='pinyin'
    state={'page':r['form']['page'],'vars':{k:v['init']for k,v in r['vars'].items()}}
    model.initialize(r,state);model.apply(r,state,'accept');key=r['form']['keyboard']['key_var']
    for digit in range(2,10):
        assert state['vars'][key]==digit and model.allowed(r,state,'append')
        model.apply(r,state,'increase')
    assert state['vars'][key]==2



def test_real_plain_widget_admission_clears_previous_widgets_source_filter(tmp_path):
    code=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text();prefix=policy_harness_prefix()
    prefix=prefix.replace('#include "pet_p4_app_form.h"','#include "pet_p4_app_form.h"\n#include "pet_p4_data_rows.h"\n#define PET_P4_DATA_ROWS 1')
    prefix=prefix.replace('typedef struct {int var,value_var,value,op;}', 'typedef struct {bool app_host;char data_source[48];int data_page;pet_p4_data_view_t data;}pet_p4_miniapp_view_t;\ntypedef struct {int var,value_var,value,op;}')
    prefix=prefix.replace('pet_local_config_t local;', 'pet_data_rows_config_t data_rows;pet_local_config_t local;')
    prefix=prefix.replace('pet_data_rows_config_t data_rows;','pet_data_rows_config_t data_rows;pet_p4_miniapp_view_t view;')
    a=prefix.index('typedef struct {int pages,page;}pet_p4_data_paging_t;');b=prefix.index('static const char *json_string',a)
    prefix=prefix[:a]+'''static uint64_t mock_now;static uint64_t esp_timer_get_time(void){return mock_now*1000;}\n'''+prefix[b:]
    a=code.index('static bool runtime_has_transition_action(');b=code.index('static bool json_int_range(',a)
    extra='''\nstatic miniapp_runtime_t g_runtime;\n#define miniapp_runtime_lock() ((void)0)\n#define miniapp_runtime_unlock() ((void)0)\nstatic void pet_app_host_view(pet_p4_miniapp_view_t*v,uint64_t now){(void)v;(void)now;}\n'''
    body=r'''
#include <assert.h>
int main(void){
 cJSON*p=cJSON_Parse("{\"schema\":1,\"source\":\"test.shared\",\"status\":\"ok\",\"ttlMs\":30000,\"message\":\"\",\"rows\":[]}");
 cJSON*rows=cJSON_GetObjectItemCaseSensitive(p,"rows");
 for(int i=0;i<10;i++){cJSON*r=cJSON_CreateObject();char id[8];snprintf(id,sizeof(id),"r%d",i);cJSON_AddStringToObject(r,"id",id);cJSON_AddStringToObject(r,"label",i?"other":"one");cJSON_AddStringToObject(r,"value","");cJSON_AddStringToObject(r,"detail","");cJSON_AddStringToObject(r,"meta","");cJSON_AddNumberToObject(r,"tone",0);cJSON_AddItemToArray(rows,r);}
 assert(pet_p4_widget_data_apply(p,1));cJSON_Delete(p);
 pet_data_rows_config_t f={.enabled=true,.filter_count=1,.filters={{.field=PET_DATA_FIELD_LABEL,.count=1,.values={"one"}}}};
 int32_t values[PET_DATA_FILTERS_MAX]={1};pet_p4_widget_data_set_filter("test.shared",&f,values);
 assert(pet_p4_widget_data_paging("test.shared",0).pages==1);
 miniapp_runtime_t r={.input_v1=true,.transition_count=1,.var_count=1};strcpy(r.data_source,"test.shared");
 r.transitions[0]=(miniapp_transition_t){.action="next",.from_state=-1,.to_state=-1,.page_op=-1,.effect_count=1,.effects={{.var_index=0,.source_var=-1,.op=MINIAPP_EFFECT_INC,.int_value=1}}};
 assert(runtime_action_enabled(&r,"next"));assert(pet_p4_widget_data_paging("test.shared",0).pages==2);
 pet_p4_widget_data_set_filter("test.shared",&f,values);assert(pet_p4_widget_data_paging("test.shared",0).pages==1);
 g_runtime=r;strcpy(g_runtime.view.data_source,"test.shared");pet_p4_miniapp_view_t out;
 assert(pet_p4_miniapp_get_view(&out)&&out.data.pages==2&&out.data.count==5);
 return 0;}
'''
    src=tmp_path/'filter.c';exe=tmp_path/'filter';src.write_text(prefix+guard_support(code)+extra+code[a:b]+function(code,"pet_p4_miniapp_get_view")+body)
    subprocess.run(['cc','-std=c11','-DPET_P4_DATA_ROWS=1','-fsanitize=address,undefined','-I'+str(ROOT/'firmware/main'),'-I'+str(ROOT/'firmware/tests/vendor/cjson'),'-I'+str(ROOT/'firmware/tests/stubs/widget_data'),str(src),str(ROOT/'firmware/main/pet_p4_widget_data.c'),str(ROOT/'firmware/main/pet_p4_local_tools.c'),str(ROOT/'firmware/tests/vendor/cjson/cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
