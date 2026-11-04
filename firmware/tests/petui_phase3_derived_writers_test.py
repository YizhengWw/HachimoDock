"""Actual C effect/transition/tick/program parsers + derived-writer gate.

Temporary native executable only; not the complete package installer or SDK.
"""
import copy
import json
import subprocess
from pathlib import Path
import pytest
from p4_catalog_membership_reboot_test import function
ROOT=Path(__file__).resolve().parents[2];FW=ROOT/'firmware';VENDOR=FW/'tests/vendor/cjson'
CONTRACT=ROOT/'pc/skills/petui/contracts/count-var-writers-v1.json'
SUITE=json.loads(CONTRACT.read_text())

@pytest.fixture(scope='module')
def checker(tmp_path_factory):
    out=tmp_path_factory.mktemp('derived-writers');src=out/'check.c';exe=out/'check'
    code=(FW/'main/pet_p4_miniapp.c').read_text()
    a=code.index('typedef enum {\n  MINIAPP_VAR_INT,');b=code.index('#if PET_P4_CELLS_V1\n/* p4-cells-v1 bounded ops',a)
    types=code[a:b]
    a=code.index('typedef struct {\n  int8_t from_state;');b=code.index('} miniapp_tick_t;',a)+len('} miniapp_tick_t;')
    types+=code[a:b]
    prefix=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cJSON.h"
#include "pet_p4_surface.h"
#include "pet_p4_app_navigation.h"
#include "pet_p4_app_form.h"
#include "pet_p4_game.h"
#include "pet_p4_component_program.h"
#include "pet_p4_variable_owners.h"
#define MINIAPP_VAR_NAME_MAX 32
#define MINIAPP_STRING_MAX 128
#define MINIAPP_V2_STRING_MAX 64
#define MINIAPP_V3_STRING_MAX 128
#define MINIAPP_INT_MIN (-1000000000)
#define MINIAPP_INT_MAX 1000000000
#define PET_P4_MINIAPP_ACTION_MAX 48
#define MINIAPP_MAX_EFFECTS 4
#define MINIAPP_MAX_CONDITIONS 2
#define MINIAPP_V1_MAX_VARS 8
#define MINIAPP_V2_MAX_VARS 16
#define MINIAPP_MAX_VARS 32
#define MINIAPP_V1_MAX_PAGES 4
#define MINIAPP_V2_MAX_PAGES 6
#define MINIAPP_MAX_PAGES 12
#define MINIAPP_V1_MAX_TRANSITIONS 24
#define MINIAPP_V2_MAX_TRANSITIONS 32
#define MINIAPP_MAX_TRANSITIONS 48
#define MINIAPP_V1_MAX_TICKS 8
#define MINIAPP_V2_MAX_TICKS 12
#define MINIAPP_MAX_TICKS 16
#define MINIAPP_TICK_MIN_MS 100
#define MINIAPP_CAP(r,a,b,c) ((r)->quota_v3?(c):(r)->quota_v2?(b):(a))
#define MINIAPP_EXT_OR_EVENT_ACTION(a)
'''
    runtime=r'''
typedef struct{
 bool app_host,app_expressions,logic,navigation,quota_v2,quota_v3,sfx;uint8_t sfx_count;
 char sfx_cue_id[8][16];uint8_t var_count,state_count,page_count,transition_count,tick_count;int8_t current_page;
 miniapp_var_t vars[32];char states[16][24],pages[12][24];
 miniapp_transition_t transitions[48];miniapp_tick_t ticks[16];pet_surface_t surface;pet_form_t form;
 pet_component_program_config_t program;pet_variable_owners_t variable_owners;
}miniapp_runtime_t;
'''
    names=['set_error','json_string','safe_id','copy_utf8','clamp_i64','find_name','find_var','object_keys_allowed','parse_vars','parse_name_array','parse_pages','parse_effect_object','parse_effects','json_bounded_int','parse_condition_item','parse_condition','find_sfx_cue','parse_sfx_ref','parse_page_op','parse_transitions','parse_ticks','runtime_host_variable','longtext_writers_valid']
    actual=''.join(function(code,n)for n in names)
    main=r'''
int main(void){
 char*raw=calloc(1,131073);miniapp_runtime_t*r=calloc(1,sizeof(*r));if(!raw||!r)return 2;
 size_t n=fread(raw,1,131072,stdin);if(!n||!feof(stdin)){free(raw);free(r);return 2;}
 cJSON*root=cJSON_Parse(raw);free(raw);char error[160]={0};bool parsed=root!=NULL;
 if(parsed){
  r->logic=!strcmp(json_string(root,"logic"),"p4-logic-v1");r->navigation=!strcmp(json_string(root,"navigation"),"p4-app-navigation-v1");
  r->quota_v2=!strcmp(json_string(root,"quota"),"p4-quota-v2");r->quota_v3=!strcmp(json_string(root,"quota"),"p4-quota-v3");r->app_expressions=true;
  r->app_host=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"host"));
  /* The shared three bases are non-form packages; form conflicts are tested
   * by the separate actual production-gate/profile-parser suite. */
  parsed=!cJSON_HasObjectItem(root,"form")&&parse_vars(r,cJSON_GetObjectItemCaseSensitive(root,"vars"),error,sizeof(error))
   &&parse_name_array(cJSON_GetObjectItemCaseSensitive(root,"states"),r->states,&r->state_count,16,"states",error,sizeof(error))
   &&parse_pages(r,cJSON_GetObjectItemCaseSensitive(root,"pages"),error,sizeof(error))
   &&pet_surface_parse(cJSON_GetObjectItemCaseSensitive(root,"surface"),&r->surface)
   &&parse_transitions(r,cJSON_GetObjectItemCaseSensitive(root,"transitions"),error,sizeof(error))
   &&parse_ticks(r,cJSON_GetObjectItemCaseSensitive(root,"tick"),error,sizeof(error))
   &&pet_variable_owners_parse(root,&r->variable_owners)
   &&pet_component_program_parse(root,&r->program);
 }
 if(!parsed)fprintf(stderr,"unrelated parser failure: %s\n",error);
 printf("%d\n",parsed?(longtext_writers_valid(r)?1:0):-1);cJSON_Delete(root);free(r);return 0;
}
'''
    src.write_text(prefix+types+runtime+actual+main)
    flags=['-DPET_P4_LONGTEXT=1','-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_VARIABLE_OWNERS_ENABLED=1']
    sources=['pet_p4_surface.c','pet_p4_canvas.c','pet_p4_component_program.c','pet_p4_program_core.c','pet_p4_variable_owners.c']
    subprocess.run(['cc','-std=c11','-Wno-deprecated-declarations','-fsanitize=address,undefined',*flags,'-I'+str(FW/'main'),'-I'+str(VENDOR),str(src),*[str(FW/'main'/s)for s in sources],str(VENDOR/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    def check(runtime):
        result=subprocess.run([str(exe)],input=json.dumps(runtime),text=True,capture_output=True,check=True)
        assert result.stdout.strip()in ('0','1'),result.stderr
        assert not result.stderr,result.stderr
        return result.stdout.strip()=='1'
    return check

@pytest.mark.parametrize('case',SUITE['cases'],ids=lambda c:c['name'])
def test_shared_guest_count_writer_admission(checker,case):
    runtime=copy.deepcopy(SUITE['bases'][case['base']])
    for patch in case['patches']:
        parent=runtime
        for key in patch['path'][:-1]:parent=parent[key]
        parent[patch['path'][-1]]=copy.deepcopy(patch['value'])
    assert checker(runtime)==case['ok']
