"""Real data-only C owner/program validators and actual dispatch input capture.

No device, filesystem mutation, grants, models, microphone or speaker.
"""
from pathlib import Path
import copy
import json
import subprocess
import sys
import pytest
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]
SKILL=ROOT.parent/'pc/skills/petui'
sys.path.insert(0,str(SKILL/'scripts'))
SUITE=json.loads((SKILL/'contracts/variable-owners-v1.json').read_text())
VENDOR=ROOT/'tests/vendor/cjson'


def patched(value,patch):
    value=copy.deepcopy(value);at=value;parts=patch['path'][1:].split('/')
    for k in parts[:-1]:at=at[int(k)] if isinstance(at,list) else at[k]
    at[int(parts[-1]) if isinstance(at,list) else parts[-1]]=patch['value']
    return value


def compile_c(tmp_path,code,enabled=1):
    source=tmp_path/f'owners-{enabled}.c';source.write_text(code);binary=tmp_path/f'owners-{enabled}'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-Wno-error=deprecated-declarations','-fsanitize=address,undefined',
        '-DPET_COMPONENT_PROGRAM_ENABLED=1',f'-DPET_VARIABLE_OWNERS_ENABLED={enabled}',
        '-I',str(ROOT/'main'),'-I',str(VENDOR),str(source),
        str(ROOT/'main/pet_p4_component_program.c'),str(ROOT/'main/pet_p4_program_core.c'),str(ROOT/'main/pet_p4_variable_owners.c'),
        str(VENDOR/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    return binary


@pytest.fixture(scope='module')
def validators(tmp_path_factory):
    code=r'''
#include "pet_p4_variable_owners.h"
#include "pet_p4_component_program.h"
#include <stdio.h>
#include <stdlib.h>
static pet_component_program_config_t program;
int main(void){
 char input[65536];size_t n=fread(input,1,sizeof(input)-1,stdin);input[n]=0;
 cJSON *root=cJSON_ParseWithOpts(input,NULL,true);pet_variable_owners_t p={0};
 bool ok=root&&pet_variable_owners_parse(root,&p),valid=root&&pet_component_program_parse(root,&program);
 printf("{\"ok\":%s,\"program\":%s,\"mixed\":%s,\"mask\":%u}\n",ok?"true":"false",valid?"true":"false",p.mixed?"true":"false",p.host_mask);
 cJSON_Delete(root);return 0;
}
'''
    folder=tmp_path_factory.mktemp('variable-owners')
    bins={n:compile_c(folder,code,n) for n in (0,1)}
    def run(runtime,enabled=1):
        result=subprocess.run([str(bins[enabled])],input=json.dumps(runtime),text=True,capture_output=True)
        assert result.returncode==0 and not result.stderr,result.stderr
        return json.loads(result.stdout)
    return run


def test_shared_native_program_owner_plan_and_python_rejections(validators):
    from variable_owners import parse,errors
    from component_program import errors as program_errors
    r=SUITE['runtime'];assert parse(r)=={'mixed':True,'host':{'step'}}
    assert validators(r)=={'ok':True,'program':True,'mixed':True,'mask':2}
    assert not program_errors(r)
    for p in SUITE['reject']:
        bad=patched(r,p);native=validators(bad)
        assert not native['ok'] and not native['program'],p
        assert errors(bad) and program_errors(bad),p


def test_default_off_and_legacy_host_contract_are_not_silently_opened(validators):
    r=SUITE['runtime'];assert not validators(r,0)['ok'] and not validators(r,0)['program']
    old=copy.deepcopy(r);del old['ownership'];del old['host'];del old['host_context']
    assert validators(old,0)['ok'] and validators(old,0)['program'] and not validators(old,0)['mixed']
    old['host']=True
    assert validators(old,0)['mask']==3 and not validators(old,0)['program']
    assert not validators(old,1)['program']  # Legacy host still exclusively owns variables/actions.


def test_actual_pointer_guards_distinguish_native_actions_host_inputs_and_geometry(tmp_path):
    source=(ROOT/'main/pet_p4_miniapp.c').read_text()
    helpers='\n'.join(function(source,n) for n in ('runtime_host_variable','runtime_host_action','runtime_action_host_dependent','pointer_action_host_dependent','host_pointer_capture'))
    code=r'''
#include "pet_p4_component_program.h"
#include "pet_p4_variable_owners.h"
#include "pet_p4_host_observation.h"
#include <assert.h>
#include <string.h>
#define PET_HOST_COLLECTION_ENABLED 1
typedef struct {unsigned host_mask,collection_slot;char show_var[32];} pet_surface_node_t;
typedef struct {bool enabled;char offset_var[32];} viewport_t;
typedef struct {bool app_host;pet_variable_owners_t variable_owners;pet_component_program_config_t program;
 struct {uint8_t count,viewport_count;pet_surface_node_t nodes[2];viewport_t viewport,viewports[2];}surface;} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;static int g_pointer_host,captures,legacy;
static int find_var(const miniapp_runtime_t *r,const char *name){(void)r;return !strcmp(name,"step")?1:!strcmp(name,"count")?0:-1;}
static int64_t esp_timer_get_time(void){return 100000;}
static void pet_app_host_render_capture_observed(const char *id,int *frame,pet_host_observation_t *out,uint64_t now){assert(!strcmp(id,"owned-case")&&now==100);*frame=++captures;out->valid=true;out->variable_epoch=captures;}
static void pet_app_host_observe(const char *id,pet_host_observation_t *out){assert(!strcmp(id,"owned-case"));legacy++;out->valid=true;}
'''
    # The helper only needs the real widget ID in addition to the bounded model.
    code=code.replace('bool app_host;pet_variable_owners_t','bool app_host;char widget_id[48];pet_variable_owners_t')
    code+=helpers+r'''
int main(void){
 cJSON *root=cJSON_Parse(DEFINITION);assert(root);
 assert(pet_variable_owners_parse(root,&g_runtime.variable_owners));assert(pet_component_program_parse(root,&g_runtime.program));cJSON_Delete(root);
 g_runtime.app_host=true;strcpy(g_runtime.widget_id,"owned-case");g_runtime.surface.count=2;
 assert(!pointer_action_host_dependent("subtract",0));assert(pointer_action_host_dependent("add",0));assert(pointer_action_host_dependent("host.increment",0));
 strcpy(g_runtime.surface.nodes[0].show_var,"count");assert(!pointer_action_host_dependent("subtract",0));
 strcpy(g_runtime.surface.nodes[0].show_var,"step");assert(pointer_action_host_dependent("subtract",0));
 assert(!pointer_action_host_dependent("subtract",1));g_runtime.surface.nodes[1].host_mask=1;assert(pointer_action_host_dependent("subtract",1));
 g_runtime.surface.nodes[1].host_mask=0;g_runtime.surface.nodes[1].collection_slot=1;assert(pointer_action_host_dependent("subtract",1));
 g_runtime.surface.viewport.enabled=true;strcpy(g_runtime.surface.viewport.offset_var,"step");assert(pointer_action_host_dependent("subtract",-1));
 pet_host_observation_t down={0},up={0};host_pointer_capture(&down);host_pointer_capture(&up);
 assert(captures==2&&!legacy&&down.valid&&up.valid&&!pet_host_observation_equal(&down,&up));
 g_runtime.variable_owners.mixed=false;host_pointer_capture(&up);assert(legacy==1&&captures==2);
 g_runtime.app_host=false;host_pointer_capture(&up);assert(!up.valid);
 return 0;
}
'''
    code=code.replace('DEFINITION',json.dumps(json.dumps(SUITE['runtime'],separators=(',',':'))))
    subprocess.run([str(compile_c(tmp_path,code))],check=True)


def test_actual_program_inputs_capture_host_once_and_never_overwrite_native(tmp_path):
    source=(ROOT/'main/pet_p4_miniapp.c').read_text()
    helpers='\n'.join(function(source,name) for name in ('runtime_host_variable','component_program_values','component_program_action_values'))
    code=r'''
#include "pet_p4_component_program.h"
#include "pet_p4_variable_owners.h"
#include "pet_p4_host_observation.h"
#include <assert.h>
#include <string.h>
#define MINIAPP_VAR_INT 0
typedef struct {int type;char name[32];int32_t int_value,int_min,int_max;} var_t;
typedef struct {bool app_host;uint8_t var_count;char widget_id[48];var_t vars[8];
 pet_variable_owners_t variable_owners;pet_component_program_config_t program;} miniapp_runtime_t;
static char g_program_input_names[4][32];static int32_t g_program_input_values[8];
static bool fresh=true;static int captures,observed_captures;static int32_t remote_value=5;
static bool pet_app_host_owned_integer_snapshot(const char *id,const char names[][32],unsigned count,int32_t out[8],uint64_t now){
 assert(!strcmp(id,"owned-case")&&count==1&&!strcmp(names[0],"step")&&now==100);
 captures++;if(!fresh)return false;out[0]=remote_value;return true;
}
static bool pet_app_host_integer_snapshot_observed(const char *id,const char names[][32],unsigned count,int32_t out[8],const pet_host_observation_t *observed,uint64_t now){
 assert(observed&&observed->valid);observed_captures++;return pet_app_host_owned_integer_snapshot(id,names,count,out,now);
}
'''+helpers+r'''
int main(void){
 cJSON *root=cJSON_Parse(DEFINITION);assert(root);miniapp_runtime_t r={0};
 assert(pet_variable_owners_parse(root,&r.variable_owners)&&pet_component_program_parse(root,&r.program));cJSON_Delete(root);
 r.app_host=true;r.var_count=2;strcpy(r.widget_id,"owned-case");
 strcpy(r.vars[0].name,"count");strcpy(r.vars[1].name,"step");
 r.vars[0].int_value=7;r.vars[0].int_min=-1000000000;r.vars[0].int_max=1000000000;
 r.vars[1].int_value=2;r.vars[1].int_min=1;r.vars[1].int_max=10;
 int32_t values[8];pet_program_event_t event;
 assert(component_program_action_values(&r,"add",values,NULL,100));
 assert(captures==1&&values[0]==7&&values[1]==5&&r.vars[1].int_value==2);
 assert(pet_component_program_event(&r.program,"add",values,&event)&&event.words[0]==5);
 remote_value=9;assert(event.words[0]==5); // Already captured words cannot follow another reply.
 fresh=false;assert(!component_program_action_values(&r,"add",values,NULL,100)&&captures==2);
 assert(component_program_action_values(&r,"subtract",values,NULL,100)&&captures==2);
 assert(pet_component_program_event(&r.program,"subtract",values,&event)&&event.words[0]==-1);
 fresh=true;remote_value=999;pet_host_observation_t observed={.valid=true};
 assert(component_program_action_values(&r,"add",values,&observed,100)&&observed_captures==1&&values[1]==10);
 // Duplicate references share the same one-epoch capture and native word.
 r.program.actions[0].event.count=4;r.program.actions[0].input[1]=1;r.program.actions[0].input[2]=0;r.program.actions[0].input[3]=1;
 int before=captures;remote_value=-99;
 assert(component_program_action_values(&r,"add",values,NULL,100)&&captures==before+1&&values[0]==7&&values[1]==1);
 assert(pet_component_program_event(&r.program,"add",values,&event)&&event.words[0]==1&&event.words[1]==1&&event.words[2]==7&&event.words[3]==1);
 assert(r.vars[0].int_value==7&&r.vars[1].int_value==2);return 0;
}
'''
    code=code.replace('DEFINITION',json.dumps(json.dumps(SUITE['runtime'])))
    binary=compile_c(tmp_path,code)
    subprocess.run([str(binary)],check=True)


@pytest.mark.parametrize('enabled',[0,1])
def test_actual_confirmation_latches_host_publication_without_blocking_native_context(tmp_path,enabled):
    source=(ROOT/'main/pet_p4_miniapp.c').read_text()
    code=r'''
#include "pet_p4_variable_owners.h"
#include "pet_p4_host_observation.h"
#include <assert.h>
#include <stdio.h>
static struct {int current_state,current_page;char widget_id[48];pet_variable_owners_t variable_owners;}g_runtime;
static uint32_t g_image_revision=7;
static char g_confirm_action[48],g_confirm_prompt[121];
static uint64_t g_confirm_until=1000,g_confirm_context;
#if PET_VARIABLE_OWNERS_ENABLED
static pet_host_observation_t g_confirm_host_observation;
static bool fresh=true;static unsigned checks;
static bool pet_app_host_owned_observation_current(const char *id,const pet_host_observation_t *obs,uint64_t now){
 assert(!strcmp(id,"owned-case")&&obs==&g_confirm_host_observation&&now<1000);checks++;return fresh;
}
#endif
static void copy_utf8(char *out,size_t size,const char *in){snprintf(out,size,"%s",in);}
'''+function(source,'confirmation_locked')+r'''
int main(void){
 strcpy(g_runtime.widget_id,"owned-case");strcpy(g_confirm_action,"add");strcpy(g_confirm_prompt,"Confirm");
 g_confirm_context=(uint64_t)7<<16;char prompt[121];
 assert(confirmation_locked(prompt,100)&&!strcmp(prompt,"Confirm"));
#if PET_VARIABLE_OWNERS_ENABLED
 g_runtime.variable_owners.mixed=true;g_confirm_host_observation.valid=true;
 assert(confirmation_locked(NULL,200)&&checks==1);
 fresh=false;assert(!confirmation_locked(NULL,201)&&!g_confirm_action[0]&&checks==2);
 strcpy(g_confirm_action,"subtract");g_confirm_host_observation.valid=false;
 assert(confirmation_locked(NULL,202)&&checks==2); // Pure native confirmation does not wait on HOST.
#endif
 g_image_revision++;assert(!confirmation_locked(NULL,300)&&!g_confirm_action[0]);
 g_confirm_context=(uint64_t)g_image_revision<<16;strcpy(g_confirm_action,"add");
 assert(!confirmation_locked(NULL,1000)&&!g_confirm_action[0]);return 0;
}
'''
    subprocess.run([str(compile_c(tmp_path,code,enabled))],check=True)
