"""p4-particles-v1: decorative fixed-step Q8 emitters. The native C core is
byte-exact against an independent Python reference and the preview WASM on the
shared simulation vectors; surface declarations follow the shared vectors on
every validator; the renderer draws dots only as an overlay above the cached
package layer; everything stays default-off. Offline only."""
import json
import os
from pathlib import Path
import subprocess
import sys
import pytest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from app_surface import app_surface_errors,particle_errors
FIX=json.loads((ROOT/'pc/skills/petui/contracts/particles-v1.json').read_text())
FNV=lambda h,n:((h^(n&0xffffffff))*16777619)&0xffffffff
STEP=20

# ---- independent reference of pet_p4_particles.c ----
def ref_sin_q8(deg):
    d=deg%360;neg=d>=180
    if neg:d-=180
    p=d*(180-d);v=(16*p*256)//(40500-p)
    return -v if neg else v
def trunc_div(n,d):
    q=abs(n)//d;return q if n>=0 else -q
class Ref:
    def __init__(s,c,seed):
        s.c=c;s.x=[0]*64;s.y=[0]*64;s.vx=[0]*64;s.vy=[0]*64;s.age=[0xFFFF]*64;s.rng=seed if seed else 0x70617274;s.accum=0;s.last=0;s.clocked=False
    def rnd(s):s.rng=(s.rng*1664525+1013904223)&0xffffffff;return s.rng>>8
    def alive(s):return sum(1 for a in s.age[:64] if a!=0xFFFF)
    def spawn(s):
        c=s.c
        for i in range(c['count']):
            if s.age[i]!=0xFFFF:continue
            rx=s.rnd();ry=s.rnd();ra=s.rnd()
            s.x[i]=(c['x']+rx%c['w'])*256+128;s.y[i]=(c['y']+ry%c['h'])*256+128
            deg=270-c['spread']//2+(ra%(c['spread']+1) if c['spread'] else 0)
            s.vx[i]=c['speed']*ref_sin_q8(deg+90);s.vy[i]=c['speed']*ref_sin_q8(deg);s.age[i]=0;return True
        return False
    def burst(s):
        n=0
        while n<s.c['count'] and s.spawn():n+=1
        return n
    def step_once(s,emitting):
        c=s.c;left=c['x']*256;right=(c['x']+c['w'])*256;top=c['y']*256;bottom=(c['y']+c['h'])*256
        for i in range(c['count']):
            if s.age[i]==0xFFFF:continue
            s.vy[i]+=trunc_div(c['gravity']*256*STEP,1000)
            s.x[i]+=trunc_div(s.vx[i]*STEP,1000);s.y[i]+=trunc_div(s.vy[i]*STEP,1000)
            age=s.age[i]+STEP
            if age>=c['life_ms'] or s.x[i]<left or s.x[i]>=right or s.y[i]<top or s.y[i]>=bottom:s.age[i]=0xFFFF
            else:s.age[i]=age
        if emitting and c['tick']:
            s.accum+=STEP
            while s.accum>=c['tick']:s.accum-=c['tick'];s.spawn()
    def step(s,now,emitting):
        if not s.clocked or now<s.last:s.last=now;s.clocked=True;return s.alive()
        steps=(now-s.last)//STEP
        if steps>8:steps=8;s.last=now-steps*STEP
        for _ in range(steps):s.step_once(emitting)
        s.last+=steps*STEP;return s.alive()
    def draw(s):
        out=[]
        for i in range(s.c['count']):
            if len(out)>=s.c['budget']:break
            if s.age[i]==0xFFFF:continue
            out.append([s.x[i]>>8,s.y[i]>>8,3 if s.c['shape']=='spark' else 1,s.c['color']])
        return out

def run_reference(vector):
    r=Ref(vector['config'],vector['seed']);trace=[]
    for ev in vector['events']:
        if ev.get('burst'):r.burst()
        alive=r.step(ev['now'],ev.get('emitting',True));dots=r.draw()
        h=2166136261
        for d in dots:
            for n in d:h=FNV(h,n)
        trace.append([alive,len(dots),h])
    return trace

