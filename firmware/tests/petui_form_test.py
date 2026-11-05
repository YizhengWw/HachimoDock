"""One contract drives the production C editor/parser and both preview models."""
import copy,json,re,subprocess,sys
from pathlib import Path
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[2]
SCRIPTS=ROOT/'pc/skills/petui/scripts';sys.path.insert(0,str(SCRIPTS))
from app_form import errors,initialize,cancel,text,allowed,apply
from bounded_app import initial,action
from app_surface import graphics_errors
from input_policy import input_errors
SUITE=ROOT/'pc/skills/petui/contracts/app-form-v1.json'
def test_single_field_legacy_smoke_keeps_disabled_actions_explicit_and_rejects_dead_touch():
    from app_form import structural_disabled
    from bounded_app import smoke
    suite=json.loads(SUITE.read_text());r=suite['runtime'];r['form']['fields']=r['form']['fields'][:1]
    r['surface']['elements']=[n for n in r['surface']['elements'] if n.get('field_bind')!='level']
    # Actual page visibility is necessary feedback for navigation, not just a
    # different hidden page index with an identical screen.
    r['surface']['elements'][0]['page']=0
    assert not errors(r,suite['buttons'])
    report=smoke(r,suite['buttons']);assert report['ok'],report
    assert report['disabledActions']==structural_disabled(r)=={'field.prev':'single_field','field.next':'single_field'}
    r['transitions'].append({'from':'*','from_page':'home','on':'dead'})
    r['surface']['elements'].append({'type':'button','page':1,'x':20,'y':300,'w':180,'h':48,'color':65535,'text':'Dead','action':'dead'})
    report=smoke(r,suite['buttons']);assert not report['ok'] and 'dead' not in report['disabledActions']

def mutate(runtime,case):
    r=copy.deepcopy(runtime);p=r
    for k in case['path'][:-1]:p=p[k]
    p[case['path'][-1]]=case['value'];return r

def test_form_shared_python_contract():
    suite=json.loads(SUITE.read_text());r=suite['runtime'];buttons=suite['buttons']
    assert not errors(r,buttons);assert not graphics_errors(r['surface'],r);assert not input_errors(r,buttons)
    for c in suite['cases']:
        s=initial(r);s['vars'].update(c.get('restore',{}));initialize(r,s)
        for a in c['actions']:
            if a=='@hide':cancel(r,s)
            else:s=action(r,s,a)
        assert list(s['vars'].values())==c['vars'],c['name']
        assert s['page']==c['page'],c['name']
        assert [text(r,s,q['id']) for q in r['form']['fields']]==c['text'],c['name']
    for c in suite['invalid']:assert errors(mutate(r,c),buttons),c['name']
    assert errors(r,buttons[:5])
    bad=copy.deepcopy(buttons);bad[0]['event']='button.sw3.short_press';assert errors(r,bad)
    # Legal user remapping: the editor binds actions, never SW1/SW2 identities.
    remap=copy.deepcopy(buttons);remap[4]['event'],remap[5]['event']=remap[5]['event'],remap[4]['event'];assert not errors(r,remap)
    s=initial(r);apply(r,s,'accept');s['vars']['choiceDraft']=20;s['vars']['amountDraft']=3
    assert not allowed(r,s,'accept') and not apply(r,s,'accept') and s['vars']['choice']==10
    # Existing formats without form remain untouched; dangling field references fail.
    legacy=copy.deepcopy(r);del legacy['form'];assert errors(legacy)
    for n in legacy['surface']['elements']:
        if 'field_bind' in n:del n['field_bind'];n['text']='legacy'
    assert not errors(legacy)
    bad=copy.deepcopy(r);bad['form']['fields'][0]['options'][0]['label']='a\0b';assert errors(bad,buttons)
    bad=copy.deepcopy(r);bad['dashboard']['title']='a\0b';assert errors(bad,buttons)
    # A newly constrained field does not authorize silent repair of old values.
    changed=copy.deepcopy(r);changed['transitions'].append({'from':'*','on':'external','set':{'choice':99}})
    assert not errors(changed,buttons)
    s=action(changed,action(changed,initial(changed),'form.accept'),'external')
    assert s['vars']['choice']==99 and s['vars']['editing']==0 and text(changed,s,'mode')=='—'

