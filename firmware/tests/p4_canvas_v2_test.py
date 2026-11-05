"""p4-canvas-v2: 64 commands / 16 bound names / 8192 spans, polyline / arc / ring.
Byte-exact native raster against an independent Python reference, C/WASM parity on
the same vectors, v1 declarations unchanged on a v2 build, v2 declarations refused
without the flag, hello/validator/install negotiation. Offline only."""
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import pytest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from app_canvas import validate_canvas
V1=json.loads((ROOT/'pc/skills/petui/contracts/canvas-v1.json').read_text())
V2=json.loads((ROOT/'pc/skills/petui/contracts/canvas-v2.json').read_text())
FNV=lambda h,n:((h^(n&0xffffffff))*16777619)&0xffffffff

# ---- independent reference rasterizer (integer only, mirrors canvas.md) ----
def ref_angle(dx,dy):
    ax,ay=abs(dx),abs(dy)
    if not ax and not ay:return 0
    big,small=max(ax,ay),min(ax,ay)
    t=small*1024//big;u=1024-t
    q=45*t+((t*u//1024)*(14356+3891*t//1024))//1024
    a=q//1024
    if ay>ax:a=90-a
    if dx<0 and dy>=0:a=180-a
    elif dx<0 and dy<0:a=180+a
    elif dx>=0 and dy<0:a=360-a
    return a%360

def ref_raster(c,values):
    w,h=c['w'],c['h'];out=[]
    def val(v,lo,hi):
        n=values[v['var']] if isinstance(v,dict) else v
        return max(lo,min(hi,n))
    def emit(x,y,sw,sh,color):
        if x<0:sw+=x;x=0
        if y<0:sh+=y;y=0
        if x+sw>w:sw=w-x
        if y+sh>h:sh=h-y
        if sw<=0 or sh<=0:return
        out.append([c['x']+x,c['y']+y,sw,sh,color])
    def line(x,y,tx,ty,color):
        dx,dy=abs(tx-x),abs(ty-y);sx=1 if x<tx else -1;sy=1 if y<ty else -1;err=dx-dy
        for _ in range(max(w,h)):
            emit(x,y,1,1,color)
            if x==tx and y==ty:break
            twice=2*err
            if twice>-dy:err-=dy;x+=sx
            if twice<dx:err+=dx;y+=sy
    def arc_point(cx,cy,dx,dy,s,e,color):
        a=ref_angle(dx,dy)
        if (s<=a<=e) if s<=e else (a>=s or a<=e):emit(cx+dx,cy+dy,1,1,color)
    radius=min(w,h)//2
    for p in c['commands']:
        color=p['color']
        if p['op']=='polyline':
            pts=[(val(x,0,w-1),val(y,0,h-1)) for x,y in p['points']]
            for (x,y),(tx,ty) in zip(pts,pts[1:]):line(x,y,tx,ty,color)
            continue
        x=val(p['x'],0,w-1);y=val(p['y'],0,h-1)
        if p['op']=='rect':emit(x,y,val(p['w'],0,w),val(p['h'],0,h),color)
        elif p['op']=='line':line(x,y,val(p['x2'],0,w-1),val(p['y2'],0,h-1),color)
        elif p['op']=='circle':
            r=val(p['r'],0,radius)
            if not r:continue
            for row in range(-r,r+1):
                half=math.isqrt(r*r-row*row);emit(x-half,y+row,2*half+1,1,color)
        elif p['op']=='arc':
            r=val(p['r'],0,radius)
            if not r:continue
            s=val(p['start'],0,359);e=val(p['end'],0,359);cx,cy=x,y;px,py,err=r,0,1-r
            while px>=py:
                arc_point(cx,cy,px,py,s,e,color);arc_point(cx,cy,-px,py,s,e,color)
                if py:arc_point(cx,cy,px,-py,s,e,color);arc_point(cx,cy,-px,-py,s,e,color)
                if px!=py:
                    arc_point(cx,cy,py,px,s,e,color);arc_point(cx,cy,py,-px,s,e,color)
                    if py:arc_point(cx,cy,-py,px,s,e,color);arc_point(cx,cy,-py,-px,s,e,color)
                py+=1
                if err<0:err+=2*py+1
                else:px-=1;err+=2*(py-px)+1
        elif p['op']=='ring':
            r=val(p['r'],0,radius)
            if not r:continue
            inner=val(p['inner'],0,r-1)
            for row in range(-r,r+1):
                half=math.isqrt(r*r-row*row)
                if abs(row)<=inner:
                    hole=math.isqrt(inner*inner-row*row)
                    emit(x-half,y+row,half-hole,1,color);emit(x+hole+1,y+row,half-hole,1,color)
                else:emit(x-half,y+row,2*half+1,1,color)
    return out

def spans_hash(spans):
    h=2166136261
    for s in spans:
        for n in s:h=FNV(h,n)
    return h

HARNESS=r'''
#include "pet_p4_canvas_json.h"
#include "pet_p4_surface.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if PET_P4_CANVAS_V2
#define ANGLE(a,b) pet_canvas_angle(a,b)
#else
#define ANGLE(a,b) ((void)(a),(void)(b),0U)
#endif
static const pet_canvas_t *active;static unsigned calls;
static void emit(void *ctx,int x,int y,int w,int h,uint16_t color){
 FILE *out=ctx;calls++;assert(calls<=pet_canvas_span_limit(active));
 assert(x>=active->x&&y>=active->y&&w>0&&h>0&&x+w<=active->x+active->w&&y+h<=active->y+active->h);
 fprintf(out,"%d %d %d %d %u\n",x,y,w,h,(unsigned)color);
}
int main(int argc,char **argv){
 assert(argc==3);FILE *file=fopen(argv[1],"rb");assert(file);static char text[65536];size_t n=fread(text,1,sizeof(text)-1,file);text[n]=0;fclose(file);
 cJSON *fixture=cJSON_Parse(text);assert(fixture);int mode=atoi(argv[2]);
 if(mode==2){pet_surface_t s;int ok=pet_surface_parse(fixture,&s);cJSON_Delete(fixture);return ok?0:1;}
 if(mode==3){printf("%u %u %u\n",ANGLE(atoi(cJSON_GetObjectItem(fixture,"dx")->valuestring),atoi(cJSON_GetObjectItem(fixture,"dy")->valuestring)),(unsigned)sizeof(pet_canvas_t),(unsigned)PET_CANVAS_SPANS);cJSON_Delete(fixture);return 0;}
 pet_canvas_t c={0};bool ok=pet_canvas_parse(cJSON_GetObjectItem(fixture,"declaration"),&c);
 if(!ok){cJSON_Delete(fixture);return 1;}
 if(mode==1){
  const cJSON *values=cJSON_GetObjectItem(fixture,"values");
  for(unsigned i=0;i<c.variable_count;i++){const cJSON *v=cJSON_GetObjectItem(values,c.names[i]);assert(cJSON_IsNumber(v));c.values[i]=(int32_t)v->valuedouble;}
  active=&c;calls=0;unsigned count=pet_canvas_draw(&c,emit,stdout);assert(count==calls);
  fprintf(stderr,"%u\n",count);
 }
 cJSON_Delete(fixture);return 0;
}
'''

@pytest.fixture(scope='module')
def native(tmp_path_factory):
    work=tmp_path_factory.mktemp('canvas-v2');source=work/'canvas.c';source.write_text(HARNESS)
    vendor=ROOT/'firmware/tests/vendor/cjson';exe=work/'canvas'
    command=['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation','-fsanitize=address,undefined','-DPET_P4_CANVAS_V2=1','-DPET_P4_GRAPHICS_V2=1','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_canvas.c'),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)]
    result=subprocess.run(command,capture_output=True,text=True);assert result.returncode==0,result.stderr
    return work,exe

