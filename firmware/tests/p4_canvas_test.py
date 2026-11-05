"""Actual native decoder/raster, C/WASM parity and strict declaration budgets.
No device, user storage, audio, credentials, network, or installed component.
"""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import pytest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from app_canvas import validate_canvas
from app_surface import app_surface_errors
FIXTURE=json.loads((ROOT/'pc/skills/petui/contracts/canvas-v1.json').read_text())

@pytest.fixture(scope='module')
def native(tmp_path_factory):
    work=tmp_path_factory.mktemp('canvas-core');source=work/'canvas.c';exe=work/'canvas'
    source.write_text(r'''
#include "pet_p4_canvas_json.h"
#include "pet_p4_surface.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
static const pet_canvas_t *active;static unsigned calls;static uint32_t hash;
static void emit(void *ctx,int x,int y,int w,int h,uint16_t color){
 (void)ctx;assert(++calls<=PET_CANVAS_SPANS);assert(x>=active->x&&y>=active->y&&w>0&&h>0&&x+w<=active->x+active->w&&y+h<=active->y+active->h);
 int data[]={x,y,w,h,color};for(unsigned i=0;i<5;i++)hash=(hash^(uint32_t)data[i])*16777619U;
}
int main(int argc,char **argv){
 assert(sizeof(pet_canvas_t)<=512);
 assert(argc==3);FILE *file=fopen(argv[1],"rb");assert(file);char text[32768];size_t n=fread(text,1,sizeof(text)-1,file);text[n]=0;fclose(file);
 cJSON *fixture=cJSON_Parse(text);assert(fixture);int mode=atoi(argv[2]);
 if(mode==2){pet_surface_t s;int ok=pet_surface_parse(fixture,&s);cJSON_Delete(fixture);return ok?0:1;}
 pet_canvas_t c={0};bool ok=pet_canvas_parse(fixture,&c);
 if(!ok){cJSON_Delete(fixture);return 1;}
 // Surface parser itself does not know runtime var types: checked at install
 // and actual miniapp parsing. Keep shape-only versus typed tests distinct.
 if(mode==1){active=&c;calls=0;hash=2166136261U;
  for(unsigned i=0;i<c.variable_count;i++)c.values[i]=(int32_t)((i+1)*3);
  unsigned count=pet_canvas_draw(&c,emit,NULL);assert(count==calls);printf("%u %u\n",count,hash);
  pet_canvas_t bad=c;bad.commands[0].op=255;calls=0;assert(!pet_canvas_draw(&bad,emit,NULL)&&!calls);
  c.commands[0].x=-1;c.commands[0].y=-2;c.commands[0].a=-3;c.commands[0].b=-4;
  uint32_t random=123;for(unsigned pass=0;pass<1000;pass++){
   for(unsigned i=0;i<c.variable_count;i++){random=random*1664525U+1013904223U;c.values[i]=(int32_t)random;}
   active=&c;calls=0;hash=2166136261U;assert(pet_canvas_draw(&c,emit,NULL)==calls);
  }
 }
 cJSON_Delete(fixture);return 0;
}
''')
    vendor=ROOT/'firmware/tests/vendor/cjson'
    command=['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation','-fsanitize=address,undefined','-DPET_CANVAS_ENABLED=1','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_canvas.c'),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)]
    result=subprocess.run(command,capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    return work,exe

def run(native,value,mode=0):
    work,exe=native;path=work/'input.json';path.write_text(json.dumps(value));return subprocess.run([str(exe),str(path),str(mode)],text=True,capture_output=True,timeout=10)

def test_shared_shapes_and_runtime_binding_types(native):
    for c in FIXTURE['valid']:
        assert validate_canvas(c,FIXTURE['runtime']) is not None
        assert run(native,c).returncode==0
    for c in FIXTURE['invalid']:
        with pytest.raises((ValueError,KeyError,TypeError)):validate_canvas(c,FIXTURE['runtime'])
        # Undeclared/wrongly typed variables are checked by actual miniapp and
        # native Rust/Python installers, not the runtime-independent decoder.
        try:validate_canvas(c)
        except (ValueError,KeyError,TypeError):assert run(native,c).returncode!=0
    source=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text()
    assert 'canvas binding must be a declared integer' in source
    # Canvas now resolves from the SAME immutable host reply as the rest of
    # its frame, not even a separate all-integer live read.
    resolver=source.split('static bool app_surface_locked(',1)[1].split('bool pet_p4_miniapp_media_config(',1)[0]
    assert 'surface->canvas.names[i],true,NULL,&surface->canvas.values[i]' in resolver
    assert 'pet_app_host_integer_snapshot(' not in resolver
    assert resolver.count('pet_app_host_render_capture(')==1
    assert 'canvas.count&&!pet_form_source_valid(widget_json)' in source

def test_canvas_budget_and_surface_profile(native):
    c=copy.deepcopy(FIXTURE['valid'][0]);c['w']=320;c['commands']=[{'op':'line','x':0,'y':0,'x2':1,'y2':1,'color':1}]*7
    with pytest.raises(ValueError):validate_canvas(c)
    assert run(native,c).returncode!=0
    c['commands'].pop();validate_canvas(c);assert run(native,c).returncode==0
    c['commands']=[{'op':'rect','x':0,'y':0,'w':1,'h':1,'color':1}]*17
    assert run(native,c).returncode!=0
    surface={'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','bindings':'p4-variable-bindings-v1','background':0,'canvas':FIXTURE['valid'][0],
        'elements':[{'type':'text','x':0,'y':0,'w':200,'h':32,'color':65535,'text':'Canvas'}]}
    assert not app_surface_errors(surface,FIXTURE['runtime']);assert run(native,surface,2).returncode==0
    del surface['bindings'];assert app_surface_errors(surface);assert run(native,surface,2).returncode!=0

def test_native_sanitized_raster_and_wasm_match(native,tmp_path):
    c=FIXTURE['valid'][0];actual=run(native,c,1);assert actual.returncode==0,actual.stderr
    expected=actual.stdout.strip().split()
    script=tmp_path/'parity.mjs';script.write_text('''import {readFileSync} from 'node:fs';
import {createCanvasCore} from '''+json.dumps((ROOT/'pc/src/component-center/canvas-core.js').as_uri())+''';
const fixture=JSON.parse(readFileSync(process.argv[2])),core=await createCanvasCore(readFileSync(process.argv[3]),fixture.valid[0]);
const vars=Object.fromEntries(core.bindings.map((name,i)=>[name,(i+1)*3]));const spans=core.project(vars);
let hash=2166136261;for(const span of spans)for(const n of span)hash=Math.imul(hash^n,16777619)>>>0;
console.log(spans.length,hash);
''')
    result=subprocess.run([os.environ.get('PET_FRAMEWORK_NODE','node'),str(script),str(ROOT/'pc/skills/petui/contracts/canvas-v1.json'),str(ROOT/'pc/src/component-center/canvas-core.wasm')],check=True,capture_output=True,text=True,timeout=10)
    assert result.stdout.strip().split()==expected

def test_canvas_stays_default_off():
    header=(ROOT/'firmware/main/pet_p4_canvas.h').read_text();assert '#define PET_CANVAS_ENABLED 0' in header
    protocol=(ROOT/'firmware/main/pet_p4_protocol.c').read_text();assert '#if PET_CANVAS_ENABLED\n  cJSON_AddStringToObject(capabilities,"widgetCanvas","p4-canvas-v1");\n#endif' in protocol
    usb=(ROOT/'pc/src-tauri/src/usb_serial.rs').read_text();assert 'crate::app_component_program::validate(&widget_definition)?' in usb

def test_default_surface_rejects_canvas_without_changing_legacy(tmp_path):
    source=tmp_path/'default.c';binary=tmp_path/'default'
    source.write_text(r'''#include "pet_p4_surface.h"
#include <stdio.h>
int main(int argc,char **argv){(void)argc;FILE*f=fopen(argv[1],"rb");if(!f)return 2;char text[8192];size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);cJSON*j=cJSON_Parse(text);pet_surface_t surface;int ok=pet_surface_parse(j,&surface);cJSON_Delete(j);return ok?0:1;}''')
    vendor=ROOT/'firmware/tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True,capture_output=True,timeout=30)
    path=tmp_path/'surface.json';value={'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','bindings':'p4-variable-bindings-v1','background':0,'elements':[{'type':'panel','x':0,'y':0,'w':100,'h':100,'color':0}]}
    for present in (False,True,False):
        if present:value['canvas']=FIXTURE['valid'][0]
        else:value.pop('canvas',None)
        path.write_text(json.dumps(value))
        assert subprocess.run([str(binary),str(path)],timeout=10).returncode==(1 if present else 0)

def test_wasm_and_adapter_mirrors_are_current():
    subprocess.run([os.environ.get('PET_FRAMEWORK_NODE','node'),str(ROOT/'pc/scripts/build-canvas-runtime.mjs'),'--check'],check=True,capture_output=True,timeout=30)

def test_exact_frame_cache_includes_canvas_geometry_values_and_readiness(tmp_path):
    source=tmp_path/'cache.c';binary=tmp_path/'cache'
    source.write_text(r'''#include "pet_p4_app_cache.h"
#include <assert.h>
static pet_app_frame_key_t key;
int main(void){pet_surface_t surface={0};surface.app=true;surface.canvas_bindings_ready=true;surface.canvas.w=64;surface.canvas.h=48;surface.canvas.count=1;surface.canvas.variable_count=1;surface.canvas.values[0]=9;const char*values[9]={0};
pet_app_frame_remember(&key,&surface,values,0,0);assert(pet_app_frame_matches(&key,&surface,values,0,0));
pet_surface_t next=surface;next.canvas.values[0]=10;assert(!pet_app_frame_matches(&key,&next,values,0,0));
next=surface;next.canvas_bindings_ready=false;assert(!pet_app_frame_matches(&key,&next,values,0,0));
next=surface;next.canvas.commands[0].color=65535;assert(!pet_app_frame_matches(&key,&next,values,0,0));
next=surface;next.canvas.page=1;assert(!pet_app_frame_matches(&key,&next,values,0,0));return 0;}''')
    subprocess.run(['cc','-std=c11','-DPET_CANVAS_ENABLED=1','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),'-I',str(ROOT/'firmware/tests/vendor/cjson'),str(source),'-o',str(binary)],check=True,capture_output=True,timeout=30)
    subprocess.run([str(binary)],check=True,timeout=10)