def test_form_shared_javascript_contract():
    script=r'''
import fs from 'node:fs';import assert from 'node:assert/strict';
import {initialApp,appAction,appNodeValue} from './pc/src/component-center/app-runtime.js';
import {formInitialize,formCancel,formAllowed,formApply} from './pc/src/component-center/app-form.js';
import {initialApp as initialMirror,appAction as actionMirror} from './pc/skills/petui/scripts/app-runtime.mjs';
import {previewFormCapabilityAccepts} from './pc/src/component-center/preview-lifecycle.js';
for(const [engine,key] of [['p4-app-form-v1','widgetAppForm'],['p4-app-form-v2','widgetAppFormV2'],['p4-app-form-v3','widgetAppFormV3']]){
 assert.equal(previewFormCapabilityAccepts(engine,{[key]:engine}),true);
 assert.equal(previewFormCapabilityAccepts(engine,{}),false);
 assert.equal(previewFormCapabilityAccepts(engine,{[key]:'wrong-engine'}),false);
 assert.equal(previewFormCapabilityAccepts(engine,{other:engine}),false);
}
for(const engine of [undefined,null,'','p4-app-form-v4','toString'])
 assert.equal(previewFormCapabilityAccepts(engine,{widgetAppForm:engine}),false);
const suite=JSON.parse(fs.readFileSync('./pc/skills/petui/contracts/app-form-v1.json')),r=suite.runtime;
for(const c of suite.cases){
 let s=initialApp(r);Object.assign(s.vars,c.restore);formInitialize(r,s);
 for(const a of c.actions)s=a==='@hide'?formCancel(r,s):appAction(r,s,a);
 assert.deepEqual(Object.values(s.vars),c.vars,c.name);assert.equal(s.page,c.page,c.name);
 assert.deepEqual(r.surface.elements.slice(0,2).map(n=>appNodeValue(n,s,{},r)),c.text,c.name);
 let mirror=initialMirror(r);Object.assign(mirror.vars,c.restore);formInitialize(r,mirror);
 for(const a of c.actions)mirror=a==='@hide'?formCancel(r,mirror):actionMirror(r,mirror,a);
 assert.deepEqual(mirror,s,c.name);
}
let s=initialApp(r);formApply(r,s,'accept');s.vars.choiceDraft=20;s.vars.amountDraft=3;
assert.equal(formAllowed(r,s,'accept'),false);assert.equal(formApply(r,s,'accept'),false);assert.equal(s.vars.choice,10);
'''
    subprocess.run(['node','--input-type=module','-e',script],cwd=ROOT,check=True)

def test_form_production_c_parser_and_editor(tmp_path):
    harness=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pet_p4_app_form_json.h"
