"""Actual C persist parser + Python admission share worst-case vectors."""
import importlib
import json
import subprocess
import sys
from pathlib import Path
import pytest
ROOT=Path(__file__).resolve().parents[2]
SCRIPTS=ROOT/'pc/skills/petui/scripts'
sys.path.insert(0,str(SCRIPTS))
from store_budget import errors,estimate
from app_surface import persistence_errors
VECTORS=json.loads((SCRIPTS.parent/'contracts/store-budget-v1.json').read_text())['cases']

@pytest.fixture(scope='module')
def native(tmp_path_factory):
    directory=tmp_path_factory.mktemp('persist-budget')
    source=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text();a=source.index('static bool parse_save(');b=source.index('\n/* Stack-owned cJSON',a)
    harness=r'''
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_store_budget.h"
#define PET_FORM_V2_ENABLED 1
#define PET_FORM_V3_ENABLED 1
#define PET_P4_EVENTS 1
#define MINIAPP_V1_SAVE_VARS 8
#define MINIAPP_V2_SAVE_VARS 16
#define MINIAPP_MAX_VARS 32
#define MINIAPP_VAR_INT 0
#define MINIAPP_CAP(r,a,b,c) ((r)->quota_v3?(c):(r)->quota_v2?(b):(a))
typedef struct {char name[32];int type;} var_t;
typedef struct {int var;unsigned max_bytes;} field_t;
typedef struct {
 struct {bool app;} surface;bool quota_v2,quota_v3;var_t vars[32];int var_count,page_count,state_count;
 char states[16][24];int save_states[16];bool save_page;unsigned save_version;uint32_t save_vars;
 struct {unsigned count;field_t fields[4];} form;
 struct {bool enabled;unsigned text_mask,max_bytes[4];} form_text;
 struct {bool enabled;unsigned count;field_t fields[4];} form3;
 struct {struct {unsigned count;} alarms;} cext;
} miniapp_runtime_t;
static int find_var(const miniapp_runtime_t*r,const char*n){for(int i=0;i<r->var_count;i++)if(!strcmp(n,r->vars[i].name))return i;return -1;}
static int find_name(char names[][24],int count,const char*n){for(int i=0;i<count;i++)if(!strcmp(n,names[i]))return i;return -1;}
static bool object_keys_allowed(const cJSON*o,const char*const*keys,unsigned n){if(!cJSON_IsObject(o))return false;const cJSON*x;cJSON_ArrayForEach(x,o){bool found=false;for(unsigned i=0;i<n;i++)if(!strcmp(keys[i],x->string))found=true;if(!found)return false;}return true;}
'''
    harness='#include <stdint.h>\n'+harness+source[a:b]+r'''
static miniapp_runtime_t r;
int main(void){static char input[65536];size_t n=fread(input,1,sizeof(input)-1,stdin);input[n]=0;cJSON*root=cJSON_Parse(input);if(!root)return 2;
 const cJSON*q=cJSON_GetObjectItemCaseSensitive(root,"quota");r.quota_v3=cJSON_IsString(q)&&!strcmp(q->valuestring,"p4-quota-v3");r.quota_v2=cJSON_IsString(q)&&!strcmp(q->valuestring,"p4-quota-v2");r.surface.app=true;
 const cJSON*x;cJSON_ArrayForEach(x,cJSON_GetObjectItemCaseSensitive(root,"vars")){if(r.var_count==32)return 2;var_t*v=&r.vars[r.var_count++];snprintf(v->name,sizeof v->name,"%s",x->string);v->type=strcmp(cJSON_GetObjectItemCaseSensitive(x,"type")->valuestring,"int")!=0;}
 cJSON_ArrayForEach(x,cJSON_GetObjectItemCaseSensitive(root,"states")){if(r.state_count==16)return 2;snprintf(r.states[r.state_count++],24,"%s",x->valuestring);}
 r.page_count=cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root,"pages"));r.cext.alarms.count=cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root,"alarms"));
 const cJSON*f=cJSON_GetObjectItemCaseSensitive(root,"form");const cJSON*engine=cJSON_GetObjectItemCaseSensitive(f,"engine");
 if(cJSON_IsString(engine)){r.form_text.enabled=!strcmp(engine->valuestring,"p4-app-form-v2");r.form3.enabled=!strcmp(engine->valuestring,"p4-app-form-v3");unsigned i=0;cJSON_ArrayForEach(x,cJSON_GetObjectItemCaseSensitive(f,"fields")){if(i==4)return 2;int v=find_var(&r,cJSON_GetObjectItemCaseSensitive(x,"var")->valuestring);unsigned cap=cJSON_GetObjectItemCaseSensitive(x,"max_bytes")->valueint;r.form.fields[i].var=r.form3.fields[i].var=v;r.form_text.max_bytes[i]=r.form3.fields[i].max_bytes=cap;r.form_text.text_mask|=1U<<i;i++;}r.form.count=r.form3.count=i;}
 puts(parse_save(&r,cJSON_GetObjectItemCaseSensitive(root,"persist"))?"yes":"no");cJSON_Delete(root);return 0;}
'''
    (directory/'budget.c').write_text(harness)
    binary=directory/'budget'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-Wno-error=deprecated-declarations','-fsanitize=address,undefined',
                    '-I',str(ROOT/'firmware/main'),'-I',str(ROOT/'firmware/tests/vendor/cjson'),
                    str(directory/'budget.c'),str(ROOT/'firmware/tests/vendor/cjson/cJSON.c'),'-o',str(binary)],check=True)
    return binary

@pytest.mark.parametrize('case',VECTORS,ids=lambda c:c['name'])
def test_persist_subset_budget_all_ports(case,native):
    runtime=case['runtime'];ok=case['ok']
    assert (not errors(runtime))==ok
    assert (not persistence_errors(runtime))==ok
    result=subprocess.run([str(native)],input=json.dumps(runtime),text=True,capture_output=True,check=True)
    assert result.stdout.strip()==('yes' if ok else 'no') and not result.stderr
    if ok:
        worst={n: -1000000000 if runtime['vars'][n]['type']=='int' else '\x01'*127 for n in runtime['persist']['vars']}
        for f in runtime.get('form',{}).get('fields',[]):
            if f['var'] in worst:worst[f['var']]='\x01'*f['max_bytes']
        saved={'version':65535,'state':'s'*23,'vars':worst}
        if runtime['persist'].get('page'):saved['page']='p'*23
        if runtime.get('alarms'):saved['alarms']='a'*255
        used,limit=estimate(runtime)
        assert len(json.dumps(saved,separators=(',',':')).encode())<=used<=limit
