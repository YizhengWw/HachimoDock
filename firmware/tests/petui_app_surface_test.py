"""App surface is a new bounded contract, not an exception to media validation."""
import copy
import json
import subprocess
import sys
import os
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from app_surface import app_surface_errors, persistence_errors

def layout():
    return {'engine':'p4-app-surface-v1','background':65535,'elements':[
        {'type':'panel','x':20,'y':20,'w':200,'h':60,'color':16000,'action':'go'},
        {'type':'text','x':30,'y':28,'w':180,'h':32,'color':0,'bind':'headline'},
        {'type':'meter','x':20,'y':90,'w':200,'h':10,'color':16000,'bind':'progress','page':3}]}

def graphics():
    return {'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','background':0,'images':[{'id':'background','asset':'assets/background.png','frame_width':640,'frame_height':480,'frames':1,'fps':1,'encoding':'jpeg'}],'elements':[
      {'type':'image','x':0,'y':0,'w':640,'h':480,'color':0,'image':'background'},
      {'type':'button','x':26,'y':366,'w':126,'h':48,'color':24000,'foreground':65535,'icon':'heart','text':'摸摸头','font_size':17,'radius':16,'padding':18,'gap':9,'stroke_width':1,'stroke':65535,'action':'go'},
      {'type':'button','x':26,'y':449,'w':32,'h':18,'color':0,'text':'SW1','font_size':11,'padding':2,'radius':4,'opacity':0,'stroke_width':1,'stroke':65535},
      {'type':'icon','x':26,'y':21,'w':21,'h':21,'color':65535,'icon':'home'},
      {'type':'panel','x':0,'y':0,'w':640,'h':115,'color':65535,'opacity':200,'opacity_end':0,'gradient':'vertical'}]}

def typography():
    s=graphics();s['typography']='p4-app-type-v1'
    s['images'].append({'id':'type0','asset':'assets/type0.png','frame_width':64,'frame_height':32,'frames':1,'fps':1})
    s['fonts']=[{'image':'type0','chars':'A家','metrics':[[19,5,28],[32,2,31]]}]
    s['elements'][2]['key_for']='go'
    return s

def interactive():
    s=typography();s['interaction']='p4-app-interaction-v1'
    s['images'].append({'id':'decor','asset':'assets/decor.png','frame_width':192,'frame_height':192,'frames':1,'fps':1})
    s['elements'].append({'type':'image','x':400,'y':100,'w':100,'h':100,'color':0,'image':'decor','tile':{'columns':3,'rows':3,'row':2,'var':'elapsed','max_var':'duration'},'show':{'var':'flowers','min':1,'max':1000000000}})
    return s

def test_python_and_real_c_surface_agree(tmp_path):
    source=tmp_path/'test.c'
    source.write_text('''#include "pet_p4_surface.h"
#include <stdio.h>
int main(int argc,char **argv){(void)argc;FILE *f=fopen(argv[1],"rb");char b[8192];size_t n=fread(b,1,sizeof(b)-1,f);b[n]=0;fclose(f);cJSON *j=cJSON_Parse(b);pet_surface_t s;int ok=pet_surface_parse(j,&s);cJSON_Delete(j);return ok?0:1;}
''')
    vendor=ROOT/'firmware/tests/vendor/cjson';exe=tmp_path/'parser'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    cases=[(layout(),True),(graphics(),True),(typography(),True),(interactive(),True)]
    hosted=graphics();hosted['elements'][1]['host_mask']=8;cases.append((hosted,True))
    for v in (0,256,True,1.5):
        bad=copy.deepcopy(hosted);bad['elements'][1]['host_mask']=v;cases.append((bad,False))
    bad=graphics();bad['elements'][0]['host_mask']=8;cases.append((bad,False))
    hd=typography();hd['fonts'][0]['cell']=[32,48];hd['images'][1]['frame_height']=48
    cases.append((hd,True))
    for cell in ([0,48],[32,0],[32,33],[48,32],[True,48],[32,48,64],[32.5,48],"32x48"):
        bad=copy.deepcopy(hd);bad['fonts'][0]['cell']=cell;cases.append((bad,False))
    bad=copy.deepcopy(hd);bad['images'][1]['frame_height']=32;cases.append((bad,False))
    for field,engine in [('data_view','p4-data-surface-v1'),('scene_view','p4-scene-surface-v1')]:
        valid=graphics();valid[field]={'engine':engine,'x':24,'y':70,'w':592,'h':330,'colors':[0,100,200,300,400]}
        valid[field].update({'layout':'todos'}if field=='data_view'else{'sprites':[]})
        cases.append((valid,True))
        for key,val in [('x',-1),('w',641),('h',12),('engine','script'),('colors',[0]*4),('colors',[0,0,0,0,True]),('colors',[0,0,0,0,65536]),('url','https://example.com')]:
            bad=copy.deepcopy(valid);bad[field][key]=val;cases.append((bad,False))
        bad=copy.deepcopy(valid);bad['game_view']={'engine':'p4-game-surface-v1','x':24,'y':80,'w':440,'h':320};cases.append((bad,False))
    game=graphics();game['game_view']={'engine':'p4-game-surface-v1','x':24,'y':80,'w':440,'h':320};cases.append((game,True))
    for key,value in [('engine','script'),('x',-1),('w',640),('h',0),('url','https://example.com')]:
        bad=copy.deepcopy(game);bad['game_view'][key]=value;cases.append((bad,False))
    for key,value in [('columns',0),('columns',5),('rows',9),('row',3),('var','../private'),('max_var',''),('script','run()')]:
        bad=interactive();bad['elements'][-1]['tile'][key]=value;cases.append((bad,False))
    for key,value in [('min',-1000000001),('max',0),('min',True),('var',''),('max_var','duration')]:
        bad=interactive();bad['elements'][-1]['show'][key]=value;cases.append((bad,False))
    bad=interactive();bad.pop('interaction');cases.append((bad,False))
    bad=interactive();bad['elements'][0]['show']={'var':'flowers','min':1,'max':10};cases.append((bad,False))
    for key,value in [('image','background'),('image','missing'),('chars','AA'),('chars','😀'),('chars','\n'),('chars',''),('chars','ABC'),('metrics',[[33,0,32],[32,0,32]]),('metrics',[[20,30,2],[32,0,32]]),('metrics',[[20,0,1e200],[32,0,32]]),('metrics',[[20,0,True],[32,0,32]])]:
        bad=typography();bad['fonts'][0][key]=value;cases.append((bad,False))
    for key,value in [('typography','unknown'),('fonts',[]),('fonts',[{'image':'type0','chars':'A','metrics':[[19,5,28]],'url':'https://example.com'}])]:
        bad=typography();bad[key]=value;cases.append((bad,False))
    for key,value in [('key_for','../go'),('action','go'),('bind','title'),('type','text')]:
        bad=typography();bad['elements'][2][key]=value;cases.append((bad,False))
    bad=typography();bad.pop('typography');cases.append((bad,False))
    bad=typography();bad['images'][1]['frame_width']=63;cases.append((bad,False))
    bad=typography();bad['fonts'].append(copy.deepcopy(bad['fonts'][0]));cases.append((bad,False))
    for field,v in [('page',5),('bind','private.key'),('action','../../file'),('motion','float'),('type','transport'),('script','x()'),('scale',0),('h',1),('rotate_ms',8000)]:
        bad=layout();bad['elements'][1][field]=v;cases.append((bad,False))
    for field,value in [('icon','arbitrary-svg'),('font_size',0),('font_size',97),('font_size',17.5),('line_height',10),('padding',80),('stroke_width',3),('align','justify'),('weight',500),('gradient','vertical'),('opacity_end',12),('image','background')]:
        bad=graphics();bad['elements'][1][field]=value;cases.append((bad,False))
    for field,value in [('motion','float'),('page',0),('x',1),('w',320)]:
        bad=graphics();bad['elements'][0][field]=value;cases.append((bad,False))
    bad=graphics();bad.pop('graphics');cases.append((bad,False))
    for field,value in [('frame_width',320),('encoding','webp'),('frame_height',481)]:
        bad=graphics();bad['images'][0][field]=value;cases.append((bad,False))
    if os.environ.get('PET_APP_PACKAGE_DIR'):
        for path in Path(os.environ['PET_APP_PACKAGE_DIR']).glob('*/runtime/widget.json'):
            s=json.loads(path.read_text()).get('surface',{})
            if s.get('engine')=='p4-app-surface-v1':cases.append((s,True))
    for s,ok in cases:
        path=tmp_path/'surface.json';path.write_text(json.dumps(s))
        assert (not app_surface_errors(s))==ok,s
        assert (subprocess.run([str(exe),str(path)]).returncode==0)==ok,s

