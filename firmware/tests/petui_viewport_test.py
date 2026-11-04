"""Run actual C projection/parser against Python and shared layout vectors."""
import copy,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from app_surface import app_surface_errors
from app_viewport import project,hit
def multi_suite():
    return json.loads((ROOT/'pc/skills/petui/contracts/app-viewports-v1.json').read_text())

def test_multiple_viewports_shared_vectors_and_rejection_parity(tmp_path):
    suite=multi_suite();surface=suite['surface']
    for case in suite['cases']:
        actual=[]
        for node in surface['elements']:
            p=project(surface,node,case['vars']);actual.append([p['x'],p['y']] if p else None)
        assert actual==case['positions']
        for x,y,expected in case['hits']:
            assert next((n['action'] for n in reversed(surface['elements']) if hit(surface,n,case['vars'],x,y)),None)==expected
    source=tmp_path/'multi.c';binary=tmp_path/'multi';vendor=ROOT/'firmware/tests/vendor/cjson'
    source.write_text(r'''#include "pet_p4_surface.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
int main(int argc,char**argv){FILE*f=fopen(argv[1],"rb");char data[16384];size_t len=fread(data,1,sizeof(data)-1,f);data[len]=0;fclose(f);cJSON*j=cJSON_Parse(data);pet_surface_t s;int ok=pet_surface_parse(argc>2?cJSON_GetObjectItemCaseSensitive(j,"surface"):j,&s);if(!ok){cJSON_Delete(j);return 1;}
if(argc>2){cJSON *cases=cJSON_GetObjectItemCaseSensitive(j,"cases"),*c;
cJSON_ArrayForEach(c,cases){cJSON*vars=cJSON_GetObjectItemCaseSensitive(c,"vars"),*positions=cJSON_GetObjectItemCaseSensitive(c,"positions"),*hits=cJSON_GetObjectItemCaseSensitive(c,"hits");
for(int i=0;i<s.count;i++){pet_surface_node_t p;const pet_surface_viewport_t*v=pet_surface_node_viewport(&s,&s.nodes[i]);cJSON*value=v?cJSON_GetObjectItemCaseSensitive(vars,v->offset_var):NULL;int scroll=value?value->valueint:0;cJSON*pos=cJSON_GetArrayItem(positions,i);bool visible=pet_surface_project(v,&s.nodes[i],scroll,&p);assert(visible==!cJSON_IsNull(pos));if(visible){assert(p.x==cJSON_GetArrayItem(pos,0)->valueint);assert(p.y==cJSON_GetArrayItem(pos,1)->valueint);}}
cJSON*h;cJSON_ArrayForEach(h,hits){int x=cJSON_GetArrayItem(h,0)->valueint,y=cJSON_GetArrayItem(h,1)->valueint;const char*action=NULL;for(int i=s.count-1;i>=0;i--){const pet_surface_viewport_t*v=pet_surface_node_viewport(&s,&s.nodes[i]);cJSON*value=v?cJSON_GetObjectItemCaseSensitive(vars,v->offset_var):NULL;if(pet_surface_hit(v,&s.nodes[i],value?value->valueint:0,x,y)){action=s.nodes[i].action;break;}}cJSON*expected=cJSON_GetArrayItem(h,2);assert(cJSON_IsNull(expected)?!action:action&&!strcmp(action,expected->valuestring));}}
}cJSON_Delete(j);return 0;}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary),str(ROOT/'pc/skills/petui/contracts/app-viewports-v1.json'),'vectors'],check=True)
    cases=[(surface,True)]
    adjacent=copy.deepcopy(surface);adjacent['viewports']['items'][1]['x']=304;cases.append((adjacent,True))
    third=copy.deepcopy(surface);third['viewports']['items'].append({'id':'bottom','x':16,'y':400,'w':608,'h':64,'content_height':2048,'offset_var':'third_offset'});cases.append((third,True))
    bad=copy.deepcopy(third);bad['viewports']['items'].append(copy.deepcopy(bad['viewports']['items'][0]));cases.append((bad,False))
    for case in suite['invalid']:
        bad=copy.deepcopy(surface);parts=case['pointer'].strip('/').split('/');owner=bad
        for part in parts[:-1]:owner=owner[int(part)] if isinstance(owner,list) else owner[part]
        owner[parts[-1]]=case['value'];cases.append((bad,False))
    for field,value in [('in_view',True),('motion','float'),('view',None),('view',[]),('unknown',True)]:
        bad=copy.deepcopy(surface);bad['elements'][0][field]=value;cases.append((bad,False))
    bad=copy.deepcopy(surface);bad['viewport']={'engine':'p4-app-viewport-v1',**{k:v for k,v in bad['viewports']['items'][0].items() if k!='id'}};cases.append((bad,False))
    for field,value in [('bindings','wrong'),('graphics','wrong'),('game_view',{}),('data_view',{}),('scene_view',{})]:
        bad=copy.deepcopy(surface);bad[field]=value;cases.append((bad,False))
    bad=copy.deepcopy(surface);bad['elements']*=11;cases.append((bad,False))
    bad=copy.deepcopy(surface);bad['viewports']['items'][0]['script']='code';cases.append((bad,False))
    for value,valid in cases:
        assert (not app_surface_errors(value))==valid,value
        path=tmp_path/'surface.json';path.write_text(json.dumps(value));assert (subprocess.run([str(binary),str(path)]).returncode==0)==valid,value

def test_multi_viewport_local_and_host_owners_remain_conservative():
    surface=multi_suite()['surface']
    runtime={'vars':{'left_offset':{'type':'int'},'right_offset':{'type':'int'}},'input':{'engine':'p4-input-v1'},'transitions':[{'on':a} for a in ('left.row','right.row','fixed')]}
    for host in (False,True):
        owner=copy.deepcopy(runtime)
        if host:owner['host']=True
        assert not app_surface_errors(surface,owner)
        for field in ('scene','game','data','media','instrument'):
            bad=copy.deepcopy(owner);bad[field]={};assert app_surface_errors(surface,bad)
        for value in ('string',None):
            bad=copy.deepcopy(owner);bad['vars']['right_offset']['type']=value;assert app_surface_errors(surface,bad)
        bad=copy.deepcopy(owner);bad.pop('input');assert app_surface_errors(surface,bad)

def test_production_effects_cancel_scroll_away_and_back_but_not_clamped_edges(tmp_path):
    _production_effects_case(tmp_path,False)


def test_mixed_host_fields_do_not_suppress_native_viewport_effect_epochs(tmp_path):
    _production_effects_case(tmp_path,True)


def _production_effects_case(tmp_path,mixed):
    import re
    text=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text()
    def function(name):
        start=re.search(r'^static [^\n;{]*\b'+name+r'\([^;{]*\)\s*\{',text,re.M).start();begin=text.index('{',start);depth=1;end=begin+1
        while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
        return text[start:end]+'\n'
    # The real helper compiles without g_input_epoch: offset changes may not
    # alter the physical gate that requires joystick neutral on page changes.
    assert 'g_input_epoch' not in function('viewport_offset_changed')
    code=r'''#include "pet_p4_surface.h"
#include "pet_p4_variable_owners.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define MINIAPP_MAX_VARS 8
#define MINIAPP_VAR_INT 0
#define MINIAPP_EFFECT_INC 1
#define MINIAPP_INT_MIN -1000000000
#define MINIAPP_INT_MAX 1000000000
typedef struct {int type;char name[32];int32_t int_value,int_min,int_max;char string_value[64];} miniapp_var_t;
typedef struct {int var_index,op,source_var;int32_t int_value;char string_value[64];} miniapp_effect_t;
typedef struct {bool app_host;pet_variable_owners_t variable_owners;pet_surface_t surface;int var_count;miniapp_var_t vars[8];} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;static unsigned g_viewport_epoch;static int g_pressed_node;
static void copy_utf8(char*out,size_t size,const char*in){snprintf(out,size,"%s",in);}
'''+''.join(function(name) for name in ('clamp_i64','runtime_host_variable','find_var','viewport_offset_changed','apply_effects'))+r'''
int main(void){
g_runtime.var_count=3;g_runtime.surface.viewport_count=2;
#if PET_VARIABLE_OWNERS_ENABLED
g_runtime.app_host=true;g_runtime.variable_owners.mixed=true;g_runtime.variable_owners.host_mask=4;
#endif
for(int i=0;i<3;i++){g_runtime.vars[i].int_min=MINIAPP_INT_MIN;g_runtime.vars[i].int_max=MINIAPP_INT_MAX;snprintf(g_runtime.vars[i].name,32,"offset%d",i);}
for(int i=0;i<2;i++){pet_surface_viewport_t*v=&g_runtime.surface.viewports[i];v->enabled=true;v->h=320;v->content_height=720;snprintf(v->offset_var,32,"offset%d",i);}
miniapp_effect_t effect={.var_index=0,.op=MINIAPP_EFFECT_INC,.source_var=-1,.int_value=120};
g_pressed_node=0;assert(apply_effects(&g_runtime,&effect,1));assert(g_viewport_epoch==1&&g_pressed_node==-1);
effect.int_value=-120;assert(apply_effects(&g_runtime,&effect,1));assert(g_viewport_epoch==2); /* restored position does not restore the old pointer epoch */
effect.var_index=1;effect.int_value=400;g_pressed_node=0;assert(apply_effects(&g_runtime,&effect,1));assert(g_viewport_epoch==3&&g_pressed_node==-1);
effect.int_value=100;g_pressed_node=0;assert(apply_effects(&g_runtime,&effect,1));assert(g_viewport_epoch==3&&g_pressed_node==0);
effect.var_index=2;assert(apply_effects(&g_runtime,&effect,1));assert(g_viewport_epoch==3&&g_pressed_node==0);
g_runtime.vars[0].int_max=0;effect.var_index=0;assert(!apply_effects(&g_runtime,&effect,1));assert(g_viewport_epoch==3);
return 0;}
'''
    source=tmp_path/'offset.c';source.write_text(code);binary=tmp_path/'offset';vendor=ROOT/'firmware/tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wno-deprecated-declarations','-fsanitize=address,undefined',f'-DPET_VARIABLE_OWNERS_ENABLED={int(mixed)}','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_simulator_observes_each_independent_scroll_action():
    from bounded_app import initial,action,visible
    runtime={'states':['ready'],'initial_state':'ready','vars':{'left_offset':{'type':'int','init':0},'right_offset':{'type':'int','init':0}},'tick':[],'transitions':[{'from':'*','on':'left.next','inc':{'left_offset':120}},{'from':'*','on':'right.next','inc':{'right_offset':200}}],'dashboard':{},'surface':multi_suite()['surface']}
    start=initial(runtime);left=action(runtime,start,'left.next');both=action(runtime,left,'right.next')
    assert visible(runtime,start)!=visible(runtime,left)!=visible(runtime,both)
    assert left['vars']['right_offset']==0 and both['vars']['left_offset']==120
def test_viewport_capability_must_be_negotiated(tmp_path):
    from validate_generated_widget import DEFAULT_CAPABILITIES,validate_runtime
    r={'schema_version':1,'engine':'p4-bounded-runtime-v4','states':['ready'],'initial_state':'ready','vars':{'scroll':{'type':'int','init':0}},'tick':[],'transitions':[],
       'input':{'engine':'p4-input-v1'},'dashboard':{'title':'Viewport check','visualStyle':'clean','visualPalette':'ocean','visualLayout':'tool','visualSprite':'timer'},
       'surface':{'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','bindings':'p4-variable-bindings-v1','background':0,'viewport':{'engine':'p4-app-viewport-v1','x':16,'y':64,'w':608,'h':352,'content_height':720,'offset_var':'scroll'},'elements':[{'type':'text','x':20,'y':600,'w':200,'h':32,'color':65535,'text':'History','in_view':True}]}}
    errors=[];validate_runtime(r,'tool',tmp_path,copy.deepcopy(DEFAULT_CAPABILITIES),errors);assert not errors
    caps=copy.deepcopy(DEFAULT_CAPABILITIES);caps.pop('widgetAppViewport');errors=[];validate_runtime(r,'tool',tmp_path,caps,errors);assert len(errors)==1 and '滚动视口' in errors[0]
    multi=copy.deepcopy(r);v=multi['surface'].pop('viewport');v.pop('engine');v['id']='main';multi['surface']['viewports']={'engine':'p4-app-viewports-v1','items':[v]}
    multi['surface']['elements'][0].pop('in_view');multi['surface']['elements'][0]['view']='main'
    errors=[];validate_runtime(multi,'tool',tmp_path,copy.deepcopy(DEFAULT_CAPABILITIES),errors);assert not errors
    caps=copy.deepcopy(DEFAULT_CAPABILITIES);caps.pop('widgetAppViewports');errors=[];validate_runtime(multi,'tool',tmp_path,caps,errors);assert len(errors)==1,errors
    # A device with the legacy capability only must not get a multi-view pack.
    assert caps['widgetAppViewport']=='p4-app-viewport-v1'
    assert any('视口' in e for e in errors)
def test_shared_projection_and_strict_parser(tmp_path):
    suite=json.loads((ROOT/'pc/skills/petui/contracts/app-viewport-v1.json').read_text())
    for case in suite['cases']:
        n=project(suite['surface'],suite['node'],{'scroll':case['scroll']})
        assert ([n['x'],n['y']]if n else None)==case['position']
        for x,y,expected in (case['hit'],case['outside']):assert hit(suite['surface'],suite['node'],{'scroll':case['scroll']},x,y)==expected
    surface={'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','bindings':'p4-variable-bindings-v1','background':65535,**suite['surface'],'elements':[{**suite['node'],'text':'Mark','color':0,'font_size':16}]}
    source=tmp_path/'viewport.c';binary=tmp_path/'viewport';vendor=ROOT/'firmware/tests/vendor/cjson'
    source.write_text('''#include "pet_p4_surface.h"
#include <stdio.h>
#include <assert.h>
int main(int argc,char**argv){(void)argc;FILE*f=fopen(argv[1],"rb");char data[8192];size_t n=fread(data,1,8191,f);data[n]=0;fclose(f);cJSON*j=cJSON_Parse(data);pet_surface_t s;int ok=pet_surface_parse(j,&s);cJSON_Delete(j);if(!ok)return 1;pet_surface_node_t p;
assert(pet_surface_project(&s.viewport,&s.nodes[0],100,&p)&&p.x==50&&p.y==280);
assert(pet_surface_hit(&s.viewport,&s.nodes[0],0,70,390));assert(!pet_surface_hit(&s.viewport,&s.nodes[0],0,70,410));
assert(!pet_surface_project(&s.viewport,&s.nodes[0],380,&p));assert(!pet_surface_hit(&s.viewport,&s.nodes[0],380,70,30));return 0;}
''')
    subprocess.run(['cc','-std=c11','-Wno-deprecated-declarations','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    cases=[(surface,True)]
    for key,value in [('engine','script'),('content_height',2049),('content_height',339),('offset_var','../private'),('h',True),('x',641),('w',0)]:
        bad=copy.deepcopy(surface);bad['viewport'][key]=value;cases.append((bad,False))
    for key,value in [('in_view',False),('in_view',1),('y',719),('h',481),('x',599),('motion','float')]:
        bad=copy.deepcopy(surface);bad['elements'][0][key]=value;cases.append((bad,False))
    for value,valid in cases:
        assert (not app_surface_errors(value))==valid,value
        path=tmp_path/'surface.json';path.write_text(json.dumps(value));assert (subprocess.run([str(binary),str(path)]).returncode==0)==valid,value

def test_production_panel_and_pixel_rasterizers_respect_viewport(tmp_path):
    import re
    text=(ROOT/'firmware/main/pet_p4_renderer.c').read_text()
    def function(name):
        start=re.search(r'^static [^\n;{]*\b'+name+r'\([^;{]*\)\s*\{',text,re.M).start()
        begin=text.index('{',start);depth=1;end=begin+1
        while depth:
            depth+=(text[end]=='{')-(text[end]=='}');end+=1
        return text[start:end]+'\n'
    clipping=text[text.index('static struct {bool active;int x,y,right,bottom;} g_app_clip;'):text.index('static void put_px(')]
    code='''#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t pixels[640*480],*g_framebuffer=pixels;
static uint16_t rgb565(unsigned r,unsigned g,unsigned b){return((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
'''+function('blend_rgb565')+clipping
    code+=''.join(function(n)for n in ['put_px','fill_rect','fill_ellipse','fill_round_rect','blend_px','blend_rect','blend_ellipse','blend_round_rect'])
    code+='''int main(void){
g_app_clip.active=true;g_app_clip.y=64;g_app_clip.bottom=416;
for(int region=0;region<3;region++){
g_app_clip.x=region==2?336:16;g_app_clip.right=region==1?304:624;
for(int y=-479;y<480;y+=37){
 for(int p=0;p<640*480;p++)pixels[p]=0x1357;
 fill_round_rect(16,y,608,304,24,0xffff);blend_round_rect(16,y,608,304,24,255,128,0,100);
 for(int row=0;row<480;row++)for(int x=0;x<640;x++)if(x<g_app_clip.x||x>=g_app_clip.right||row<64||row>=416)assert(pixels[row*640+x]==0x1357);
}
}
g_app_clip.active=false;fill_rect(0,0,640,480,0x2222);for(int p=0;p<640*480;p++)assert(pixels[p]==0x2222);
return 0;}
'''
    source=tmp_path/'raster.c';source.write_text(code);binary=tmp_path/'raster'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined',str(source),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
