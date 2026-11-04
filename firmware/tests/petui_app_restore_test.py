"""Exercise production save restoration, including additive package upgrades."""
from pathlib import Path
import subprocess
import pytest
from p4_catalog_membership_reboot_test import function
ROOT=Path(__file__).resolve().parents[1]

@pytest.mark.parametrize('typed',[False,True])
def test_restore_preserves_existing_values_and_rejects_corruption(tmp_path,typed):
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    begin=code.index('static bool restore_app_runtime_buffer(')
    end=code.index('\nstatic bool sync_side_runtime(',begin)
    helpers=''.join(function(code,n)for n in ['form3_owns','form3_string_limit','runtime_string_limit'])
    merge=code[code.index('static void merge_live_persistent_runtime('):code.index('\nstatic bool parse_runtime(')]
    harness=r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "cJSON.h"
#include "pet_p4_fruit_core.h"
#include "pet_p4_app_form_json.h"
#include "pet_p4_form_v3.h"
#define PET_APP_SAVE_BYTES 1024
#define MINIAPP_VAR_INT 0
#define MINIAPP_V2_STRING_MAX 64
typedef struct {char name[32],string_value[128];int type,int_min,int_max,int_value;} var_t;
typedef var_t miniapp_var_t;
typedef struct {int save_version,save_vars,var_count,state_count,current_state,page_count,current_page,save_states[6];bool active,save_page,slicing,quota_v3;pet_fruit_t fruit;pet_form_t form;pet_form_text_config_t form_text;pet_form3_t form3;char widget_id[48],states[6][24],pages[4][24];var_t vars[32];} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
#define miniapp_runtime_lock() ((void)0)
#define miniapp_runtime_unlock() ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static bool workspace_fail;
static unsigned workspace_live;
static void *heap_caps_malloc(size_t bytes,int caps){assert(caps==3);if(workspace_fail)return NULL;void *p=malloc(bytes);if(p)workspace_live++;return p;}
static void heap_caps_free(void *p){assert(p&&workspace_live);workspace_live--;free(p);}
static const char *saved;
static bool pet_app_store_read(const char *id,char *out){(void)id;if(!saved)return false;snprintf(out,PET_APP_SAVE_BYTES,"%s",saved);return true;}
static bool pet_app_store_read_cached(const char *id,char *out){return pet_app_store_read(id,out);}
static const char *json_string(const cJSON *j,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static int find_name(char names[][24],int n,const char *key){for(int i=0;i<n;i++)if(!strcmp(names[i],key))return i;return -1;}
static void copy_utf8(char *out,size_t n,const char *s){snprintf(out,n,"%s",s);}
static void refresh_view(miniapp_runtime_t *r){(void)r;}
static void finish_component_restore(miniapp_runtime_t *r){(void)r;}
static int find_var(const miniapp_runtime_t *r,const char *name){for(int i=0;i<r->var_count;i++)if(!strcmp(r->vars[i].name,name))return i;return -1;}
static void *no_alloc(size_t n){(void)n;assert(!"publish allocated");return NULL;}
static void *failed_alloc(size_t n){(void)n;return NULL;}
'''+helpers+code[begin:end]+merge+r'''
int main(void){
 miniapp_runtime_t r={.save_version=1,.save_vars=3,.var_count=2,.state_count=3,.save_states={0,2,2},.save_page=true,.page_count=2};
 strcpy(r.states[0],"idle");strcpy(r.states[1],"run");strcpy(r.states[2],"paused");strcpy(r.pages[0],"one");strcpy(r.pages[1],"two");
 r.vars[0]=(var_t){.name="count",.int_min=0,.int_max=1000000000};r.vars[1]=(var_t){.name="plant",.int_min=0,.int_max=1000000000,.int_value=2};
 saved="{\"version\":1,\"state\":\"run\",\"page\":\"two\",\"vars\":{\"count\":42}}";restore_app_runtime(&r);
 assert(r.vars[0].int_value==42&&r.vars[1].int_value==2&&r.current_state==2&&r.current_page==1);
 saved="{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":99,\"plant\":-1}}";assert(!restore_app_runtime(&r));assert(r.vars[0].int_value==42&&r.current_state==2);
 saved="{\"version\":2,\"state\":\"idle\",\"vars\":{\"count\":99}}";assert(!restore_app_runtime(&r));assert(r.vars[0].int_value==42);
 const char *invalid[]={
  "{\"version\":1,\"state\":\"missing\",\"vars\":{\"count\":99}}",
  "{\"version\":1,\"state\":\"idle\",\"page\":\"missing\",\"vars\":{\"count\":99}}",
  "{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":99,\"removed\":7}}",
  "{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":99,\"count\":100}}",
  "{\"version\":1,\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":99}}",
  "{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":99},\"unrecognized\":42}",
  "{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\\u0000other\":99}}"
 };
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){saved=invalid[i];assert(!restore_app_runtime(&r));assert(r.vars[0].int_value==42&&r.vars[1].int_value==2&&r.current_state==2&&r.current_page==1);}
 saved="{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":12,\"plant\":1}}";restore_app_runtime(&r);assert(r.vars[0].int_value==12&&r.vars[1].int_value==1&&r.current_state==0);
 miniapp_runtime_t fruit={.slicing=true};pet_fruit_init(&fruit.fruit,60,3,55);
 saved="{\"fruitVersion\":1,\"mode\":2,\"fruitRecords\":[1,20,300]}";restore_app_runtime(&fruit);
 assert(fruit.fruit.mode==2&&fruit.fruit.best==300&&fruit.fruit.records[1]==20&&fruit.fruit.phase==PET_FRUIT_IDLE);
 saved="{\"fruitVersion\":1,\"mode\":0,\"fruitRecords\":[100,-1,0]}";assert(!restore_app_runtime(&fruit));
 assert(fruit.fruit.mode==2&&fruit.fruit.records[0]==1);
 saved="{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":88}}";
 workspace_fail=true;assert(!restore_app_runtime(&r)&&r.vars[0].int_value==12&&!workspace_live);workspace_fail=false;
 cJSON_Hooks failed={.malloc_fn=failed_alloc};cJSON_InitHooks(&failed);
 assert(!restore_app_runtime(&r)&&r.vars[0].int_value==12);cJSON_InitHooks(NULL);
 // Live persisted projection is independent of JSON allocation. A slow
 // storage transaction must not replace the most recent input with old cache.
 g_runtime=r;g_runtime.active=true;g_runtime.vars[0].int_value=99;g_runtime.current_state=1;g_runtime.current_page=1;
 cJSON_Hooks hooks={.malloc_fn=no_alloc};cJSON_InitHooks(&hooks);
 merge_live_persistent_runtime(&r);
 assert(r.vars[0].int_value==99&&r.current_state==2&&r.current_page==1);
 strcpy(r.widget_id,"other");g_runtime.vars[0].int_value=123;merge_live_persistent_runtime(&r);assert(r.vars[0].int_value==99);
 cJSON_InitHooks(NULL);
