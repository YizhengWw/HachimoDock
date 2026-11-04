"""Production form parsers and final derived-writer admission; pure host evidence."""
import copy
import ctypes as C
import json
import subprocess
from pathlib import Path
import pytest
from p4_catalog_membership_reboot_test import function
ROOT=Path(__file__).resolve().parents[2]
FW=ROOT/'firmware';VENDOR=FW/'tests/vendor/cjson'

@pytest.fixture(scope='module')
def parsers(tmp_path_factory):
    out=tmp_path_factory.mktemp('form-branch-writers');src=out/'parse.c';lib=out/'parse.dylib'
    src.write_text('''#include "pet_p4_form_v3_json.h"
int check(int profile,const char*s,const char*b){cJSON*r=cJSON_Parse(s),*buttons=cJSON_Parse(b);pet_form_t f;pet_form_text_config_t t;pet_form3_t f3;int ok=profile==1?pet_form_parse(r,buttons,&f):profile==2?pet_form2_parse(r,buttons,&f,&t):pet_form3_parse(r,buttons,&f3);cJSON_Delete(r);cJSON_Delete(buttons);return ok;}
''')
    subprocess.run(['cc','-std=c11','-shared','-fPIC','-I'+str(FW/'main'),'-I'+str(VENDOR),str(src),str(FW/'main/pet_p4_form_v2.c'),str(FW/'main/pet_p4_form_v3.c'),str(VENDOR/'cJSON.c'),'-lm','-o',str(lib)],check=True)
    loaded=C.CDLL(str(lib));loaded.check.argtypes=[C.c_int,C.c_char_p,C.c_char_p];loaded.check.restype=C.c_int
    return loaded

@pytest.mark.parametrize('profile',[1,2,3])
def test_actual_profile_parser_checks_else_and_tick_bodies_with_original_masks(parsers,profile):
    suite=json.loads((ROOT/f'pc/skills/petui/contracts/app-form-v{profile}.json').read_text());r=suite['runtime'];buttons=json.dumps(suite['buttons']).encode()
    def accepted(value):return bool(parsers.check(profile,json.dumps(value).encode(),buttons))
    assert accepted(r)
    form=r['form'];committed={f['var']for f in form['fields']}
    roles={form['focus_var'],form['active_var']}|committed|{f['draft_var']for f in form['fields']}
    roles|={v for k,v in form.get('keyboard',{}).items()if k.endswith('_var')}
    for name in roles:
        for op in ('set','inc'):
            value='BYPASS' if r['vars'][name]['type']=='string' else 99
            for location in ('main','else','tick','then'):
                case=copy.deepcopy(r);case['logic']='p4-logic-v1'
                body={op:{name:value}}
                if location in ('main','else'):
                    rule={'from':'*','on':'extra.review'}
                    rule.update(body if location=='main' else {'when':{'var':form['active_var'],'op':'eq','value':1},'else':body})
                    case['transitions'].append(rule)
                else:
                    case['tick']=[{'every_ms':1000,**(body if location=='tick'else {'then':body})}]
                # v1 intentionally allows committed-field transition writers.
                expected=profile==1 and location in ('main','else') and name in committed
                assert accepted(case)==expected,(profile,name,op,location)
    # Legal non-form else remains accepted. v2's full eight slots can reuse
    # the now-independent vars of a removed optional field without adding vars.
    case=copy.deepcopy(r)
    if profile==2:
        name=case['form']['fields'].pop()['var'];case['surface']['elements']=[n for n in case['surface']['elements']if not n.get('field_bind')or n['field_bind']==case['form']['fields'][0]['id']]
    else:
        name='unrelated';case['vars'][name]={'type':'int','init':0}
    case['transitions'].append({'from':'*','on':'extra.review','else':{'inc':{name:1}}})
    assert accepted(case)


