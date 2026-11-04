"""Compile production admission + transition execution slice against shared PC vectors.

Hardware/media/game callbacks are inert: this verifies the real C guarded branch,
nav mutation and effects, not scheduling, rendering or device timing.
"""
import json
import subprocess
from pathlib import Path
from petui_input_policy_test import ROOT, guard_support, policy_harness_prefix


def test_guard_navigation_production_admission_and_execution(tmp_path):
    code=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text()
    a=code.index('static bool runtime_has_transition_action(')
    b=code.index('static bool json_int_range(',a)
    prefix=policy_harness_prefix()
    prefix=prefix.replace('int var_index,source_var,op,int_value;', 'int var_index,source_var,op,int_value;char string_value[128];')
    prefix=prefix.replace('typedef struct {\n bool input_v1', 'typedef struct {int int_value,type,int_min,int_max;char name[32],string_value[128];}miniapp_var_t;\ntypedef struct {\n bool input_v1')
    prefix=prefix.replace('struct {int int_value,type;}vars[8];', 'miniapp_var_t vars[8];')
    prefix=prefix.replace('current_state,current_page;', 'current_state,page_count;int8_t current_page;')
    prefix=prefix.replace('pet_local_config_t local;', 'struct {bool active;int playing_state;struct {int last_tick_ms;}engine;}game;char pause_action[48];int tick_count;struct {int last_ms;}ticks[1];pet_local_config_t local;')
    extra=r'''
#include <assert.h>
#include <limits.h>
#define MINIAPP_MAX_VARS 8
static miniapp_runtime_t g_runtime;
static unsigned g_input_epoch;
typedef int pet_p4_game_command_t;
static void pet_p4_game_release(void*a,bool b,uint64_t c){(void)a;(void)b;(void)c;}
static int32_t clamp_i64(long long v){return v<INT32_MIN?INT32_MIN:v>INT32_MAX?INT32_MAX:(int32_t)v;}
static void viewport_offset_changed(miniapp_runtime_t*r,const char*n,int a,int b){(void)r;(void)n;(void)a;(void)b;}
'''
    ea=code.index('static bool apply_effects(');eb=code.index('static bool game_command_for_action(',ea)
    la=code.index('  for (int i = 0; i < g_runtime.transition_count;',code.index('static bool dispatch_action_observed('))
    lb=code.index('#if PET_P4_CELLS_V1',la)
    execution='static bool execute(const char*action){bool handled=false;uint64_t now_ms=0;\n'+code[la:lb]+'handled=true;break;}return handled;}\n'
    cases=json.loads((ROOT/'pc/skills/petui/contracts/pc-contract-regressions-v1.json').read_text())['navigation']
    tests=[]
    ops={'push':'PET_NAV_PUSH','replace':'PET_NAV_REPLACE','reset':'PET_NAV_RESET','back':'PET_NAV_BACK'}
    for item in cases:
        r=item['runtime'];pages=[p['id']for p in r['pages']];states=r['states'];vars=list(r['vars']);exp=item['expected']
        lines=['{g_runtime=(miniapp_runtime_t){.input_v1=true,.navigation=true,.page_count=%d,.var_count=%d,.transition_count=%d,.current_page=%d};'%(len(pages),len(vars),len(r['transitions']),pages.index(r['initial_page']))]
        for i,v in enumerate(r['vars'].values()):lines.append(f'g_runtime.vars[{i}]=(miniapp_var_t){{.int_value={v["init"]},.int_min=-1000000000,.int_max=1000000000}};')
        hist=item['initialHistory'];lines.append(f'g_runtime.navigation_history.depth={len(hist)};')
        for i,p in enumerate(hist):lines.append(f'g_runtime.navigation_history.pages[{i}]={pages.index(p)};')
        for i,t in enumerate(r['transitions']):
            lines.append(f'miniapp_transition_t*t{i}=&g_runtime.transitions[{i}];strcpy(t{i}->action,"{t["on"]}");t{i}->from_state={-1 if t["from"]=="*" else states.index(t["from"])};t{i}->to_state=t{i}->page_op=t{i}->else_to_state=t{i}->else_page_op=-1;t{i}->navigation_rule=(pet_nav_rule_t){{.from_page={pages.index(t["from_page"])if "from_page"in t else -1},.target=-1}};')
            if 'page'in t:
                op,value=next(iter(t['page'].items()));lines.append(f't{i}->navigation_rule.op={ops[op]};t{i}->navigation_rule.target={-1 if op=="back"else pages.index(value)};')
            if 'when'in t:
                c=t['when'];lines.append(f't{i}->when_count=1;t{i}->when[0]=(miniapp_condition_t){{.var={vars.index(c["var"])},.value_var=-1,.value={c["value"]},.op=PET_P4_GAME_CMP_EQ}};')
            offset=0
            for branch,label in [(t,''),(t.get('else',{}),'else_')]:
                if isinstance(branch.get('page'),str):lines.append(f't{i}->{label}page_op={(-2 if branch["page"]=="next"else -3 if branch["page"]=="previous"else pages.index(branch["page"]))};')
                if label:lines.append(f't{i}->has_else={str("else"in t).lower()};')
                for name,amount in branch.get('inc',{}).items():
                    lines.append(f't{i}->effects[{offset}]=(miniapp_effect_t){{.var_index={vars.index(name)},.source_var=-1,.op=MINIAPP_EFFECT_INC,.int_value={amount}}};t{i}->{label}effect_count++;');offset+=1
        lines.append(f'assert(runtime_action_enabled(&g_runtime,"{item["action"]}")=={str(exp["enabled"]).lower()});')
        # Production dispatcher rejects failed input admission before the loop.
        lines.append(f'if(runtime_action_enabled(&g_runtime,"{item["action"]}"))assert(execute("{item["action"]}"));')
        lines.append(f'assert(g_runtime.current_page=={pages.index(exp["page"])});assert(g_runtime.vars[{vars.index("hits")}].int_value=={exp["hits"]});assert(g_runtime.navigation_history.depth=={len(exp["history"])});')
        for i,p in enumerate(exp['history']):lines.append(f'assert(g_runtime.navigation_history.pages[{i}]=={pages.index(p)});')
        lines.append('}');tests+=lines
    src=tmp_path/'guard.c';src.write_text(prefix+guard_support(code)+extra+code[a:b]+code[ea:eb]+execution+'int main(void){\n'+'\n'.join(tests)+'return 0;}')
    vendor=ROOT/'firmware/tests/vendor/cjson';exe=tmp_path/'guard'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I',str(vendor),'-I',str(ROOT/'firmware/main'),str(src),str(ROOT/'firmware/main/pet_p4_local_tools.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
