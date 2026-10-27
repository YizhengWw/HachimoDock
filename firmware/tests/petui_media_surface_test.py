"""The real C parser and Python package validator agree on bounded layouts."""
import json
import subprocess
import sys
import os
import copy
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from pc_extension import surface_errors, extension_errors
from validate_generated_widget import validate_widget

def test_surface_c_parser_matches_package_contract(tmp_path):
    source=tmp_path/'test.c'
    source.write_text('''#include "pet_p4_surface.h"
#include <stdio.h>
int main(int argc,char **argv){FILE *f=fopen(argv[1],"rb");if(!f)return 2;char buf[16384];size_t n=fread(buf,1,sizeof(buf)-1,f);buf[n]=0;fclose(f);cJSON *j=cJSON_Parse(buf);pet_surface_t out;int ok=pet_surface_parse(j,&out);cJSON_Delete(j);return ok?0:1;}
''')
    cfile=next((ROOT/'firmware/tests/vendor').rglob('cJSON.c'))
    exe=tmp_path/'surface-test'
    subprocess.run(['cc','-std=c11','-I',str(ROOT/'firmware/main'),'-I',str(cfile.parent),str(source),str(ROOT/'firmware/main/pet_p4_surface.c'),str(cfile),'-lm','-o',str(exe)],check=True)
    good=json.loads((ROOT/'pc/builtin-clawpkgs/city-radio/runtime/widget.json').read_text())['surface']
    other={'background':65535,'elements':[{'type':'text','x':10,'y':10,'w':500,'h':64,'color':0,'scale':2,'text':'另一种布局'}]}
    cases=[(good,True),(other,True)]
    if os.environ.get('PET_APP_PACKAGE_DIR'):
        latest=json.loads((Path(os.environ['PET_APP_PACKAGE_DIR'])/'city-radio/runtime/widget.json').read_text())['surface']
        cases.append((latest,True))
        for change in [
            lambda v:v.pop('input'),
            lambda v:v.update(input='arbitrary-input'),
            lambda v:next(n for n in v['elements'] if n.get('action')).update(action='shell.execute'),
        ]:
            bad=copy.deepcopy(latest);change(bad);cases.append((bad,False))
    for field,value in [('x',640),('scale',4),('color',-1),('bind','private.key'),('script','evil'),('h',1)]:
        bad=json.loads(json.dumps(other));bad['elements'][0][field]=value;cases.append((bad,False))
    for change in [
        lambda v:v["elements"][3].update(image="missing"),
        lambda v:v["elements"][3].update(rotate_ms=1),
        lambda v:v["elements"][3].update(w=200),
        lambda v:v["images"][0].update(asset="../secret.png"),
        lambda v:v["images"][0].update(frame_width=513),
        lambda v:v["images"][0].update(frames=2),
    ]:
        bad=json.loads(json.dumps(good));change(bad);cases.append((bad,False))
    for i,(layout,expected) in enumerate(cases):
        path=tmp_path/f'{i}.json';path.write_text(json.dumps(layout))
        assert (not surface_errors(layout))==expected
        assert (subprocess.run([str(exe),str(path)]).returncode==0)==expected

def test_extension_and_complete_package_are_valid():
    package=ROOT/'pc/builtin-clawpkgs/city-radio'
    assert not validate_widget(package)
    ext=json.loads((package/'pc/extension.json').read_text())
    assert not extension_errors(ext)
    ext['search']['endpoint']='https://undeclared.example/search'
    assert extension_errors(ext)
    ext['script']='shell()'
    assert extension_errors(ext)
