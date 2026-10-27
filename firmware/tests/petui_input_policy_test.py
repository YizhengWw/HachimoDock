"""Production C policy/game checks plus cross-validator negative cases."""
from pathlib import Path
import copy
import json
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"pc/skills/petui/scripts"))
from input_policy import input_errors

def test_input_policy_real_c_and_python(tmp_path):
    code=(ROOT/"firmware/main/pet_p4_miniapp.c").read_text()
    start=code.index("static bool runtime_has_transition_action(")
    end=code.index("static bool json_int_range(",start)
    harness=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_local_tools.h"
enum {MINIAPP_EFFECT_SET,MINIAPP_EFFECT_INC};
typedef struct {int var_index,source_var,op,int_value;} miniapp_effect_t;
typedef struct {int from_state,to_state,page_op,effect_count;char action[48];miniapp_effect_t effects[4];} transition_t;
typedef transition_t miniapp_transition_t;
typedef struct {char event[48],action[48];} button_t;
typedef struct {
 bool input_v1;uint8_t repeat_mask,press_mask;char swipe_actions[4][48];
 char drag_entity[16];uint8_t drag_axis;char hold_action[48];
 char confirm_actions[3][48],confirm_prompts[3][121];
 char cooldown_actions[3][48];uint16_t cooldown_ms[3];uint64_t cooldown_at[3];uint8_t cooldown_used;
 int transition_count,button_count,current_state;
 char data_source[48];int data_page_var;struct {int int_value;}vars[8];
 transition_t transitions[24];button_t buttons[8];
 pet_local_config_t local;pet_local_state_t local_state;
} miniapp_runtime_t;
typedef struct {int pages,page;}pet_p4_data_view_t;
static int data_pages=1;
static uint64_t mock_now;
static uint64_t esp_timer_get_time(void){return mock_now*1000;}
static void pet_p4_widget_data_view(const char *source,int page,uint64_t now,pet_p4_data_view_t *v){
 (void)source;(void)now;v->pages=data_pages;v->page=((page%data_pages)+data_pages)%data_pages;
}
static const char *json_string(const cJSON *j,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static void copy_utf8(char *out,size_t size,const char *v){snprintf(out,size,"%s",v);}
static bool object_keys_allowed(const cJSON *o,const char *const *keys,size_t n){
 if(!cJSON_IsObject(o))return false;const cJSON *v;
 cJSON_ArrayForEach(v,o){bool ok=false;for(size_t i=0;i<n;i++)if(!strcmp(v->string,keys[i]))ok=true;if(!ok)return false;}return true;
}
'''+code[start:end]+r'''
int main(int argc,char **argv){
 (void)argc;char buf[8192];FILE *f=fopen(argv[1],"rb");size_t n=fread(buf,1,sizeof(buf)-1,f);buf[n]=0;fclose(f);
 cJSON *root=cJSON_Parse(buf);miniapp_runtime_t r={.transition_count=3,.button_count=3};
 const char *actions[]={"go","flap","reset"},*events[]={"knob.rotate_cw","button.sw1.short_press","button.sw2.short_press"};
 for(int i=0;i<3;i++){strcpy(r.transitions[i].action,actions[i]);r.transitions[i].from_state=i==0?0:-1;strcpy(r.buttons[i].action,actions[i]);strcpy(r.buttons[i].event,events[i]);}
 bool ok=parse_input_policy(&r,root);
 if(ok){r.current_state=1;if(runtime_action_enabled(&r,"go"))return 2;if(!runtime_action_enabled(&r,"flap"))return 3;}
 if(ok){
   strcpy(r.data_source,"todos.upcoming");r.current_state=0;
   r.transitions[0].to_state=r.transitions[0].page_op=-1;r.transitions[0].effect_count=1;
   r.transitions[0].effects[0]=(miniapp_effect_t){.var_index=0,.source_var=-1,.op=MINIAPP_EFFECT_INC,.int_value=1};
   if(runtime_action_enabled(&r,"go"))return 4;
   data_pages=4;if(!runtime_action_enabled(&r,"go"))return 5;
   r.transitions[0].effects[0].op=MINIAPP_EFFECT_SET;r.transitions[0].effects[0].int_value=0;
   if(runtime_action_enabled(&r,"go"))return 6;
   r.vars[0].int_value=-1;if(!runtime_action_enabled(&r,"go"))return 7;
   strcpy(r.cooldown_actions[0],"go");r.cooldown_ms[0]=30000;r.cooldown_used=1;r.cooldown_at[0]=0;
   if(runtime_action_enabled(&r,"go"))return 8;
   mock_now=30000;if(!runtime_action_enabled(&r,"go"))return 9;
 }
 cJSON_Delete(root);return ok?0:1;
}
'''
    src=tmp_path/"policy.c";src.write_text(harness);exe=tmp_path/"policy"
    vendor=ROOT/"firmware/tests/vendor/cjson"
    subprocess.run(["cc","-std=c11","-fsanitize=address,undefined","-I",str(vendor),"-I",str(ROOT/'firmware/main'),str(src),str(ROOT/'firmware/main/pet_p4_local_tools.c'),str(vendor/"cJSON.c"),"-lm","-o",str(exe)],check=True)
    runtime={"transitions":[{"on":a}for a in ("go","flap","reset")],"game":{"actions":{"flap":"flap"}},
             "input":{"engine":"p4-input-v1","repeat":["go"],"press":["flap"],"swipe":{"left":"go"},"confirm":{"reset":"重置计时？"}}}
    buttons=[{"action":a,"event":e}for a,e in zip(("go","flap","reset"),("knob.rotate_cw","button.sw1.short_press","button.sw2.short_press"))]
    cases=[(runtime,True)]
    for key,v in [("engine","script"),("cooldown",{"go":249}),("cooldown",{"go":60001}),("cooldown",{"go":True}),("cooldown",{"absent":500}),("repeat",["go"]*2),("press",["reset"]),("swipe",{"left":"unknown"}),("swipe",{"diagonal":"go"}),("confirm",{"reset":"字"*41}),("confirm",{"absent":"确定？"}),("repeat",["reset"]),("url","https://example.com")]:
        r=copy.deepcopy(runtime);r["input"][key]=v;cases.append((r,False))
    for i,(r,valid) in enumerate(cases):
        assert (not input_errors(r,buttons))==valid,r
        p=tmp_path/f"{i}.json";p.write_text(json.dumps(r))
        assert (subprocess.run([str(exe),str(p)]).returncode==0)==valid,r

def test_soft_drop_and_absolute_snake_in_production_core(tmp_path):
    src=tmp_path/"game.c"
    src.write_text(r'''
#include "pet_p4_game.h"
#include <assert.h>
#include <string.h>
int main(void){
 pet_p4_game_engine_t g;
 assert(pet_p4_game_configure(&g,PET_P4_GAME_BLOCKS,500,17));
 pet_p4_game_command(&g,PET_P4_GAME_COMMAND_START,1);
 for(int i=0;i<50;i++)pet_p4_game_command(&g,PET_P4_GAME_COMMAND_SOFT_DROP,i);
 for(int i=0;i<PET_P4_GAME_GRID_MAX_CELLS;i++)assert(g.board[i]==0);
 pet_p4_game_command(&g,PET_P4_GAME_COMMAND_DROP,100);
 int occupied=0;for(int i=0;i<PET_P4_GAME_GRID_MAX_CELLS;i++)occupied+=g.board[i]!=0;
 assert(occupied==4);
 assert(pet_p4_game_configure(&g,PET_P4_GAME_SNAKE,500,17));
 pet_p4_game_command(&g,PET_P4_GAME_COMMAND_START,1);
 assert(!pet_p4_game_command(&g,PET_P4_GAME_COMMAND_WEST,1));
 assert(pet_p4_game_command(&g,PET_P4_GAME_COMMAND_NORTH,2));
 assert(!pet_p4_game_command(&g,PET_P4_GAME_COMMAND_WEST,3));
 assert(!pet_p4_game_command(&g,PET_P4_GAME_COMMAND_SOUTH,4));
 pet_p4_game_process(&g,501);
 assert(g.snake_direction==3&&!g.snake_turn_pending);
 assert(!pet_p4_game_command(&g,PET_P4_GAME_COMMAND_SOUTH,501));
 assert(pet_p4_game_command(&g,PET_P4_GAME_COMMAND_WEST,502));
 pet_p4_game_process(&g,1001);assert(g.snake_direction==2);
 return 0;
}
''')
    exe=tmp_path/"game"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-I",str(ROOT/"firmware/main"),str(src),str(ROOT/"firmware/main/pet_p4_game.c"),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
