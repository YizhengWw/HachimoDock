"""Same-version hot update admission covers the whole old saved declaration."""
from pathlib import Path
from p4_catalog_membership_reboot_test import function
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_real_schema_gate_rejects_narrowing_before_concurrent_value_projection(tmp_path):
    code = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    gate = code[code.index('static bool hot_update_save_compatible('):code.index('static bool resource_repair_package_matches(')]
    merge = code[code.index('static void merge_live_persistent_runtime('):code.index('\nstatic bool parse_runtime(')]
    helpers=''.join(function(code,n)for n in ['form3_owns','form3_string_limit','runtime_string_limit'])
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PET_FORM_V3_ENABLED 1
#define PET_FORM_V2_ENABLED 1
#include "pet_p4_form_v3.h"
#define MINIAPP_VAR_INT 0
#define MINIAPP_V2_STRING_MAX 64
typedef struct {char name[32],string_value[128];int type,int_min,int_max,int_value;} miniapp_var_t;
typedef struct {unsigned records[3],mode;} fruit_t;
typedef struct {
  bool active,slicing,save_page,quota_v3;pet_form3_t form3;pet_form_t form;pet_form_text_config_t form_text;int save_version,save_vars,var_count,state_count,current_state,page_count,current_page,save_states[6];
  char widget_id[48],states[6][24],pages[4][24];miniapp_var_t vars[8];fruit_t fruit;
} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
static unsigned critical,locks;
#define miniapp_runtime_lock() do{assert(!critical);critical=1;locks++;}while(0)
#define miniapp_runtime_unlock() do{assert(critical);critical=0;}while(0)
static int find_var(const miniapp_runtime_t *r,const char *name){for(int i=0;i<r->var_count;i++)if(!strcmp(r->vars[i].name,name))return i;return -1;}
static int find_name(char names[][24],int count,const char *name){for(int i=0;i<count;i++)if(!strcmp(names[i],name))return i;return -1;}
static void copy_utf8(char *out,size_t n,const char *text){snprintf(out,n,"%s",text);}
static void refresh_view(miniapp_runtime_t *r){(void)r;assert(!critical);}
static void finish_component_restore(miniapp_runtime_t *r){(void)r;assert(!critical);}
static void pet_fruit_select_mode(fruit_t *f,unsigned mode){f->mode=mode;}
''' + helpers + gate + merge + r'''
int main(void){
 g_runtime=(miniapp_runtime_t){.active=true,.save_page=true,.save_version=1,.save_vars=1,.var_count=1,.state_count=3,.page_count=2,.save_states={0,2,2}};
 strcpy(g_runtime.widget_id,"journal");strcpy(g_runtime.states[0],"idle");strcpy(g_runtime.states[1],"running");strcpy(g_runtime.states[2],"paused");
 strcpy(g_runtime.pages[0],"home");strcpy(g_runtime.pages[1],"detail");
 g_runtime.vars[0]=(miniapp_var_t){.name="count",.int_min=0,.int_max=1000,.int_value=1};
 miniapp_runtime_t next=g_runtime;assert(hot_update_save_compatible(&next));
 // Value 1 currently fits either schema, but later legal input can reach 1000.
 next.vars[0].int_max=10;assert(!hot_update_save_compatible(&next));
 next=g_runtime;next.vars[0].int_min=1;assert(!hot_update_save_compatible(&next));
 next=g_runtime;next.vars[0].type=1;assert(!hot_update_save_compatible(&next));
 next=g_runtime;next.save_vars=0;assert(!hot_update_save_compatible(&next));
 next=g_runtime;strcpy(next.vars[0].name,"replacement");assert(!hot_update_save_compatible(&next));
 next=g_runtime;strcpy(next.states[1],"other");assert(!hot_update_save_compatible(&next));
 next=g_runtime;next.save_states[1]=0;assert(!hot_update_save_compatible(&next));
 next=g_runtime;next.save_page=false;assert(!hot_update_save_compatible(&next));
 next=g_runtime;next.page_count=1;assert(!hot_update_save_compatible(&next));
 // Additive declarations and wider ranges remain compatible. Names, not
 // numeric indices, define state/page mappings and saved variable identity.
 next=g_runtime;next.var_count=2;next.save_vars=3;
 next.vars[1]=(miniapp_var_t){.name="growth",.int_min=-100,.int_max=100,.int_value=5};
 next.vars[0].int_min=-10;next.vars[0].int_max=2000;
 next.state_count=4;strcpy(next.states[3],"complete");next.save_states[3]=3;
 next.page_count=3;strcpy(next.pages[2],"history");assert(hot_update_save_compatible(&next));
 g_runtime.vars[0].int_value=1000;g_runtime.current_state=1;g_runtime.current_page=1;
 merge_live_persistent_runtime(&next);
 assert(next.vars[0].int_value==1000&&next.vars[1].int_value==5&&next.current_state==2&&next.current_page==1);
 next=g_runtime;strcpy(next.states[0],"running");strcpy(next.states[1],"idle");next.save_states[0]=2;next.save_states[1]=1;
 strcpy(next.pages[0],"detail");strcpy(next.pages[1],"home");assert(hot_update_save_compatible(&next));
 merge_live_persistent_runtime(&next);assert(next.current_state==2&&next.current_page==0&&next.vars[0].int_value==1000);
 // This gate is specifically hot same-ID/same-version compatibility, not an
 // assertion of migration support or a restriction on unrelated components.
 next=g_runtime;strcpy(next.widget_id,"another");next.var_count=0;assert(hot_update_save_compatible(&next));

 // Ordinary quota3 strings preserve all 127 bytes; profile narrowing rejects
 // the declaration even while the prepared value is still short.
 g_runtime.current_state=0;g_runtime.vars[0].type=1;g_runtime.quota_v3=true;
 strcpy(g_runtime.vars[0].string_value,"short");next=g_runtime;next.quota_v3=false;
 assert(!hot_update_save_compatible(&next));
 next=g_runtime;assert(hot_update_save_compatible(&next));
 memset(g_runtime.vars[0].string_value,'x',127);g_runtime.vars[0].string_value[127]=0;
 merge_live_persistent_runtime(&next);assert(strlen(next.vars[0].string_value)==127);
 next=g_runtime;next.quota_v3=false;strcpy(next.vars[0].string_value,"prepared");
 merge_live_persistent_runtime(&next);assert(!strcmp(next.vars[0].string_value,"prepared"));
 // A form role uses its own max_bytes, unrelated strings retain the profile.
 g_runtime.form3=(pet_form3_t){.enabled=true,.count=1,.focus_var=2,.active_var=3,.key_var=4,.cursor_var=5,.buffer_var=6,.label_var=7,.candidates_var=8};
 g_runtime.form3.fields[0]=(pet_form3_field_t){.var=0,.draft_var=1,.max_bytes=63};
 strcpy(g_runtime.vars[0].string_value,"ok");next=g_runtime;next.form3.fields[0].max_bytes=12;
 assert(!hot_update_save_compatible(&next));
 g_runtime.form3.fields[0].max_bytes=12;next=g_runtime;next.form3.fields[0].max_bytes=63;
 assert(hot_update_save_compatible(&next));
 assert(runtime_string_limit(&g_runtime,0)==12&&runtime_string_limit(&g_runtime,6)==PET_FORM3_BUFFER_DIGITS);
 next.form3.fields[0].var=1;next.form3.fields[0].draft_var=9;assert(runtime_string_limit(&next,0)==127);

 g_runtime.form3.enabled=false;g_runtime.form=(pet_form_t){.enabled=true,.count=1,.fields={{.var=0,.draft_var=1}}};
 g_runtime.form_text=(pet_form_text_config_t){.enabled=true,.text_mask=1,.max_bytes={20}};
 next=g_runtime;next.form_text.max_bytes[0]=10;assert(!hot_update_save_compatible(&next));
 next.form_text.max_bytes[0]=40;assert(hot_update_save_compatible(&next));
 assert(runtime_string_limit(&g_runtime,0)==20);
 assert(!hot_update_save_compatible(NULL));assert(!critical&&locks>10);return 0;
}
'''
    src = tmp_path / 'schema.c'
    src.write_text(source)
    exe = tmp_path / 'schema'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(ROOT/'main'), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
