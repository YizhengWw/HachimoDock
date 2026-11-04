"""Real miniapp body/op parser + device solver versus same-core preview WASM."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import pytest
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from app_bodies import bodies_errors
FIX=json.loads((ROOT/'pc/skills/petui/contracts/bodies-v1.json').read_text())

PREFIX=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "pet_p4_bodies_json.h"
#include "pet_p4_bodies_raster.h"
#define MINIAPP_VAR_INT 0
#define MINIAPP_MAX_CONDITIONS 4
typedef struct {int var,value_var,op,value;}miniapp_condition_t;
typedef struct {bool logic,sfx;struct{int type;}vars[8];}miniapp_runtime_t;
static void set_error(char*out,size_t n,const char*s){snprintf(out,n,"%s",s);}
static const char*json_string(const cJSON*j,const char*k){const cJSON*v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static bool json_int_range(const cJSON*v,int lo,int hi,int*out){return pet_body_number(v,lo,hi,out);}
static bool object_keys_allowed(const cJSON*j,const char*const*keys,size_t n){if(!cJSON_IsObject(j))return false;const cJSON*v;cJSON_ArrayForEach(v,j){bool found=false;for(size_t i=0;i<n;i++)if(!strcmp(v->string,keys[i]))found=true;if(!found)return false;}return true;}
static void copy_utf8(char*out,size_t n,const char*s){snprintf(out,n,"%s",s);}
static bool safe_id(const char*s,size_t n){return s&&s[0]&&strlen(s)<n;}
static int bounded_find_entity(const pet_p4_bounded_game_config_t*c,const char*s){for(unsigned i=0;i<c->entity_count;i++)if(!strcmp(c->entities[i].id,s))return i;return -1;}
typedef struct{int count;}pet_p4_miniapp_sprite_pack_t;
static int sprite_pack_find(const pet_p4_miniapp_sprite_pack_t*s,const char*id){(void)s;(void)id;return -1;}
static int find_sfx_cue(const miniapp_runtime_t*r,const char*s){(void)r;(void)s;return -1;}
static int find_var(const miniapp_runtime_t*r,const char*s){(void)r;(void)s;return -1;}
static bool runtime_has_transition_action(const miniapp_runtime_t*r,const char*s){(void)r;return !strcmp(s,"start")||!strcmp(s,"kick");}
static bool runtime_has_button_action(const miniapp_runtime_t*r,const char*s){return runtime_has_transition_action(r,s);}
static bool parse_condition(const miniapp_runtime_t*r,const cJSON*w,miniapp_condition_t*g,uint8_t*n,bool multi,char*err,size_t size){(void)r;(void)w;(void)g;(void)n;(void)multi;(void)err;(void)size;return false;}
'''
MAIN=r'''
static int num(const cJSON*j,const char*k,int fallback){const cJSON*v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsNumber(v)?v->valueint:fallback;}
static void snapshot(pet_p4_game_engine_t*g){
 pet_p4_game_frame_t f;pet_p4_game_get_frame(g,&f);printf("[%ld,%d,%d,[",(long)f.score,f.running,f.game_over);
 for(unsigned i=0;i<f.entity_count;i++){pet_p4_game_entity_t*e=&f.entities[i];if(i)printf(",");printf("[%ld,%ld,%ld,%ld,%ld]",(long)e->x_q8,(long)e->y_q8,(long)e->body_vx_q8,(long)e->body_vy_q8,(long)e->angle_q8);}
 printf("]]\n");
}
int main(int argc,char**argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);char bytes[65536];size_t n=fread(bytes,1,sizeof(bytes)-1,f);bytes[n]=0;fclose(f);
 cJSON*root=cJSON_Parse(bytes);assert(root);const cJSON*s=cJSON_GetObjectItemCaseSensitive(root,"scene"),*grid=cJSON_GetObjectItemCaseSensitive(s,"grid");
 pet_p4_bounded_game_config_t c={0};c.width=num(grid,"width",0);c.height=num(grid,"height",0);c.tick_ms=num(s,"tick_ms",0);c.motion=cJSON_HasObjectItem(s,"motion");
 const cJSON*node;miniapp_runtime_t r={0};char error[160];pet_p4_miniapp_sprite_pack_t sprites={0};
 if(!parse_bounded_entities(&c,&sprites,cJSON_GetObjectItemCaseSensitive(s,"entities"),error,sizeof(error)))return 1;

 if(!pet_bodies_parse(s,&c)||!parse_bounded_rules(&r,&c,cJSON_GetObjectItemCaseSensitive(s,"rules"),error,sizeof(error)))return 1;
 pet_p4_game_engine_t game;if(!pet_p4_game_configure_bounded(&game,&c,17))return 1;
 if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s,"auto_start"))){game.running=true;game.frame.running=true;}
 snapshot(&game);
 cJSON_ArrayForEach(node,cJSON_GetObjectItemCaseSensitive(root,"events")){
  if(cJSON_HasObjectItem(node,"tick"))pet_p4_game_process(&game,num(node,"tick",0));
  else pet_p4_game_dispatch_action(&game,json_string(node,"action"),num(node,"now",0));snapshot(&game);
 }
 cJSON_Delete(root);return 0;
}
'''

@pytest.fixture(scope='module')
def native(tmp_path_factory):
    tmp=tmp_path_factory.mktemp('bodies');source=tmp/'body.c';exe=tmp/'body'
    mini=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text()
    names=['parse_bounded_entities','parse_bounded_coord','parse_bounded_entity_ref','parse_bounded_op','bounded_edge_from_name','parse_bounded_rules']
    source.write_text(PREFIX+'\n'.join(function(mini,name) for name in names)+MAIN)
    vendor=ROOT/'firmware/tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation','-fsanitize=address,undefined','-DPET_P4_BODIES=1','-DPET_P4_GRAPHICS_V2=1',
      '-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_game.c'),str(vendor/'cJSON.c'),'-o',str(exe)],check=True,capture_output=True)
    return exe

def run(exe,scene,tmp_path,events=None):
    path=tmp_path/'body.json';path.write_text(json.dumps({'scene':scene,'events':events or []}))
    return subprocess.run([str(exe),str(path)],capture_output=True,text=True,timeout=15)

def test_shared_schema_vectors_native_and_python(native,tmp_path):
    for s in FIX['valid']:
        assert not bodies_errors({**FIX['runtime'],'scene':s});r=run(native,s,tmp_path);assert r.returncode==0,r.stderr
    for s in FIX['invalid']:
        assert bodies_errors({**FIX['runtime'],'scene':s}),s
        r=run(native,s,tmp_path);assert r.returncode==1,(s,r.stderr)

def test_device_and_wasm_simulation_are_byte_exact(native,tmp_path):
    js=tmp_path/'trace.mjs';js.write_text('''import fs from 'node:fs';
import {createBodiesPreview} from '''+json.dumps((ROOT/'pc/src/component-center/bodies-core.js').as_uri())+''';
const f=JSON.parse(fs.readFileSync(process.argv[2]));const core=await createBodiesPreview(fs.readFileSync(process.argv[3]),f.scene);
const snapshot=()=>{const s=core.snapshot();console.log(JSON.stringify([s.score,+s.running,+s.over,s.entities.map(e=>[e.x*256,e.y*256,e.vx*256,e.vy*256,Math.round(e.rotation*256)])]));};
snapshot();for(const e of f.events){if(e.tick!==undefined)core.tick(e.tick);else core.action(e.action,e.now);snapshot();}
''')
    scenes=[FIX['valid'][0]]
    s=copy.deepcopy(scenes[0]);s['gravity']=64;s['entities'][1]['vx']=0;s['bodies'][1]['static']=True;scenes.append(s)
    s=copy.deepcopy(scenes[0]);s['bodies'][0].update(mass=3,restitution=40);s['bodies'][1].update(mass=7,restitution=80);s['tick_ms']=200;scenes.append(s)
    for s in scenes:
        result=run(native,s,tmp_path,FIX['events']);assert result.returncode==0,result.stderr
        wasm=subprocess.run(['node',str(js),str(tmp_path/'body.json'),str(ROOT/'pc/src/component-center/classic-core.wasm')],capture_output=True,text=True,check=True)
        assert [json.loads(l) for l in result.stdout.splitlines()]==[json.loads(l) for l in wasm.stdout.splitlines()]

def test_bodies_candidate_advertisement_requires_the_default_off_implementation_flag():
    protocol=(ROOT/'firmware/main/pet_p4_protocol.c').read_text()
    policy=(ROOT/'firmware/main/pet_p4_render_policy.h').read_text()
    assert '#ifndef PET_P4_BODIES\n#define PET_P4_BODIES 0\n#endif' in policy
    grant='cJSON_AddStringToObject(capabilities,"widgetBodies","p4-bodies-v1");'
    assert protocol.count(grant)==1
    candidate=protocol.split('#if PET_P4_BODIES\n',1)[1].split('#endif',1)[0]
    assert grant in candidate


def test_actual_body_renderer_rotates_palette_sprites_and_clips(tmp_path):
    renderer=(ROOT/'firmware/main/pet_p4_renderer.c').read_text()
    code=r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_surface.h"
#include "pet_p4_game.h"
#include "pet_p4_bodies_raster.h"
typedef struct {int frame_width,frame_height,fps;}pet_p4_miniapp_sprite_t;
typedef struct {uint16_t primary;}miniapp_pixel_palette_t;
static struct{int count;pet_p4_miniapp_sprite_t items[6];}g_surface_images;
static uint8_t g_surface_indices[64],g_surface_alpha[6][256];
static uint16_t g_surface_colors[6][256],pixels[640*480];
static unsigned g_surface_offsets[6];static unsigned writes;
static uint16_t miniapp_game_tone_color(int tone,const miniapp_pixel_palette_t*p){(void)tone;return p->primary;}
static void put_px(int x,int y,uint16_t color){assert(x>=0&&x<640&&y>=0&&y<480);pixels[y*640+x]=color;writes++;}
static void blend_px(int x,int y,int r,int g,int b,unsigned a){(void)r;(void)g;(void)b;assert(a>0&&a<255);put_px(x,y,99);}
'''+function(renderer,'draw_app_body')+r'''
int main(void){
 pet_p4_game_entity_t e={.width=1,.height=1,.shape=PET_P4_GAME_SHAPE_RECT,.angle_q8=90*256};
 pet_surface_scene_sprite_t sprite={.image=0,.columns=1,.fps=1};
 pet_surface_scene_view_t view={.x=10,.y=10,.w=2,.h=2};miniapp_pixel_palette_t palette={.primary=7};
 g_surface_images.count=1;g_surface_images.items[0]=(pet_p4_miniapp_sprite_t){.frame_width=2,.frame_height=2};
 for(unsigned i=0;i<4;i++){g_surface_indices[i]=i+1;g_surface_colors[0][i+1]=i+1;g_surface_alpha[0][i+1]=255;}
 for(unsigned i=0;i<640*480;i++)pixels[i]=77;
 draw_app_body(&e,&sprite,10,10,2,&view,&palette,0);
 assert(writes==4);assert(pixels[10*640+10]==3&&pixels[10*640+11]==1&&pixels[11*640+10]==4&&pixels[11*640+11]==2);
 for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++)if(x<10||x>=12||y<10||y>=12)assert(pixels[y*640+x]==77);
 /* Transparent samples do not overwrite the cached scene beneath the body. */
 g_surface_alpha[0][3]=0;g_surface_alpha[0][1]=128;pixels[10*640+10]=77;writes=0;
 draw_app_body(&e,&sprite,10,10,2,&view,&palette,0);assert(writes==3);assert(pixels[10*640+10]==77&&pixels[10*640+11]==99);
 /* Clipping removes rotated pixels rather than moving them into the viewport. */
 view.w=1;writes=0;draw_app_body(&e,&sprite,10,10,2,&view,&palette,0);assert(writes==1);
 return 0;
}
'''
    source=tmp_path/'raster.c';exe=tmp_path/'raster';source.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',
      '-DPET_P4_BODIES=1','-DPET_P4_GRAPHICS_V2=1','-I',str(ROOT/'firmware/main'),'-I',str(ROOT/'firmware/tests/vendor/cjson'),str(source),'-o',str(exe)],check=True,capture_output=True)
    subprocess.run([str(exe)],check=True,timeout=10)


def test_full_python_scene_validator_uses_bodies_schema(tmp_path):
    from validate_generated_widget import validate_scene
    for s in FIX['valid']:
        errors=[];validate_scene(s,{**FIX['runtime'],'scene':s},tmp_path,{'widgetScene':'p4-grid-scene-v2','widgetBodies':'p4-bodies-v1'},{'start','kick'},errors)
        assert not errors,errors
    for s in FIX['invalid']:
        errors=[];validate_scene(s,{**FIX['runtime'],'scene':s},tmp_path,{'widgetScene':'p4-grid-scene-v2','widgetBodies':'p4-bodies-v1'},{'start','kick'},errors)
        assert errors,s


def test_default_off_loader_refuses_bodies(native,tmp_path):
    source=native.parent/'body.c';exe=tmp_path/'body-off';vendor=ROOT/'firmware/tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-O1','-Wno-deprecated-declarations','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_game.c'),str(vendor/'cJSON.c'),'-o',str(exe)],check=True,capture_output=True)
    assert run(exe,FIX['valid'][0],tmp_path).returncode==1


def test_package_smoke_routes_to_the_same_body_core(tmp_path):
    from smoke_test_widget_game import smoke_test_game
    (tmp_path/'runtime').mkdir();(tmp_path/'runtime/widget.json').write_text(json.dumps(FIX['runtime']))
    (tmp_path/'component.json').write_text('{}')
    (tmp_path/'buttons.json').write_text(json.dumps([{'event':'button.sw1.short_press','action':'start'},{'event':'button.sw2.short_press','action':'kick'}]))
    result=smoke_test_game(tmp_path)
    assert result['ok'] and result['mode']=='same-firmware-bodies-core' and result['moved']
    assert result['deviceVerified'] is False
