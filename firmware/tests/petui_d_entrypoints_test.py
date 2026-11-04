"""Production D parser, writeback/restore and output-drain entrypoints; no device."""
import json
import re
import subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
FW=ROOT/'firmware'
VENDOR=FW/'tests/vendor/cjson'

def function(code,name):
    start=re.search(r'^static [^\n]*\b'+name+r'\(',code,re.M).start()
    end=code.index('\n}',start)+2
    return code[start:end]+'\n'

def run_c(tmp_path,body,extra=()):
    src=tmp_path/'entry.c';exe=tmp_path/'entry';src.write_text(body)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Wno-misleading-indentation','-fsanitize=address,undefined','-I'+str(FW/'main'),'-I'+str(VENDOR),str(src),str(VENDOR/'cJSON.c'),*[str(x) for x in extra],'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=20)

def test_actual_form3_writeback_and_restore_preserve_unowned_quota3_string(tmp_path):
    code=(FW/'main/pet_p4_miniapp.c').read_text()
    actual=''.join(function(code,n) for n in ['form3_owns','form3_string_limit','form3_values','form3_write','restore_app_runtime_buffer'])
    harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_app_form_json.h"
#include "pet_p4_form_v3.h"
#include "pet_p4_fruit_core.h"
#define PET_APP_SAVE_BYTES 4096
#define MINIAPP_VAR_INT 0
#define MINIAPP_V2_STRING_MAX 64
#define PET_FORM_V3_ENABLED 1
#define PET_FORM_V2_ENABLED 0
#define PET_P4_EVENTS 0
typedef struct{char name[32],string_value[128];int type,int_min,int_max,int_value;} var_t;
typedef struct{bool slicing,quota_v3,save_page;int save_version;uint32_t save_vars;int var_count,state_count,current_state,page_count,current_page,save_states[16];char widget_id[48],states[16][24],pages[12][24];var_t vars[32];pet_form3_t form3;pet_fruit_t fruit;} miniapp_runtime_t;
static uint64_t g_viewport_epoch;static int g_pressed_node;
static const char *saved;
static bool pet_app_store_read(const char*id,char*out){(void)id;strcpy(out,saved);return true;}
static bool pet_app_store_read_cached(const char*id,char*out){return pet_app_store_read(id,out);}
static void copy_utf8(char*out,size_t n,const char*s){snprintf(out,n,"%s",s);}
static const char*json_string(const cJSON*j,const char*k){const cJSON*v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static int find_var(const miniapp_runtime_t*r,const char*n){for(int i=0;i<r->var_count;i++)if(!strcmp(r->vars[i].name,n))return i;return -1;}
static int find_name(char names[][24],int n,const char*k){for(int i=0;i<n;i++)if(!strcmp(names[i],k))return i;return -1;}
static void refresh_view(miniapp_runtime_t*r){(void)r;}
'''+actual+r'''
int main(void){
 miniapp_runtime_t r={.quota_v3=true,.save_version=1,.save_vars=(1U<<0)|(1U<<31),.var_count=32,.state_count=1};strcpy(r.states[0],"idle");
 r.form3=(pet_form3_t){.enabled=true,.count=1,.focus_var=2,.active_var=3,.key_var=4,.cursor_var=5,.buffer_var=6,.label_var=7,.candidates_var=8,.fields={{.var=0,.draft_var=1,.max_bytes=6}}};
 for(unsigned i=0;i<32;i++){snprintf(r.vars[i].name,32,"v%u",i);r.vars[i].type=i<2||i>=6?1:0;r.vars[i].int_min=-1000000000;r.vars[i].int_max=1000000000;}
 memset(r.vars[31].string_value,'x',127);r.vars[31].string_value[127]=0;strcpy(r.vars[0].string_value,"ok");strcpy(r.vars[1].string_value,"ok");
 pet_form3_value_t values[PET_FORM3_VARS];form3_values(&r,values);assert(!values[31].text[0]);strcpy(values[31].text,"corrupt");strcpy(values[0].text,"new");assert(form3_write(&r,values));assert(strlen(r.vars[31].string_value)==127&&!strcmp(r.vars[0].string_value,"new"));
 char raw[4096],payload[4096];snprintf(payload,sizeof(payload),"{\"version\":1,\"state\":\"idle\",\"vars\":{\"v0\":\"valid\",\"v31\":\"%s\"}}",r.vars[31].string_value);saved=payload;
 assert(restore_app_runtime_buffer(&r,false,raw));assert(strlen(r.vars[31].string_value)==127&&!strcmp(r.vars[0].string_value,"valid"));
 saved="{\"version\":1,\"state\":\"idle\",\"vars\":{\"v0\":\"toolong\",\"v31\":\"changed\"}}";assert(!restore_app_runtime_buffer(&r,false,raw));assert(strlen(r.vars[31].string_value)==127&&!strcmp(r.vars[0].string_value,"valid"));
 r.quota_v3=false;saved=payload;assert(!restore_app_runtime_buffer(&r,false,raw));assert(strlen(r.vars[31].string_value)==127);
 return 0;
}
'''
    run_c(tmp_path,harness,[FW/'main/pet_p4_form_v2.c',FW/'main/pet_p4_fruit_core.c'])
    # The real input/restore/render paths use a locked PSRAM workspace, not
    # another 2176-byte caller stack array beneath the core's atomic candidate.
    assert 'pet_form3_value_t values[PET_FORM3_VARS];' not in code
    assert 'static EXT_RAM_BSS_ATTR pet_form3_value_t g_form3_workspace' in code

def test_real_surface_parser_shell_pages_and_canvas_rejection(tmp_path):
    src=tmp_path/'surface.c';exe=tmp_path/'surface';src.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "pet_p4_surface.h"
int main(int argc,char**argv){assert(argc==3);FILE*f=fopen(argv[1],"rb");char raw[16000];size_t n=fread(raw,1,sizeof(raw)-1,f);fclose(f);raw[n]=0;cJSON*j=cJSON_Parse(raw);static pet_surface_t s;bool ok=pet_surface_parse(j,&s);cJSON_Delete(j);if(argv[2][0]=='0')return ok?1:0;assert(ok);if(argv[2][0]=='2')return 0;assert(s.count==4);for(unsigned p=0;p<12;p++){for(unsigned i=0;i<4;i++)assert(pet_surface_node_on_page(&s.nodes[i],p)==(i==3||p==(i==0?0:i==1?4:11)));}return 0;}
''')
    flags=['-DPET_CANVAS_ENABLED=1','-DPET_P4_APP_SHELL=1','-DPET_P4_QUOTA_V3=1','-DPET_P4_TEXT_TABLE=1','-DPET_P4_APP_DIRTY_CACHE=1']
    subprocess.run(['cc','-std=c11',*flags,'-fsanitize=address,undefined','-I'+str(FW/'main'),'-I'+str(VENDOR),str(src),str(FW/'main/pet_p4_surface.c'),str(FW/'main/pet_p4_canvas.c'),str(VENDOR/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    s={'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','shell':'p4-app-shell-v1','bindings':'p4-variable-bindings-v1','background':0,'elements':[dict(type='text',x=0,y=i*32,w=200,h=32,color=65535,text='page',**({} if p is None else {'page':p})) for i,p in enumerate([0,4,11,None])]}
    path=tmp_path/'surface.json';path.write_text(json.dumps(s));subprocess.run([str(exe),str(path),'1'],check=True)
    s['canvas']={'engine':'p4-canvas-v1','x':0,'y':0,'w':100,'h':100,'page':4,'commands':[{'op':'line','x':0,'y':0,'x2':99,'y2':99,'color':1}]};path.write_text(json.dumps(s));subprocess.run([str(exe),str(path),'0'],check=True)
    del s['shell'];s['elements']=[{'type':'panel','x':0,'y':0,'w':20,'h':20,'color':0,'page':4}];path.write_text(json.dumps(s));subprocess.run([str(exe),str(path),'2'],check=True)

def test_production_notify_and_refresh_parsers_queue_and_drain(tmp_path):
    code=(FW/'main/pet_p4_miniapp.c').read_text()
    types=code[code.index('typedef struct {\n  int8_t from_state;'):code.index('} miniapp_tick_t;')+len('} miniapp_tick_t;')]
    parsers=function(code,'parse_page_op')+code[code.index('static bool parse_transitions('):code.index('static bool parse_value(')]
    # Run the actual branch-side hooks from the accepted transition/tick paths.
    hooks=code[code.index('#if PET_P4_NOTIFY\n    if(taken&&transition->notify)'):code.index('    if (transition->sfx_cue >= 0) g_runtime.sfx_pending')]
    tickhook=code[code.index('#if PET_P4_NOTIFY\n        if(tick->completion_notify)'):code.index('        if (tick->completion_sfx_cue >= 0) g_runtime.sfx_pending')]
    harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_notify.h"
#include "pet_p4_app_navigation.h"
#define PET_P4_NOTIFY 1
#define PET_P4_DATA_ROWS 1
#define PET_P4_CELLS_V1 0
#define PET_P4_CAPABILITY_EXT 0
#define PET_P4_MINIAPP_ACTION_MAX 48
#define MINIAPP_MAX_EFFECTS 4
#define MINIAPP_MAX_CONDITIONS 2
#define MINIAPP_VAR_INT 0
#define MINIAPP_V1_MAX_TRANSITIONS 24
#define MINIAPP_V2_MAX_TRANSITIONS 32
#define MINIAPP_MAX_TRANSITIONS 48
#define MINIAPP_V1_MAX_TICKS 8
#define MINIAPP_V2_MAX_TICKS 12
#define MINIAPP_MAX_TICKS 16
#define MINIAPP_TICK_MIN_MS 100
#define MINIAPP_CAP(r,a,b,c) (c)
#define MINIAPP_EXT_OR_EVENT_ACTION(a)
typedef struct{int var;}miniapp_condition_t;
typedef struct{int dummy;}miniapp_effect_t;
'''+types+r'''
typedef struct{char name[32];int type,int_value;} var_t;
typedef struct{bool active,notice_enabled,notice_pending,navigation,logic;int8_t notice_badge_var;pet_notify_t notice;char widget_id[48],data_source[48],states[16][24],pages[12][24];int state_count,page_count,var_count;var_t vars[32];unsigned transition_count,tick_count;miniapp_transition_t transitions[48];miniapp_tick_t ticks[16];struct{char refresh_action[48];}data_rows;bool data_refresh_pending,data_refresh_used;uint64_t data_refresh_last;} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;static bool g_input_visible=true;
static int emitted;static char last_topic[48];static cJSON *last_payload;
static bool pet_app_host_emit(const char*t,const cJSON*p){emitted++;strcpy(last_topic,t);cJSON_Delete(last_payload);last_payload=cJSON_Duplicate(p,true);return true;}
static int find_var(const miniapp_runtime_t*r,const char*n){for(int i=0;i<r->var_count;i++)if(!strcmp(r->vars[i].name,n))return i;return -1;}
static int find_name(char names[][24],int n,const char*k){for(int i=0;i<n;i++)if(!strcmp(names[i],k))return i;return -1;}
static void copy_utf8(char*out,size_t n,const char*s){snprintf(out,n,"%s",s);}
static const char*json_string(const cJSON*j,const char*k){const cJSON*v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static bool safe_id(const char*s,size_t n){return s&&s[0]&&strlen(s)<n;}
static bool object_keys_allowed(const cJSON*j,const char*const*keys,size_t n){if(!cJSON_IsObject(j))return false;const cJSON*v;cJSON_ArrayForEach(v,j){bool ok=false;for(unsigned i=0;i<n;i++)if(!strcmp(v->string,keys[i]))ok=true;if(!ok)return false;}return true;}
static void set_error(char*out,size_t n,const char*s){snprintf(out,n,"%s",s);}
/* These existing operations are orthogonal to the D hooks tested here. */
static bool parse_effects(miniapp_runtime_t*r,const cJSON*j,miniapp_effect_t*e,uint8_t*c,char*out,size_t n){(void)r;(void)j;(void)e;(void)out;(void)n;*c=0;return true;}
static bool parse_condition(miniapp_runtime_t*r,const cJSON*j,miniapp_condition_t*c,uint8_t*n,bool legacy,char*out,size_t z){(void)r;(void)j;(void)c;(void)legacy;(void)out;(void)z;*n=1;return true;}
static bool parse_sfx_ref(const miniapp_runtime_t*r,const cJSON*j,int8_t*out,char*err,size_t n){(void)r;(void)j;(void)err;(void)n;*out=-1;return true;}
#include "pet_p4_miniapp_d.inc"
'''+parsers+r'''
static void accepted_transition(miniapp_transition_t *transition,const char*action,bool taken,uint64_t now_ms){
'''+hooks+r'''
}
static void completed_tick(miniapp_tick_t*tick,uint64_t now_ms){
'''+tickhook+r'''
}
int main(void){
 char error[120];g_runtime.active=true;g_runtime.var_count=1;strcpy(g_runtime.widget_id,"test-app");strcpy(g_runtime.vars[0].name,"badge");g_runtime.vars[0].int_value=7;
 cJSON*root=cJSON_Parse("{\"notify\":{\"title\":\"Done\",\"badge_var\":\"badge\"}}");assert(miniapp_parse_notify(&g_runtime,root));cJSON_Delete(root);
 cJSON*rules=cJSON_Parse("[{\"from\":\"*\",\"on\":\"done\",\"notify\":true}]");assert(parse_transitions(&g_runtime,rules,error,sizeof(error)));cJSON_Delete(rules);
 miniapp_transition_t*t=&g_runtime.transitions[0];accepted_transition(t,"done",false,100);miniapp_d_drain_locked(true);assert(emitted==0);
 accepted_transition(t,"done",true,100);assert(g_runtime.notice_pending);g_runtime.vars[0].int_value=99;miniapp_d_drain_locked(true);assert(emitted==1&&!strcmp(last_topic,"widget/notify"));pet_notify_t notice;assert(pet_notify_parse(last_payload,&notice)&&notice.badge==7);
 accepted_transition(t,"done",true,200);miniapp_d_drain_locked(true);assert(emitted==1);
 cJSON*ticks=cJSON_Parse("[{\"every_ms\":100,\"when\":{\"var\":\"badge\",\"lte\":0},\"then\":{\"notify\":true}}]");assert(parse_ticks(&g_runtime,ticks,error,sizeof(error)));cJSON_Delete(ticks);completed_tick(&g_runtime.ticks[0],30100);miniapp_d_drain_locked(true);assert(emitted==2);
 accepted_transition(t,"done",true,60100);miniapp_d_drain_locked(false);assert(emitted==2&&!g_runtime.notice_pending);miniapp_d_drain_locked(true);assert(emitted==2);
 g_runtime.transition_count=0;rules=cJSON_Parse("[{\"from\":\"*\",\"on\":\"done\",\"notify\":false}]");assert(!parse_transitions(&g_runtime,rules,error,sizeof(error)));cJSON_Delete(rules);
 g_runtime.notice_enabled=false;bool enabled;root=cJSON_Parse("{\"notify\":true}");assert(!miniapp_parse_notify_rule(&g_runtime,root,&enabled));cJSON_Delete(root);
 strcpy(g_runtime.data_source,"todos.upcoming");strcpy(g_runtime.data_rows.refresh_action,"data.refresh");
 accepted_transition(t,"data.refresh",true,100);miniapp_d_drain_locked(true);assert(emitted==3&&!strcmp(last_topic,"widget/data/refresh"));assert(!strcmp(json_string(last_payload,"source"),"todos.upcoming"));
 accepted_transition(t,"data.refresh",true,5099);miniapp_d_drain_locked(true);assert(emitted==3);accepted_transition(t,"data.refresh",true,5100);miniapp_d_drain_locked(true);assert(emitted==4);
 accepted_transition(t,"data.refresh",true,10100);memset(&g_runtime,0,sizeof(g_runtime));miniapp_d_drain_locked(true);assert(emitted==4);cJSON_Delete(last_payload);return 0;
}
'''
    run_c(tmp_path,harness)
    assert code.index('miniapp_d_drain_locked(visible);')>code.index('void pet_p4_miniapp_host_process(')
    assert 'component_app::data_refresh_event(&payload)' in (ROOT/'pc/src-tauri/src/lib.rs').read_text()

def test_legacy_form_quota3_rejection_and_defensive_write_bounds(tmp_path):
    import ctypes as C
    import copy
    import sys
    sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
    import app_form,app_form_v2
    src=tmp_path/'legacy.c';libpath=tmp_path/'legacy.dylib'
    src.write_text('''#include "pet_p4_form_v2_json.h"
int parse(const char*raw,const char*bs,int version){cJSON*r=cJSON_Parse(raw),*b=cJSON_Parse(bs);pet_form_t f;pet_form_text_config_t t;int ok=version==1?pet_form_parse(r,b,&f):pet_form2_parse(r,b,&f,&t);cJSON_Delete(r);cJSON_Delete(b);return ok;}
''')
    subprocess.run(['cc','-shared','-fPIC','-I'+str(FW/'main'),'-I'+str(VENDOR),str(src),str(FW/'main/pet_p4_form_v2.c'),str(VENDOR/'cJSON.c'),'-o',str(libpath)],check=True)
    lib=C.CDLL(str(libpath));lib.parse.argtypes=[C.c_char_p,C.c_char_p,C.c_int];lib.parse.restype=C.c_int
    for version,module in [(1,app_form),(2,app_form_v2)]:
        s=json.loads((ROOT/f'pc/skills/petui/contracts/app-form-v{version}.json').read_text())
        for count in [8,9,16,17,32]:
            r=copy.deepcopy(s['runtime']);r['quota']='p4-quota-v3'
            while len(r['vars'])<count:r['vars'][f'extra{len(r["vars"])}']={'type':'int','init':0}
            assert bool(lib.parse(json.dumps(r).encode(),json.dumps(s['buttons']).encode(),version))==(count==8)
            assert (not module.errors(r,s['buttons']))==(count==8)
    code=(FW/'main/pet_p4_miniapp.c').read_text()
    actual=''.join(function(code,n) for n in ['form2_values','form2_write','form_write'])
    run_c(tmp_path,r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_form_v2.h"
#define MINIAPP_VAR_INT 0
typedef struct{int type,int_value;char string_value[128];}var_t;
typedef struct{int var_count;var_t vars[32];}miniapp_runtime_t;
static unsigned g_viewport_epoch;static int g_pressed_node;
static void copy_utf8(char*out,size_t n,const char*s){snprintf(out,n,"%s",s);}
'''+actual+r'''
int main(void){miniapp_runtime_t r={.var_count=32};memset(r.vars[31].string_value,'x',127);r.vars[31].type=1;pet_form_value_t values[PET_FORM_VARS];memset(values,0xa5,sizeof(values));form2_values(&r,values);for(unsigned i=0;i<sizeof(values);i++)assert(((unsigned char*)values)[i]==0);assert(!form2_write(&r,values));int32_t ints[PET_FORM_VARS]={0};assert(!form_write(&r,ints));assert(strlen(r.vars[31].string_value)==127);assert(g_viewport_epoch==0);return 0;}
''')
