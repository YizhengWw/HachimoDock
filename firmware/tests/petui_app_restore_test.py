"""Exercise production save restoration, including additive package upgrades."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_restore_preserves_existing_values_and_rejects_corruption(tmp_path):
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    begin=code.index('static void restore_app_runtime(')
    end=code.index('\nstatic bool parse_runtime(',begin)
    harness=r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "cJSON.h"
#include "pet_p4_fruit_core.h"
#define PET_APP_SAVE_BYTES 1024
#define MINIAPP_VAR_INT 0
typedef struct {char name[32],string_value[64];int type,int_min,int_max,int_value;} var_t;
typedef struct {int save_version,save_vars,var_count,state_count,current_state,page_count,current_page,save_states[6];bool save_page,slicing;pet_fruit_t fruit;char widget_id[48],states[6][24],pages[4][24];var_t vars[8];} miniapp_runtime_t;
static const char *saved;
static bool pet_app_store_read(const char *id,char *out){(void)id;if(!saved)return false;snprintf(out,PET_APP_SAVE_BYTES,"%s",saved);return true;}
static const char *json_string(const cJSON *j,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static int find_name(char names[][24],int n,const char *key){for(int i=0;i<n;i++)if(!strcmp(names[i],key))return i;return -1;}
static void copy_utf8(char *out,size_t n,const char *s){snprintf(out,n,"%s",s);}
static void refresh_view(miniapp_runtime_t *r){(void)r;}
'''+code[begin:end]+r'''
int main(void){
 miniapp_runtime_t r={.save_version=1,.save_vars=3,.var_count=2,.state_count=3,.save_states={0,2,2},.save_page=true,.page_count=2};
 strcpy(r.states[0],"idle");strcpy(r.states[1],"run");strcpy(r.states[2],"paused");strcpy(r.pages[0],"one");strcpy(r.pages[1],"two");
 r.vars[0]=(var_t){.name="count",.int_min=0,.int_max=1000000000};r.vars[1]=(var_t){.name="plant",.int_min=0,.int_max=1000000000,.int_value=2};
 saved="{\"version\":1,\"state\":\"run\",\"page\":\"two\",\"vars\":{\"count\":42}}";restore_app_runtime(&r);
 assert(r.vars[0].int_value==42&&r.vars[1].int_value==2&&r.current_state==2&&r.current_page==1);
 saved="{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":99,\"plant\":-1}}";restore_app_runtime(&r);assert(r.vars[0].int_value==42&&r.current_state==2);
 saved="{\"version\":2,\"state\":\"idle\",\"vars\":{\"count\":99}}";restore_app_runtime(&r);assert(r.vars[0].int_value==42);
 saved="{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":12,\"plant\":1}}";restore_app_runtime(&r);assert(r.vars[0].int_value==12&&r.vars[1].int_value==1&&r.current_state==0);
 miniapp_runtime_t fruit={.slicing=true};pet_fruit_init(&fruit.fruit,60,3,55);
 saved="{\"fruitVersion\":1,\"mode\":2,\"fruitRecords\":[1,20,300]}";restore_app_runtime(&fruit);
 assert(fruit.fruit.mode==2&&fruit.fruit.best==300&&fruit.fruit.records[1]==20&&fruit.fruit.phase==PET_FRUIT_IDLE);
 saved="{\"fruitVersion\":1,\"mode\":0,\"fruitRecords\":[100,-1,0]}";restore_app_runtime(&fruit);
 assert(fruit.fruit.mode==2&&fruit.fruit.records[0]==1);
 return 0;
}
'''
    src=tmp_path/'restore.c';src.write_text(harness);exe=tmp_path/'restore';vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-fsanitize=address,undefined','-I',str(vendor),'-I',str(ROOT/'main'),str(src),str(ROOT/'main/pet_p4_fruit_core.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
