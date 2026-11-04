"""Actual input gate and acceptance stamp: no device, sound or grants."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]

def test_python_application_cooldown_is_admission_based_and_smoke_waits_for_feedback():
    import copy
    import sys
    sys.path.insert(0,str(ROOT.parent/'pc/skills/petui/scripts'))
    from bounded_app import initial,action,advance,smoke
    from input_policy import action_accepted,cooldown_allowed
    runtime={'engine':'p4-bounded-runtime-v4','initial_state':'idle','states':['idle'],
        'vars':{'count':{'type':'int','init':0}},'tick':[],
        'input':{'engine':'p4-input-v1','cooldown':{'add':250}},
        'transitions':[{'from':'*','on':'add','inc':{'count':1}},{'from':'*','on':'clear','set':{'count':0}}],
        'dashboard':{},'surface':{'elements':[{'type':'text','x':0,'y':0,'w':50,'h':30,'var_bind':'count','show':{'var':'count','min':2,'max':100}}]}}
    state=initial(runtime)
    for outcome in (False,'queue_full',None):
        assert action_accepted(runtime,copy.deepcopy(state),'add',outcome)==state
    state=action(runtime,state,'add');assert state['vars']['count']==1 and state['actionTimes']=={'add':0}
    assert action(runtime,state,'add') is state
    before=advance(runtime,state,249);assert not cooldown_allowed(runtime,before,'add')
    ready=advance(runtime,before,1);assert cooldown_allowed(runtime,ready,'add')
    assert action(runtime,ready,'add')['vars']['count']==2
    backwards=copy.deepcopy(state);backwards['ms']=-1;assert not cooldown_allowed(runtime,backwards,'add')
    assert action(runtime,state,'unknown').get('actionTimes')=={'add':0}
    result=smoke(runtime,[{'action':'add'},{'action':'clear'}])
    assert result['ok'] and result['visibleActions']==['add','clear'],result

def test_actual_program_gate_cannot_bypass_cooldown_or_stamp_a_refused_action(tmp_path):
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    code=r'''
#include "pet_p4_component_program.h"
#include "pet_p4_app_form.h"
#include "pet_p4_app_navigation.h"
#include "pet_p4_local_tools.h"
#include <assert.h>
#include <string.h>
typedef struct {int var_index,source_var,int_value,op;}miniapp_effect_t;
typedef struct {char action[48];int from_state,to_state,page_op,effect_count,else_to_state,else_page_op,else_effect_count,when_count;bool has_else;int when[4];pet_nav_rule_t navigation_rule;miniapp_effect_t effects[4];}miniapp_transition_t;
typedef struct {bool input_v1,program_suspended,navigation;pet_component_program_config_t program;
 char cooldown_actions[3][48];uint16_t cooldown_ms[3];uint64_t cooldown_at[3];uint8_t cooldown_used;
 pet_form_t form;pet_local_config_t local;pet_local_state_t local_state;pet_nav_history_t navigation_history;
 int transition_count,current_state,current_page,data_page_var;char data_source[48];struct{int int_value;}vars[8];miniapp_transition_t transitions[2];}miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
static bool conditions_hold(const miniapp_runtime_t*r,const int*conditions,int count){
 (void)r;(void)conditions;(void)count;assert(false);return false;
}
static int g_component_program;static bool g_component_program_initialized=true,queue_free=true;
static uint32_t g_image_revision=7;static uint64_t now;
static uint64_t esp_timer_get_time(void){return now*1000;}
static bool pet_component_program_session_can_enqueue(const int *p,uint32_t revision){assert(p==&g_component_program&&revision==7);return queue_free;}
static int form_operation(const miniapp_runtime_t *r,const char *a){(void)r;(void)a;return -1;}
static void form_values(const miniapp_runtime_t *r,int32_t values[8]){(void)r;(void)values;assert(false);}
typedef struct {int pages,page;}pet_p4_data_paging_t;
static pet_p4_data_paging_t pet_p4_widget_data_paging(const char *s,int p){(void)s;(void)p;return(pet_p4_data_paging_t){.pages=1};}
#define MINIAPP_EFFECT_INC 1
'''+function(text,'runtime_action_accepted')+function(text,'runtime_action_cooled')+function(text,'runtime_transition_branch')+function(text,'runtime_action_enabled')+r'''
int main(void){
 g_runtime.input_v1=true;g_runtime.program.enabled=true;g_runtime.program.action_count=1;
 strcpy(g_runtime.program.actions[0].name,"add");strcpy(g_runtime.cooldown_actions[0],"add");g_runtime.cooldown_ms[0]=250;
 assert(runtime_action_enabled(&g_runtime,"add"));runtime_action_accepted(&g_runtime,"add",0);
 assert(g_runtime.cooldown_used==1&&g_runtime.cooldown_at[0]==0);
 now=249;assert(!runtime_action_enabled(&g_runtime,"add"));now=250;assert(runtime_action_enabled(&g_runtime,"add"));
 queue_free=false;assert(!runtime_action_enabled(&g_runtime,"add"));assert(g_runtime.cooldown_at[0]==0);
 queue_free=true;runtime_action_accepted(&g_runtime,"add",250);now=249;assert(!runtime_action_enabled(&g_runtime,"add"));
 now=500;assert(runtime_action_enabled(&g_runtime,"add"));g_runtime.program_suspended=true;assert(!runtime_action_enabled(&g_runtime,"add"));
 g_runtime.program_suspended=false;g_component_program_initialized=false;assert(!runtime_action_enabled(&g_runtime,"add"));
 return 0;
}
'''
    source=tmp_path/'cooldown.c';source.write_text(code);exe=tmp_path/'cooldown'
    vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-error=deprecated-declarations','-fsanitize=address,undefined',
        '-DPET_COMPONENT_PROGRAM_ENABLED=1','-I',str(ROOT/'main'),'-I',str(vendor),str(source),str(ROOT/'main/pet_p4_local_tools.c'),
        str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