def run(native,payload,mode):
    work,exe=native;path=work/'input.json';path.write_text(json.dumps(payload))
    return subprocess.run([str(exe),str(path),str(mode)],text=True,capture_output=True,timeout=20)

def native_spans(native,declaration,values):
    r=run(native,{'declaration':declaration,'values':values},1);assert r.returncode==0,r.stderr
    spans=[[int(v) for v in l.split()] for l in r.stdout.strip().splitlines() if l.strip()]
    assert int(r.stderr.strip().splitlines()[-1])==len(spans)
    return spans

def values_for(vector):
    return {k:v for k,v in vector.get('values',{}).items()}

def test_shared_v2_vectors_parse_and_v1_vectors_keep_their_budgets(native):
    for c in V2['valid']:
        assert validate_canvas(c,V2['runtime']) is not None
        assert run(native,{'declaration':c},0).returncode==0,c
    for c in V2['invalid']:
        with pytest.raises((ValueError,KeyError,TypeError)):validate_canvas(c,V2['runtime'])
        try:validate_canvas(c)
        except (ValueError,KeyError,TypeError):assert run(native,{'declaration':c},0).returncode!=0,c
    # The v2 build still applies v1 numbers to v1 declarations.
    for c in V1['valid']:assert run(native,{'declaration':c},0).returncode==0
    for c in V1['invalid']:
        try:validate_canvas(c)
        except (ValueError,KeyError,TypeError):assert run(native,{'declaration':c},0).returncode!=0,c
    c=copy.deepcopy(V1['valid'][0]);c['commands']=[{'op':'rect','x':0,'y':0,'w':1,'h':1,'color':1}]*17
    assert run(native,{'declaration':c},0).returncode!=0
    c['engine']='p4-canvas-v2';assert run(native,{'declaration':c},0).returncode==0
    c['commands']=[{'op':'rect','x':0,'y':0,'w':1,'h':1,'color':1}]*65;assert run(native,{'declaration':c},0).returncode!=0

