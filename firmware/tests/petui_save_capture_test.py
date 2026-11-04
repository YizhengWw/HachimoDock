"""Do not turn a transient allocation failure into a partial private save."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_actual_runtime_save_capture_is_all_or_nothing_under_oom(tmp_path):
    code = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    save = code[code.index('static void save_json_child('):code.index('static bool restore_app_runtime_buffer(')]
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "cJSON.h"
#define MINIAPP_MAX_VARS 8
#define MINIAPP_VAR_INT 0
#define PET_APP_SAVE_BYTES 1024
#define PET_WIDGET_TRACE_SCOPE(name,kind) int name __attribute__((unused))=0
static unsigned critical;
#define miniapp_runtime_lock() do{assert(!critical);critical=1;}while(0)
#define miniapp_runtime_unlock() do{assert(critical);critical=0;}while(0)
typedef struct {char name[32];int type;int32_t int_value;char string_value[64];} miniapp_var_t;
static struct {
 bool active,slicing,save_page;
 struct {unsigned records[3],mode;}fruit;
 uint16_t save_version;uint8_t save_vars,var_count,current_state,current_page;
 char widget_id[48],states[6][24],pages[4][24];miniapp_var_t vars[8];
} g_runtime;
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static bool workspace_fail;
static unsigned workspace_live;
static void *heap_caps_calloc(size_t n,size_t bytes,int caps){assert(!critical&&caps==3);if(workspace_fail)return NULL;void *p=calloc(n,bytes);if(p)workspace_live++;return p;}
static void heap_caps_free(void *p){assert(!critical&&p&&workspace_live);workspace_live--;free(p);}
static unsigned allocations,fail_at,live,writes;
static char private_save[1024];
static void *allocate(size_t n){allocations++;if(allocations==fail_at)return NULL;void *p=malloc(n);if(p)live++;return p;}
static void release(void *p){if(p){assert(live);live--;free(p);}}
static void copy_utf8(char *out,size_t n,const char *text){snprintf(out,n,"%s",text);}
static bool pet_app_store_update(const char *id,const char *json){assert(!critical&&!strcmp(id,"example"));writes++;snprintf(private_save,sizeof(private_save),"%s",json);return true;}
''' + save + r'''
int main(void){
 cJSON_Hooks hooks={allocate,release};cJSON_InitHooks(&hooks);
 g_runtime.active=true;strcpy(g_runtime.widget_id,"example");g_runtime.save_version=2;
 g_runtime.save_vars=3;g_runtime.var_count=2;g_runtime.save_page=true;
 strcpy(g_runtime.states[0],"paused");strcpy(g_runtime.pages[0],"detail");
 g_runtime.vars[0]=(miniapp_var_t){.name="count",.type=0,.int_value=42};
 g_runtime.vars[1]=(miniapp_var_t){.name="label",.type=1,.string_value="retained"};
 for(int kind=0;kind<2;kind++){
   g_runtime.slicing=kind==1;g_runtime.fruit.records[0]=1;g_runtime.fruit.records[1]=20;g_runtime.fruit.records[2]=300;g_runtime.fruit.mode=2;
   fail_at=0;allocations=writes=0;assert(save_app_runtime());assert(writes==1&&!allocations&&!live&&!critical);
   char complete[1024];strcpy(complete,private_save);
   allocations=writes=0;fail_at=1;strcpy(private_save,"original-complete-save");
   assert(save_app_runtime());assert(writes==1&&!allocations&&!live&&!critical&&!strcmp(private_save,complete));
   fail_at=0;
   assert(!workspace_live);
   workspace_fail=true;writes=0;strcpy(private_save,"original-complete-save");
   assert(!save_app_runtime()&&!writes&&!workspace_live&&!critical&&!strcmp(private_save,"original-complete-save"));
   workspace_fail=false;assert(save_app_runtime());
   cJSON *root=cJSON_Parse(private_save);assert(root);
   if(kind)assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root,"fruitRecords"))==3);
   else assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root,"vars"))==2);
   cJSON_Delete(root);assert(!live);
 }
 // Defensive legacy strings are fully escaped; a payload above the fixed
 // store budget fails without replacing the existing complete private save.
 g_runtime.slicing=false;g_runtime.save_vars=255;g_runtime.var_count=8;
 for(int i=0;i<8;i++){snprintf(g_runtime.vars[i].name,32,"v%d",i);g_runtime.vars[i].type=1;memset(g_runtime.vars[i].string_value,1,63);g_runtime.vars[i].string_value[63]=0;}
 writes=allocations=0;strcpy(private_save,"original-complete-save");
 assert(!save_app_runtime()&&!writes&&!allocations&&!strcmp(private_save,"original-complete-save"));
 return 0;
}
'''
    src = tmp_path / 'capture.c'
    src.write_text(source)
    exe = tmp_path / 'capture'
    vendor = ROOT / 'tests/vendor/cjson'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-error=deprecated-declarations', '-fsanitize=address,undefined',
                    '-I', str(vendor), str(src), str(vendor / 'cJSON.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
