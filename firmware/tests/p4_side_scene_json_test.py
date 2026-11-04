"""Compare strict Python and production C parsing of the same side-scene data."""
import copy
import json
import os
import subprocess
import sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from side_scene import side_scene_errors

def declaration():
    scene={'engine':'p4-side-scene-v1','width':20,'height':8,
        'rows':['0'*20]*6+['1'*20]*2,'tiles':[{'flags':0},{'flags':1}],
        'actors':[{'id':'player','behavior':'controlled','contact':'none','x':32,'y':80,'width':12,'height':16,'skin':0}],
        'physics':{'run_speed':96,'acceleration':800,'friction':1000,'gravity':800,'jump_speed':280,'terminal_speed':480,'lives':3},
        'actions':{'left':'move.left','right':'move.right','jump':'player.jump','pause':'game.pause'},
        'active_state':'playing','result_state':'lost','win_state':'won',
        'score_var':'score','coins_var':'coins','lives_var':'lives','checkpoint_var':'checkpoint',
        'art':{'image':'world','columns':2,'rows':1,'scale':2,'x':0,'y':48,'w':640,'h':416,'sky':0x6bdf,'tiles':[0,1],
            'skins':[{'idle':[0],'walk':[0],'air':[0],'hurt':[0],'fps':8}]}}
    return {'engine':'p4-bounded-runtime-v4','scene':scene,'tick':[],'initial_state':'ready',
        'vars':{v:{'type':'int','init':0}for v in ('score','coins','lives','checkpoint')},
        'states':['ready','playing','paused','lost','won'],
        'input':{'engine':'p4-input-v1','hold':'player.jump'},
        'surface':{'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','images':[{'id':'world','frame_width':32,'frame_height':16}]},
        'transitions':[{'from':'ready','on':'player.jump','to':'playing'},{'from':'playing','on':'move.left'},{'from':'playing','on':'move.right'},{'from':'playing','on':'game.pause','to':'paused'},{'from':'paused','on':'game.pause','to':'playing'}]}

def test_strict_c_and_python_side_declaration(tmp_path):
    source=tmp_path/'parse.c';exe=tmp_path/'parse';vendor=ROOT/'firmware/tests/vendor/cjson'
    source.write_text('''#include "pet_p4_side_scene_json.h"
#include <stdio.h>
#include <stdlib.h>
static pet_side_declaration_t declaration;
int main(int argc,char**argv){(void)argc;char data[16384],error[128];FILE*f=fopen(argv[1],"rb");if(!f)return 2;size_t n=fread(data,1,sizeof(data)-1,f);fclose(f);data[n]=0;cJSON*j=cJSON_Parse(data);int ok=pet_side_parse(cJSON_GetObjectItemCaseSensitive(j,"scene"),&declaration,error,sizeof(error));cJSON_Delete(j);return ok?0:1;}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-error=deprecated-declarations','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_side_scene.c'),str(ROOT/'firmware/main/pet_p4_side_scene_json.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    base=declaration();cases=[(base,True)]
    changes=[('/width',161),('/height',7),('/rows/0','0'*19),('/rows/0','z'*20),('/tiles/1/flags',True),('/actors/0/behavior','script'),('/actors/0/x',319),('/actors/0/y',96),('/actors/0/skin',1),('/physics/gravity',0),('/physics/lives',True),('/actions/jump','move.left'),('/art/columns',33),('/art/scale',0),('/art/tiles/1',2),('/art/skins/0/fps',21),('/art/skins/0/idle',[]),('/engine','unknown')]
    for pointer,value in changes:
        r=copy.deepcopy(base);owner=r['scene'];parts=pointer.strip('/').split('/')
        for part in parts[:-1]:owner=owner[int(part)] if isinstance(owner,list) else owner[part]
        owner[int(parts[-1]) if isinstance(owner,list) else parts[-1]]=value;cases.append((r,False))
    for r,valid in cases:
        assert (not side_scene_errors(r))==valid,r
        path=tmp_path/'scene.json';path.write_text(json.dumps(r))
        assert (subprocess.run([str(exe),str(path)]).returncode==0)==valid,r
    r=copy.deepcopy(base);r['scene']['script']='run()';path.write_text(json.dumps(r));assert subprocess.run([str(exe),str(path)]).returncode==1
    raw=json.dumps(base).replace('"width": 20','"width":20,"width":20',1);path.write_text(raw);assert subprocess.run([str(exe),str(path)]).returncode==1
    if os.environ.get('PET_SIDE_PACKAGE'):
        r=json.loads((Path(os.environ['PET_SIDE_PACKAGE'])/'runtime/widget.json').read_text());assert not side_scene_errors(r)
        path.write_text(json.dumps(r));subprocess.run([str(exe),str(path)],check=True)

def test_side_scene_requires_capability_and_precise_bindings():
    r=declaration();assert not side_scene_errors(r,{'widgetSideScene':'p4-side-scene-v1'})
    assert side_scene_errors(r,{})
    r['scene']['score_var']='checkpoint';assert side_scene_errors(r)
    r=declaration();r['surface']['images'][0]['frame_width']=16;assert side_scene_errors(r)