#if PET_FORM_V2_ENABLED
 miniapp_runtime_t text=r;text.form_text.enabled=true;text.vars[1].type=1;strcpy(text.vars[1].string_value,"原始值");
 saved="{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":30,\"plant\":\"很长但有效的已保存文本😀\"}}";assert(restore_app_runtime(&text));
 assert(text.vars[0].int_value==30&&!strcmp(text.vars[1].string_value,"很长但有效的已保存文本😀"));
 saved="{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":99,\"plant\":\"a\\u0080b\"}}";assert(!restore_app_runtime(&text));
 assert(text.vars[0].int_value==30&&!strcmp(text.vars[1].string_value,"很长但有效的已保存文本😀"));
 saved="{\"version\":1,\"state\":\"idle\",\"vars\":{\"count\":99,\"plant\":55}}";assert(!restore_app_runtime(&text));assert(text.vars[0].int_value==30);
#endif

 // Use the real physical 128B string storage and profile/role helper. A
 // staged shorter schema must never receive a silently truncated live value.
 miniapp_runtime_t strings=r;strings.quota_v3=true;strings.active=true;
 strcpy(strings.widget_id,"strings");strings.vars[1].type=1;
 strings.form_text.enabled=false;strings.form3.enabled=false;
 char long_value[128];memset(long_value,'x',127);long_value[127]=0;
 char saved_text[512];snprintf(saved_text,sizeof(saved_text),"{\"version\":1,\"state\":\"idle\",\"vars\":{\"plant\":\"%s\"}}",long_value);saved=saved_text;
 assert(runtime_string_limit(&strings,1)==127&&restore_app_runtime(&strings));
 assert(strlen(strings.vars[1].string_value)==127);
 strings.quota_v3=false;strcpy(strings.vars[1].string_value,"prepared");
 assert(runtime_string_limit(&strings,1)==63&&!restore_app_runtime(&strings));
 assert(!strcmp(strings.vars[1].string_value,"prepared"));
 strings.quota_v3=true;strcpy(strings.vars[1].string_value,long_value);g_runtime=strings;
 miniapp_runtime_t narrow=strings;narrow.quota_v3=false;strcpy(narrow.vars[1].string_value,"prepared");
 merge_live_persistent_runtime(&narrow);assert(!strcmp(narrow.vars[1].string_value,"prepared"));
 miniapp_runtime_t wide=strings;strcpy(wide.vars[1].string_value,"");
 merge_live_persistent_runtime(&wide);assert(strlen(wide.vars[1].string_value)==127);
 // An enabled form3 narrows only its actual roles. The distinct unused string
 // remains quota3-sized, while field/draft use max_bytes and digits use 6.
 strings.var_count=11;strings.form3=(pet_form3_t){.enabled=true,.count=1,.page=0,.focus_var=3,.active_var=4,.key_var=5,.cursor_var=6,.buffer_var=7,.label_var=8,.candidates_var=9,.fields={{.id="name",.var=1,.draft_var=2,.mode=PET_FORM3_LATIN,.max_bytes=12}}};
 for(unsigned i=0;i<PET_FORM3_ACTIONS;i++)snprintf(strings.form3.actions[i],48,"form.op%u",i);
 for(int i=2;i<11;i++){snprintf(strings.vars[i].name,32,"role%d",i);strings.vars[i].type=(i==2||i>=7)?1:MINIAPP_VAR_INT;strings.vars[i].string_value[0]=0;}
 assert(pet_form3_valid(&strings.form3));
 assert(runtime_string_limit(&strings,1)==12&&runtime_string_limit(&strings,2)==12);
 assert(runtime_string_limit(&strings,7)==PET_FORM3_BUFFER_DIGITS&&runtime_string_limit(&strings,10)==127);
 strcpy(strings.vars[1].string_value,"too long live text");g_runtime=strings;
 narrow=strings;strcpy(narrow.vars[1].string_value,"prepared");
 merge_live_persistent_runtime(&narrow);assert(!strcmp(narrow.vars[1].string_value,"prepared"));
 strcpy(g_runtime.vars[1].string_value,"fits");merge_live_persistent_runtime(&narrow);assert(!strcmp(narrow.vars[1].string_value,"fits"));