def test_native_raster_is_byte_exact_against_the_reference_and_the_contract(native):
    for vector in V2['raster']:
        values=values_for(vector);spans=native_spans(native,vector['declaration'],values)
        expected=ref_raster(vector['declaration'],values)
        assert spans==expected,vector['name']
        assert spans_hash(spans)==vector['fnv'] and len(spans)==vector['count'],vector['name']
    # Random bound values, every op, never a span outside the budget or the canvas.
    decl=next(v for v in V2['raster'] if v['name']=='all ops bound')['declaration']
    seed=1234
    for _ in range(200):
        values={}
        for name in V2['runtime']['vars']:
            seed=(seed*1664525+1013904223)&0xffffffff;values[name]=(seed>>8)%700-200
        assert native_spans(native,decl,values)==ref_raster(decl,values)

def test_arc_angles_and_shapes_have_the_documented_integer_semantics(native):
    for dx,dy in [(1,0),(0,1),(-1,0),(0,-1),(1,1),(-1,1),(-1,-1),(1,-1),(3,1),(1,3),(100,-7),(0,0)]:
        r=run(native,{'dx':str(dx),'dy':str(dy)},3);assert r.returncode==0
        angle,size,spans=(int(v) for v in r.stdout.split())
        assert angle==ref_angle(dx,dy)
        if dx or dy:assert abs(((angle-round(math.degrees(math.atan2(dy,dx))))%360+180)%360-180)<=1,(dx,dy,angle)
        else:assert angle==0
        assert size<=2048 and spans==8192
    base={'engine':'p4-canvas-v2','x':0,'y':0,'w':64,'h':64,'commands':[]}
    full=dict(base,commands=[{'op':'arc','x':32,'y':32,'r':20,'start':0,'end':359,'color':1}])
    spans=native_spans(native,full,{});pixels={(s[0],s[1]) for s in spans}
    assert len(spans)==len(pixels)<=6*20+8 and all(s[2]==1 and s[3]==1 for s in spans)
    assert all(abs(math.hypot(x-32,y-32)-20)<1.0 for x,y in pixels)
    half=dict(base,commands=[{'op':'arc','x':32,'y':32,'r':20,'start':0,'end':180,'color':1}])
    lower={(s[0],s[1]) for s in native_spans(native,half,{})}
    assert lower<pixels and all(y>=32 for _,y in lower) and (52,32) in lower and (12,32) in lower
    wrap=dict(base,commands=[{'op':'arc','x':32,'y':32,'r':20,'start':270,'end':90,'color':1}])
    right={(s[0],s[1]) for s in native_spans(native,wrap,{})}
    assert all(x>=32 for x,_ in right) and (52,32) in right and (12,32) not in right
    ring=dict(base,commands=[{'op':'ring','x':32,'y':32,'r':20,'inner':12,'color':1}])
    covered=set()
    for x,y,w,h,_ in native_spans(native,ring,{}):
        assert h==1
        for i in range(x,x+w):covered.add((i,y))
    for y in range(64):
        for x in range(64):
            d2=(x-32)**2+(y-32)**2
            assert ((x,y) in covered)==(d2<=400 and not (abs(y-32)<=12 and (x-32)**2<=144-(y-32)**2)),(x,y)
    poly=dict(base,commands=[{'op':'polyline','points':[[0,0],[63,0],[63,63],[0,63]],'color':7}])
    spans=native_spans(native,poly,{});corners={(0,0),(63,0),(63,63),(0,63)}
    assert corners<={(s[0],s[1]) for s in spans} and len(spans)==64*3