HARNESS=r'''
#include "pet_p4_particles.h"
#include "pet_p4_surface.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t hash;static unsigned dots;static const pet_particles_config_t *active;
static void dot(void *ctx,int x,int y,int size,uint16_t color){
 (void)ctx;int v[4]={x,y,size,color};for(int i=0;i<4;i++)hash=(hash^(uint32_t)v[i])*16777619U;dots++;
 assert(x>=active->x&&x<active->x+active->w&&y>=active->y&&y<active->y+active->h);
}
int main(int argc,char **argv){
 assert(argc==3);FILE *f=fopen(argv[1],"rb");assert(f);static char text[65536];size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
 cJSON *j=cJSON_Parse(text);assert(j);int mode=atoi(argv[2]);
 if(mode==2){pet_surface_t s;int ok=pet_surface_parse_profile(j,&s,48,true);cJSON_Delete(j);return ok?0:1;}
 const cJSON *c=cJSON_GetObjectItem(j,"config");
 #define N(k) (cJSON_GetObjectItem(c,k)->valueint)
 pet_particles_config_t cfg={.x=N("x"),.y=N("y"),.w=N("w"),.h=N("h"),.color=N("color"),.life_ms=N("life_ms"),.tick_ms=N("tick"),.spread=N("spread"),.count=N("count"),.budget=N("budget"),.speed=N("speed"),.shape=strcmp(cJSON_GetObjectItem(c,"shape")->valuestring,"spark")?0:1,.gravity=N("gravity")};
 assert(pet_particles_valid(&cfg));active=&cfg;
 static pet_particles_state_t state;pet_particles_reset(&state,(uint32_t)cJSON_GetObjectItem(j,"seed")->valuedouble);
 const cJSON *ev;cJSON_ArrayForEach(ev,cJSON_GetObjectItem(j,"events")){
  const cJSON *b=cJSON_GetObjectItem(ev,"burst"),*e=cJSON_GetObjectItem(ev,"emitting");
  if(cJSON_IsTrue(b))pet_particles_burst(&cfg,&state);
  unsigned alive=pet_particles_step(&cfg,&state,(uint64_t)cJSON_GetObjectItem(ev,"now")->valuedouble,e?cJSON_IsTrue(e):true);
  hash=2166136261U;dots=0;unsigned emitted=pet_particles_draw(&cfg,&state,dot,NULL);assert(emitted==dots&&emitted<=cfg.budget);
  printf("%u %u %u\n",alive,dots,hash);
 }
 cJSON_Delete(j);return 0;
}
'''

@pytest.fixture(scope='module')
def native(tmp_path_factory):
    work=tmp_path_factory.mktemp('particles');src=work/'p.c';src.write_text(HARNESS);exe=work/'p'
    vendor=ROOT/'firmware/tests/vendor/cjson'
    r=subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-error=deprecated-declarations','-fsanitize=address,undefined','-DPET_P4_GRAPHICS_V2=1','-DPET_P4_PARTICLES=1','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(src),str(ROOT/'firmware/main/pet_p4_particles.c'),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],capture_output=True,text=True)
    assert r.returncode==0,r.stderr
    return work,exe

def run(native,payload,mode):
    work,exe=native;p=work/'in.json';p.write_text(json.dumps(payload))
    return subprocess.run([str(exe),str(p),str(mode)],capture_output=True,text=True,timeout=30)

def test_native_simulation_is_byte_exact_against_reference_and_contract(native):
    for vector in FIX['simulation']:
        r=run(native,vector,1);assert r.returncode==0,r.stderr
        trace=[[int(v) for v in l.split()] for l in r.stdout.strip().splitlines()]
        assert trace==run_reference(vector)==vector['trace'],vector['name']
        # Alive dots and drawn dots never exceed the declared count/budget.
        assert all(a<=vector['config']['count'] and d<=vector['config']['budget'] for a,d,_ in trace)

def test_wasm_core_matches_the_contract_trace(tmp_path):
    script=tmp_path/'p.mjs';script.write_text('''import {readFileSync} from 'node:fs';
import {createParticleCore} from '''+json.dumps((ROOT/'pc/skills/petui/scripts/canvas-core.mjs').as_uri())+''';
const fixture=JSON.parse(readFileSync(process.argv[2])),bytes=readFileSync(process.argv[3]);
for(const vector of fixture.simulation){const core=await createParticleCore(bytes);core.begin(0,vector.config,vector.seed);const out=[];
for(const ev of vector.events){if(ev.burst)core.burst(0);const alive=core.step(0,ev.now,ev.emitting!==false);const dots=core.dots(0);let hash=2166136261;for(const d of dots)for(const n of d)hash=Math.imul(hash^n,16777619)>>>0;out.push([alive,dots.length,hash]);}
console.log(vector.name+'|'+JSON.stringify(out));}
''')
    r=subprocess.run([os.environ.get('PET_FRAMEWORK_NODE','node'),str(script),str(ROOT/'pc/skills/petui/contracts/particles-v1.json'),str(ROOT/'pc/skills/petui/scripts/canvas-core.wasm')],check=True,capture_output=True,text=True,timeout=60)
    got={l.split('|')[0]:json.loads(l.split('|')[1]) for l in r.stdout.strip().splitlines()}
    for vector in FIX['simulation']:assert got[vector['name']]==vector['trace'],vector['name']