#include "pet_p4_app_navigation.h"
#include "pet_p4_surface.h"
static void check(bool good,const char*name){if(!good){fprintf(stderr,"failed: %s\n",name);abort();}}
int main(int argc,char**argv){
 assert(argc==2);FILE*file=fopen(argv[1],"rb");assert(file);char raw[32768];size_t n=fread(raw,1,sizeof(raw)-1,file);fclose(file);raw[n]=0;
 cJSON*suite=cJSON_Parse(raw);assert(suite);const cJSON*r=pet_form_get(suite,"runtime"),*buttons=pet_form_get(suite,"buttons");
 pet_form_t form;assert(pet_form_parse(r,buttons,&form));pet_surface_t surface;assert(pet_surface_parse(pet_form_get(r,"surface"),&surface));assert(!strcmp(surface.nodes[0].field_bind,"mode"));
 assert(!pet_form_source_valid("{\"label\":\"a\\u0000b\"}"));
 assert(pet_form_source_valid("{\"label\":\"a\\\\u0000b\"}"));
 assert(pet_form_source_valid("{\"label\":\"a\\u0080b\"}"));
 char pages[4][24]={{"edit"},{"home"}};const cJSON*c;
 cJSON_ArrayForEach(c,pet_form_get(suite,"cases")){
   const char*name=pet_form_string(c,"name");int32_t values[8]={0};int i=0;const cJSON*v;
   cJSON_ArrayForEach(v,pet_form_get(r,"vars")){values[i]=cJSON_GetNumberValue(pet_form_get(v,"init"));const cJSON*saved=pet_form_get(pet_form_get(c,"restore"),v->string);if(saved)values[i]=saved->valueint;i++;}
   pet_form_initialize(&form,values);int8_t page=0;pet_nav_history_t history={0};
   cJSON_ArrayForEach(v,pet_form_get(c,"actions")){
     if(!strcmp(v->valuestring,"@hide")){pet_form_cancel(&form,values);continue;}
     int op=pet_form_operation(&form,v->valuestring);
     if(op>=0){pet_form_apply(&form,values,page,op);continue;}
     const cJSON*t;cJSON_ArrayForEach(t,pet_form_get(r,"transitions")){
       if(strcmp(pet_form_string(t,"on"),v->valuestring))continue;
       pet_nav_rule_t nav;assert(pet_nav_parse_rule(true,pages,2,t,&nav));
       if(!pet_nav_allowed(&history,page,&nav))continue;
       pet_form_cancel(&form,values);pet_nav_apply(&history,&page,&nav);break;
     }
   }
   for(i=0;i<6;i++)check(values[i]==cJSON_GetArrayItem(pet_form_get(c,"vars"),i)->valueint,name);
   check(!strcmp(pages[page],pet_form_string(c,"page")),name);
   for(i=0;i<form.count;i++){char value[121];pet_form_text(&form,values,i,value);check(!strcmp(value,cJSON_GetArrayItem(pet_form_get(c,"text"),i)->valuestring),name);}
 }
 cJSON_ArrayForEach(c,pet_form_get(suite,"invalid")){
   cJSON*copy=cJSON_Duplicate(r,true),*parent=copy;const cJSON*path=pet_form_get(c,"path");int count=cJSON_GetArraySize(path);
   for(int i=0;i<count-1;i++){const cJSON*k=cJSON_GetArrayItem(path,i);parent=cJSON_IsString(k)?cJSON_GetObjectItemCaseSensitive(parent,k->valuestring):cJSON_GetArrayItem(parent,k->valueint);assert(parent);}
   const cJSON*k=cJSON_GetArrayItem(path,count-1);cJSON*value=cJSON_Duplicate(pet_form_get(c,"value"),true);
   if(cJSON_IsString(k)){cJSON_DeleteItemFromObjectCaseSensitive(parent,k->valuestring);cJSON_AddItemToObject(parent,k->valuestring,value);}else cJSON_ReplaceItemInArray(parent,k->valueint,value);
   check(!pet_form_parse(copy,buttons,&form),pet_form_string(c,"name"));cJSON_Delete(copy);
 }
 assert(pet_form_parse(r,buttons,&form));int32_t values[8]={10,10,2,2,0,0};assert(pet_form_apply(&form,values,0,PET_FORM_ACCEPT));values[1]=20;values[3]=3;
 assert(!pet_form_apply(&form,values,0,PET_FORM_ACCEPT)&&values[0]==10&&values[2]==2);
 pet_form_t extreme={.enabled=true,.count=1,.focus_var=2,.active_var=3};
 extreme.fields[0]=(pet_form_field_t){.var=0,.draft_var=1,.min=-1000000000,.max=1000000000,.step=1000000000};
 int32_t limits[8]={-1000000000,-1000000000,0,0};assert(pet_form_apply(&extreme,limits,0,PET_FORM_ACCEPT));
 assert(pet_form_apply(&extreme,limits,0,PET_FORM_INCREASE)&&limits[1]==0);
 assert(pet_form_apply(&extreme,limits,0,PET_FORM_INCREASE)&&limits[1]==1000000000);
 assert(!pet_form_apply(&extreme,limits,0,PET_FORM_INCREASE));assert(pet_form_apply(&extreme,limits,0,PET_FORM_ACCEPT)&&limits[0]==1000000000);
 cJSON*missing=cJSON_Duplicate(buttons,true);cJSON_DeleteItemFromArray(missing,4);assert(!pet_form_parse(r,missing,&form));cJSON_Delete(missing);
 cJSON_Delete(suite);return 0;
}
'''
    source=tmp_path/'form.c';source.write_text(harness);exe=tmp_path/'form';vendor=ROOT/'firmware/tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-Wno-misleading-indentation','-fsanitize=address,undefined','-I'+str(ROOT/'firmware/main'),'-I'+str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe),str(SUITE)],check=True)

def test_form_runtime_and_install_integration():
    source=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text()
    assert 'pet_form_parse(root,form_buttons,&parsed->form)' in source and 'pet_form_source_valid(widget_json)' in source
    assert 'pet_form_apply(&g_runtime.form' in source and 'if(!visible)changed|=form_cancel(&g_runtime)' in source
    assert 'pet_form_initialize(&r->form' in source and 'pet_form_text(&g_runtime.form' in source
    dispatch=function(source,'dispatch_action_observed')
    assert dispatch.index('if(!runtime_transition_branch(')<dispatch.index('changed|=form_cancel(&g_runtime)')
    branch=function(source,'runtime_transition_branch')
    assert 'if(r->navigation){' in branch and 'if(!pet_nav_allowed(&r->navigation_history,r->current_page,&rule))return false;' in branch
    frontend_path=ROOT/'pc/src/ComponentCenter.jsx'
    frontend=frontend_path.read_text()
    # Preview feature checks remain strict; optional hardware adverts no longer
    # block native installation. Package validation still precedes transport.
    helper=ROOT/'pc/src/component-center/preview-lifecycle.js'
    assert 'export function previewFormCapabilityAccepts(' in helper.read_text()
    assert 'if(!previewFormCapabilityAccepts(component.appRuntime.form.engine,usb?.capabilities))' not in frontend
    native=(ROOT/'pc/src-tauri/src/usb_serial.rs').read_text()
    assert 'crate::app_component_program::validate(&widget_definition)?' in native
    assert 'widgetAppForm' in (ROOT/'pc/skills/petui/scripts/validate_generated_widget.py').read_text()

def test_form_full_python_validation_and_capability(tmp_path):
    from validate_generated_widget import validate_runtime,validate_buttons,DEFAULT_CAPABILITIES
    suite=json.loads(SUITE.read_text());r=suite['runtime'];buttons=suite['buttons'];failures=[]
    actions=validate_runtime(r,'tool',tmp_path,DEFAULT_CAPABILITIES,failures)
    validate_buttons(buttons,actions,DEFAULT_CAPABILITIES,r,failures)
    assert not failures
    old=dict(DEFAULT_CAPABILITIES);old.pop('widgetAppForm');failures=[]
    validate_runtime(r,'tool',tmp_path,old,failures)
    assert any('表单' in message for message in failures)