def test_v2_wasm_matches_native_and_the_skill_mirror_is_current(native,tmp_path):
    subprocess.run([os.environ.get('PET_FRAMEWORK_NODE','node'),str(ROOT/'pc/scripts/build-canvas-runtime.mjs'),'--check'],check=True,capture_output=True,timeout=60)
    script=tmp_path/'parity.mjs';script.write_text('''import {readFileSync} from 'node:fs';
import {createCanvasCore} from '''+json.dumps((ROOT/'pc/skills/petui/scripts/canvas-core.mjs').as_uri())+''';
const fixture=JSON.parse(readFileSync(process.argv[2])),bytes=readFileSync(process.argv[3]);
for(const vector of fixture.raster){const core=await createCanvasCore(bytes,vector.declaration,fixture.runtime);const spans=core.project(vector.values||{});
let hash=2166136261;for(const span of spans)for(const n of span)hash=Math.imul(hash^n,16777619)>>>0;console.log(vector.name+'|'+spans.length+'|'+hash);}
''')
    result=subprocess.run([os.environ.get('PET_FRAMEWORK_NODE','node'),str(script),str(ROOT/'pc/skills/petui/contracts/canvas-v2.json'),str(ROOT/'pc/skills/petui/scripts/canvas-core.wasm')],check=True,capture_output=True,text=True,timeout=30)
    lines=dict((l.split('|')[0],(int(l.split('|')[1]),int(l.split('|')[2]))) for l in result.stdout.strip().splitlines())
    for vector in V2['raster']:
        assert lines[vector['name']]==(vector['count'],vector['fnv']),vector['name']

def test_flag_default_off_refuses_v2_declarations_and_hello_replaces_the_string(tmp_path):
    policy=(ROOT/'firmware/main/pet_p4_render_policy.h').read_text();header=(ROOT/'firmware/main/pet_p4_canvas.h').read_text()
    assert '#ifndef PET_P4_CANVAS_V2\n#define PET_P4_CANVAS_V2 0\n#endif' in policy and '#define PET_CANVAS_ENABLED 0' in header
    protocol=(ROOT/'firmware/main/pet_p4_protocol.c').read_text()
    assert '#if PET_CANVAS_ENABLED\n  cJSON_AddStringToObject(capabilities,"widgetCanvas","p4-canvas-v1");\n#endif\n#if PET_P4_CANVAS_V2\n  cJSON_ReplaceItemInObject(capabilities,"widgetCanvas",cJSON_CreateString("p4-canvas-v2"));\n#endif' in protocol
    source=tmp_path/'v1only.c';source.write_text(HARNESS);vendor=ROOT/'firmware/tests/vendor/cjson';exe=tmp_path/'v1only'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-DPET_CANVAS_ENABLED=1','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_canvas.c'),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True,capture_output=True)
    path=tmp_path/'in.json'
    for c in V2['valid']:
        path.write_text(json.dumps({'declaration':c}));assert (subprocess.run([str(exe),str(path),'0'],capture_output=True).returncode==0)==(c['engine']=='p4-canvas-v1'),c
    for c in V1['valid']:
        path.write_text(json.dumps({'declaration':c}));assert subprocess.run([str(exe),str(path),'0'],capture_output=True).returncode==0
    path.write_text(json.dumps({'dx':'1','dy':'0'}));out=subprocess.run([str(exe),str(path),'3'],capture_output=True,text=True)
    assert out.returncode==0 and int(out.stdout.split()[2])==2048 and int(out.stdout.split()[1])<=512
    # Capability negotiation on every PC path: v1 packages accept v1 or v2 devices, v2 packages need v2.
    validator=(ROOT/'pc/skills/petui/scripts/validate_generated_widget.py').read_text()
    assert "canvas_capability_accepts(" in validator
    rust=(ROOT/'pc/src-tauri/src/app_canvas.rs').read_text()
    assert 'ENGINE_V2' in rust and 'fn require_capability' in rust
    assert 'crate::app_component_program::validate(&widget_definition)?' in (ROOT/'pc/src-tauri/src/usb_serial.rs').read_text()

def test_surface_with_v2_canvas_parses_only_with_bindings(native):
    surface={'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','bindings':'p4-variable-bindings-v1','background':0,'canvas':V2['valid'][0],
        'elements':[{'type':'text','x':0,'y':0,'w':200,'h':32,'color':65535,'text':'Canvas'}]}
    assert run(native,surface,2).returncode==0
    del surface['bindings'];assert run(native,surface,2).returncode!=0


def test_v2_cannot_disable_its_interpreter(tmp_path):
    source=tmp_path/"canvas-gate.c";source.write_text("#include \"pet_p4_canvas.h\"\nint main(void){return 0;}\n")
    args=["cc","-std=c11","-fsyntax-only","-I",str(ROOT/"firmware/main"),str(source)]
    for flags,ok in [([],True),(["-DPET_P4_CANVAS_V2=1"],True),(["-DPET_P4_CANVAS_V2=1","-DPET_CANVAS_ENABLED=0"],False)]:
        result=subprocess.run([*args,*flags],capture_output=True,text=True)
        assert (result.returncode==0)==ok,result.stderr
        if not ok:assert "Canvas v2 requires the bounded canvas interpreter" in result.stderr