def test_shared_surface_vectors_agree_on_python_and_native(native):
    for s in FIX['valid']:
        assert not app_surface_errors(s,FIX['runtime']),s
        assert run(native,s,2).returncode==0,s
    for s in FIX['invalid']:
        assert app_surface_errors(s,FIX['runtime']),s
        if not particle_errors(s,None):continue  # runtime-only rejection (undeclared var/action/page) is the owner's job
        assert run(native,s,2).returncode!=0,s

def test_default_off_and_runtime_owner_hooks(tmp_path):
    policy=(ROOT/'firmware/main/pet_p4_render_policy.h').read_text()
    assert '#ifndef PET_P4_PARTICLES\n#define PET_P4_PARTICLES 0\n#endif\n#if PET_P4_PARTICLES && !PET_P4_GRAPHICS_V2\n#error' in policy
    src=tmp_path/'g.c';src.write_text('#include "pet_p4_surface.h"\nint main(void){return 0;}\n')
    bad=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_PARTICLES=1','-I',str(ROOT/'firmware/main'),'-I',str(ROOT/'firmware/tests/vendor/cjson'),str(src)],capture_output=True,text=True)
    assert bad.returncode!=0 and 'Particle emitters require the graphics v2 quota parent' in bad.stderr
    # Without the flag the surface parser refuses the key and the struct does not grow.
    vendor=ROOT/'firmware/tests/vendor/cjson';exe=tmp_path/'off'
    main=tmp_path/'off.c';main.write_text('#include "pet_p4_surface.h"\n#include <stdio.h>\nint main(int argc,char **argv){(void)argc;FILE*f=fopen(argv[1],"rb");char b[16384];size_t n=fread(b,1,sizeof(b)-1,f);b[n]=0;fclose(f);cJSON*j=cJSON_Parse(b);pet_surface_t s;int ok=pet_surface_parse(j,&s);cJSON_Delete(j);return ok?0:1;}\n')
    subprocess.run(['cc','-std=c11','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(main),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True,capture_output=True)
    p=tmp_path/'s.json';p.write_text(json.dumps(FIX['valid'][0]));assert subprocess.run([str(exe),str(p)]).returncode!=0
    protocol=(ROOT/'firmware/main/pet_p4_protocol.c').read_text()
    assert '#if PET_P4_PARTICLES\n  cJSON_AddStringToObject(capabilities,"widgetParticles","p4-particles-v1");\n#endif' in protocol
    miniapp=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text()
    assert 'particle guard must be a declared integer' in miniapp and 'particle trigger must be a declared action' in miniapp
    assert 'if(handled)pet_p4_renderer_note_action(action,g_image_revision);' in miniapp
    renderer=(ROOT/'firmware/main/pet_p4_renderer.c').read_text();glue=(ROOT/'firmware/main/pet_p4_capability_draw.inc').read_text()
    # Dots are an overlay after the cached package layer, never part of the frame cache or the retained logical frame.
    overlays=renderer[renderer.index('app_overlays:;'):renderer.index('app_overlays:;')+400]
    assert 'draw_app_particles(surface,app->widget_id,g_surface_revision,app->surface_page,now_ms);' in overlays
    assert '&&!PET_PARTICLES_ACTIVE(surface)' in renderer
    assert 'static EXT_RAM_BSS_ATTR pet_particles_state_t g_particle_state[PET_SURFACE_PARTICLE_EMITTERS];' in glue
    assert 'pet_particles_draw(&c,&g_particle_state[i],particle_dot,(void*)e)' in glue
    assert 'xTaskCreate' not in glue and 'malloc' not in glue.split('#if PET_P4_PARTICLES')[1]
    usb=(ROOT/'pc/src-tauri/src/usb_serial.rs').read_text();assert 'crate::app_component_program::validate(&widget_definition)?' in usb