#if PET_FORM_V2_ENABLED
 strings.form3.enabled=false;strings.form=(pet_form_t){.enabled=true,.count=1,.fields={{.var=1,.draft_var=2}}};
 strings.form_text=(pet_form_text_config_t){.enabled=true,.text_mask=1,.max_bytes={8}};
 assert(runtime_string_limit(&strings,1)==8&&runtime_string_limit(&strings,2)==8&&runtime_string_limit(&strings,10)==127);
 strcpy(strings.vars[1].string_value,"over eight");g_runtime=strings;narrow=strings;strcpy(narrow.vars[1].string_value,"staged");
 merge_live_persistent_runtime(&narrow);assert(!strcmp(narrow.vars[1].string_value,"staged"));
#endif
 assert(!workspace_live);
 return 0;
}
'''
    src=tmp_path/'restore.c';src.write_text(harness);exe=tmp_path/'restore';vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation',f'-DPET_FORM_V2_ENABLED={1 if typed else 0}','-DPET_FORM_V3_ENABLED=1','-fsanitize=address,undefined','-I',str(vendor),'-I',str(ROOT/'main'),str(src),str(ROOT/'main/pet_p4_fruit_core.c'),str(ROOT/'main/pet_p4_form_v2.c'),str(ROOT/'main/pet_p4_form_v3.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
