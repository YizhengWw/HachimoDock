"""Shared native/Python/JS page-history contract and validator gates."""
import copy,json,subprocess,sys
from pathlib import Path
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[2]
SCRIPTS=ROOT/'pc/skills/petui/scripts'
sys.path.insert(0,str(SCRIPTS))
from app_navigation import errors
from bounded_app import initial,action
SUITE=ROOT/'pc/skills/petui/contracts/app-navigation-v1.json'

def test_navigation_vectors_python():
    suite=json.loads(SUITE.read_text());runtime=suite['runtime'];assert not errors(runtime)
    for c in suite['cases']:
        s=initial(runtime)
        for name in c['actions']:s=action(runtime,s,name)
        assert (s['page'],s.get('history',[]),s['vars']['hits'])==(c['page'],c['history'],c['hits']),c['name']
    for pointer,value in [('navigation',None),('host',True),('local',{}),('media',{}),('data',{})]:
        r=copy.deepcopy(runtime);r[pointer]=value;assert errors(r)
    for p in ({'back':False},{'push':'missing'},{'back':True,'push':'home'},['home'],None,{'unknown':'home'}):
        r=copy.deepcopy(runtime);r['transitions'][0]['page']=p;assert errors(r),p
    r=copy.deepcopy(runtime);del r['navigation'];assert errors(r)

def test_navigation_actual_c_core(tmp_path):
    harness=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pet_p4_app_navigation.h"
int main(int argc,char **argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);char input[16384];size_t n=fread(input,1,sizeof(input)-1,f);fclose(f);input[n]=0;
 cJSON*suite=cJSON_Parse(input);assert(suite);cJSON*r=cJSON_GetObjectItemCaseSensitive(suite,"runtime");
 char pages[4][24];cJSON*ps=cJSON_GetObjectItemCaseSensitive(r,"pages");
 for(int i=0;i<4;i++)snprintf(pages[i],24,"%s",cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(ps,i),"id")->valuestring);
 cJSON*rules=cJSON_GetObjectItemCaseSensitive(r,"transitions"),*c;
 cJSON_ArrayForEach(c,cJSON_GetObjectItemCaseSensitive(suite,"cases")){
   pet_nav_history_t history={0};int8_t page=0;int hits=0;cJSON*a;
   cJSON_ArrayForEach(a,cJSON_GetObjectItemCaseSensitive(c,"actions")){
     cJSON*t;cJSON_ArrayForEach(t,rules){
       if(strcmp(cJSON_GetObjectItemCaseSensitive(t,"on")->valuestring,a->valuestring))continue;
       pet_nav_rule_t rule;assert(pet_nav_parse_rule(true,pages,4,t,&rule));
       if(!pet_nav_allowed(&history,page,&rule))continue;
       assert(pet_nav_apply(&history,&page,&rule));
       hits+=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(t,"inc"),"hits"))==1;
       break;
     }
   }
   assert(!strcmp(pages[page],cJSON_GetObjectItemCaseSensitive(c,"page")->valuestring));
   assert(hits==cJSON_GetObjectItemCaseSensitive(c,"hits")->valueint);
   cJSON*expected=cJSON_GetObjectItemCaseSensitive(c,"history");assert(history.depth==cJSON_GetArraySize(expected));
   for(int i=0;i<history.depth;i++)assert(!strcmp(pages[history.pages[i]],cJSON_GetArrayItem(expected,i)->valuestring));
 }
 const char*bad[]={"{\"page\":{\"back\":false}}","{\"page\":null}","{\"page\":{\"push\":\"missing\"}}","{\"page\":{\"push\":\"home\",\"back\":true}}","{\"from_page\":\"missing\"}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){cJSON*t=cJSON_Parse(bad[i]);pet_nav_rule_t rule;assert(!pet_nav_parse_rule(true,pages,4,t,&rule));cJSON_Delete(t);}
 cJSON*t=cJSON_Parse("{\"page\":{\"push\":\"home\"}}");pet_nav_rule_t rule;assert(!pet_nav_parse_rule(false,pages,4,t,&rule));cJSON_Delete(t);
 cJSON_Delete(suite);return 0;
}
'''
    file=tmp_path/'navigation.c';file.write_text(harness);exe=tmp_path/'navigation';vendor=ROOT/'firmware/tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-fsanitize=address,undefined','-I'+str(ROOT/'firmware/main'),'-I'+str(vendor),str(file),str(vendor/'cJSON.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe),str(SUITE)],check=True)

def test_navigation_device_and_install_gates():
    paths=['firmware/main/pet_p4_protocol.c','pc/skills/petui/scripts/validate_generated_widget.py']
    assert all('widgetAppNavigation' in (ROOT/p).read_text() for p in paths)
    native=(ROOT/'pc/src-tauri/src/usb_serial.rs').read_text()
    assert 'crate::app_component_program::validate(&widget_definition)?' in native
    c=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text()
    assert 'pet_nav_parse_rule(runtime->navigation' in c
    branch=function(c,'runtime_transition_branch')
    # Only navigation-enabled runtimes consult history; legacy rules bypass it.
    assert 'if(r->navigation){' in branch
    assert branch.index('if(r->navigation){')<branch.index('if(!pet_nav_allowed(&r->navigation_history,r->current_page,&rule))return false;')<branch.index('return true;')
    assert 'if(!runtime_transition_branch(' in function(c,'dispatch_action_observed')
    assert 'if(runtime_transition_branch(' in function(c,'runtime_action_enabled')