def test_icon_tables_are_one_bounded_product_owned_set():
    from app_surface import ICONS
    icons=json.loads((ROOT/'pc/src/component-center/app-icons.json').read_text())
    assert set(icons)==ICONS
    for paths in icons.values():
        assert len(paths)<=12
        for points in paths:
            assert 2<=len(points)<=32
            assert all(len(p)==2 and all(0<=v<=24 for v in p) for p in points)
    subprocess.run(['node',str(ROOT/'pc/scripts/build-app-icons.mjs'),'--check'],check=True)

def test_native_punctuation_is_bounded_and_visible_at_all_font_sizes(tmp_path):
    source=tmp_path/'punctuation.c';exe=tmp_path/'punctuation'
    source.write_text('''#include "pet_p4_app_glyphs.h"
#include <assert.h>
int main(void) {
  unsigned cps[]={0xb7,0xb0,0x2103,0x2013,0x2014,0x2026,0x2212};
  assert(pet_app_symbol_advance('A')==0);
  for(unsigned i=0;i<sizeof(cps)/sizeof(cps[0]);i++){
    int width=pet_app_symbol_advance(cps[i]),top,bottom;
    pet_app_symbol_bounds(cps[i],&top,&bottom);
    assert(width>0&&width<=18&&top>=0&&bottom<16&&top<=bottom);
    for(int px=10;px<=96;px++){
      float scale=px/16.0f,ink=0;
      for(int y=0;y<px;y++)for(int x=0;x<(int)ceilf(width*scale);x++){
        float a=pet_app_symbol_coverage(cps[i],(x+.5f)/scale,(y+.5f)/scale,scale);
        assert(isfinite(a)&&a>=0&&a<=1);ink+=a;
      }
      assert(ink>0);
    }
  }
  assert(pet_app_symbol_coverage(0xb0,4,4,1)==0);
  assert(pet_app_symbol_coverage(0xb7,3,8,1)==1);
  return 0;
}''')
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),str(source),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

def test_action_references_pages_and_persistence():
    r={'surface':layout(),'pages':[{'id':str(i)} for i in range(4)],'transitions':[{'on':'go'}],'states':['idle','active','paused'],'vars':{'count':{'type':'int'}}}
    assert not app_surface_errors(r['surface'],r)
    r['transitions']=[];assert app_surface_errors(r['surface'],r)
    r['persist']={'version':1,'vars':['count'],'states':{'idle':'idle','active':'paused','paused':'paused'}}
    assert not persistence_errors(r)
    for k,v in [('version',True),('version',0),('vars',['count','count']),('vars',['missing']),('states',{'idle':'idle'}),('path','secret')]:
        bad=copy.deepcopy(r);bad['persist'][k]=v;assert persistence_errors(bad)