def test_production_longtext_writer_gate_rejects_form_program_host_and_duplicate_outputs(tmp_path):
    code=(FW/'main/pet_p4_miniapp.c').read_text()
    body=r'''
#define PET_P4_LONGTEXT 1
#define PET_FORM_V2_ENABLED 1
#define PET_FORM_V3_ENABLED 1
#define PET_VARIABLE_OWNERS_ENABLED 1
#define PET_COMPONENT_PROGRAM_ENABLED 1
#include <assert.h>
#include <string.h>
#include "pet_p4_surface.h"
#include "pet_p4_form_v3.h"
#include "pet_p4_variable_owners.h"
#include "pet_p4_component_program.h"
#define MINIAPP_VAR_INT 0
typedef struct{char name[32];int type;}var_t;
typedef struct{unsigned var_index;}effect_t;
typedef struct{unsigned effect_count,else_effect_count;effect_t effects[4];}miniapp_transition_t;
typedef struct{unsigned effect_count,completion_effect_count;effect_t effects[4],completion_effects[4];}miniapp_tick_t;
typedef struct{bool app_host;int var_count;var_t vars[16];pet_surface_t surface;pet_form_t form;pet_form_text_config_t form_text;pet_form3_t form3;pet_variable_owners_t variable_owners;pet_component_program_config_t program;unsigned transition_count,tick_count;miniapp_transition_t transitions[2];miniapp_tick_t ticks[2];}miniapp_runtime_t;
static int find_var(const miniapp_runtime_t*r,const char*n){for(int i=0;i<r->var_count;i++)if(!strcmp(r->vars[i].name,n))return i;return -1;}
'''+''.join(function(code,n)for n in ['runtime_host_variable','form3_owns','longtext_writers_valid'])+r'''
static miniapp_runtime_t r;
static void reset(void){memset(&r,0,sizeof(r));r.var_count=16;for(int i=0;i<16;i++)snprintf(r.vars[i].name,32,"v%d",i);r.surface.text_count=1;r.surface.texts[0].used=true;strcpy(r.surface.texts[0].count_var,"v0");memset(r.program.projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(r.program.projection.target));}
int main(void){
 reset();assert(longtext_writers_valid(&r));
 for(int role=0;role<4;role++){reset();r.form=(pet_form_t){.enabled=true,.count=1,.focus_var=1,.active_var=2,.fields={{.var=3,.draft_var=4}}};if(role==0)r.form.focus_var=0;if(role==1)r.form.active_var=0;if(role==2)r.form.fields[0].var=0;if(role==3)r.form.fields[0].draft_var=0;assert(!longtext_writers_valid(&r));}
 for(int role=0;role<2;role++){reset();r.form_text.enabled=true;r.form_text.index_var=role?1:0;r.form_text.label_var=role?0:1;assert(!longtext_writers_valid(&r));}
 for(int role=0;role<9;role++){
  reset();r.form3=(pet_form3_t){.enabled=true,.count=1,.focus_var=1,.active_var=2,.key_var=3,.cursor_var=4,.buffer_var=5,.label_var=6,.candidates_var=7,.fields={{.var=8,.draft_var=9}}};
  uint8_t*refs[]={&r.form3.focus_var,&r.form3.active_var,&r.form3.key_var,&r.form3.cursor_var,&r.form3.buffer_var,&r.form3.label_var,&r.form3.candidates_var,&r.form3.fields[0].var,&r.form3.fields[0].draft_var};*refs[role]=0;assert(!longtext_writers_valid(&r));
 }
 reset();r.program.enabled=true;r.program.projection.target[2]=0;assert(!longtext_writers_valid(&r));
 reset();r.program.enabled=true;r.program.projection.target[2]=1;r.program.action_count=1;r.program.actions[0].input[0]=0;assert(longtext_writers_valid(&r)); /* derived count is a read-only PROGRAM input */
 reset();r.app_host=true;assert(!longtext_writers_valid(&r));r.variable_owners.mixed=true;r.variable_owners.host_mask=1;assert(!longtext_writers_valid(&r));r.variable_owners.host_mask=2;assert(longtext_writers_valid(&r));
 reset();r.surface.text_count=2;r.surface.texts[1].used=true;strcpy(r.surface.texts[1].count_var,"v0");assert(!longtext_writers_valid(&r));strcpy(r.surface.texts[1].count_var,"v1");assert(longtext_writers_valid(&r));
 r.vars[0].type=1;assert(!longtext_writers_valid(&r));reset();r.surface.texts[0].count_var[0]=0;assert(longtext_writers_valid(&r));return 0;}
'''
    src=tmp_path/'writers.c';exe=tmp_path/'writers';src.write_text(body)
    subprocess.run(['cc','-std=c11','-include','stdio.h','-fsanitize=address,undefined','-I'+str(FW/'main'),'-I'+str(VENDOR),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    # Production hook runs after complete program/form declarations and before
    # publication/refresh, so no first-render derived mutation can bypass it.
    parse=code[code.index('static bool parse_runtime('):code.index('static bool apply_effects(')]
    assert parse.index('pet_component_program_parse(root,')<parse.index('if(!longtext_writers_valid(parsed))')<parse.index('parsed->active = true;')


@pytest.mark.parametrize('profile',[1,2,3])
def test_shared_branch_recipes_use_the_real_profile_parser(parsers,profile):
    import re
    suite=json.loads((ROOT/'pc/skills/petui/contracts/phase2-pc-contracts-v1.json').read_text())
    base=f'app-form-v{profile}'
    buttons=json.dumps(json.loads((ROOT/f'pc/skills/petui/contracts/{base}.json').read_text())['buttons']).encode()
    checked=0
    for case in suite['cases']:
        if case['base']!=base or not re.match(re.escape(base)+r'-(?:main|else|tick|then)-(?:set|inc)-',case['name']):continue
        runtime=copy.deepcopy(suite['bases'][base])
        for patch in case['patches']:
            parent=runtime
            for key in patch['path'][:-1]:parent=parent[key]
            parent[patch['path'][-1]]=copy.deepcopy(patch['value'])
        assert bool(parsers.check(profile,json.dumps(runtime).encode(),buttons))==case['ok'],case['name']
        checked+=1
    assert checked>=32,(base,checked)
