"""D-line pure contract regressions. No device, network or package build."""
import copy
import json
import sys
from pathlib import Path
import pytest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
import pc_fetch
import app_shell
import app_form_v3
import data_rows
import text_flow
import notify_policy

def suite(name):
    return json.loads((ROOT/f'pc/skills/petui/contracts/{name}.json').read_text())

def patch(r,c):
    r=copy.deepcopy(r);p=r
    for k in c['path'][:-1]:p=p[k]
    p[c['path'][-1]]=c['value']
    return r

@pytest.mark.parametrize('name,module',[('app-shell-v1',app_shell),('app-form-v3',app_form_v3),('data-rows-v1',data_rows),('notify-v1',notify_policy)])
def test_runtime_contracts(name,module):
    s=suite(name)
    assert not module.errors(s['runtime'])
    # Some invalid vectors exercise surface/whole-runtime checks, tested separately.
    assert sum(bool(module.errors(patch(s['runtime'],c))) for c in s['invalid'])>0

def test_longtext_layout_vectors():
    for c in suite('longtext-v1')['flow']:
        assert text_flow.lines(c['text'],c['width'],c['font_size'],c.get('max_lines',256))==(c['lines'],c['truncated'])

def test_fetch_untrusted_url_pointer_scalar_and_first_error():
    for u in ['https://example.org:bad','https://example.org:444','https://example.org/?%74oken=x','https://u:p@example.org/','https://127.0.0.1/']:
        assert not pc_fetch.host_allowed(u,['example.org'])
    assert pc_fetch.resolve_pointer({'x':[1]},'/x/01') is None
    assert pc_fetch.resolve_pointer({'a/b':{'~':7}},'/a~1b/~0')==7
    assert pc_fetch.scalar_for('a\n\x7fb','string')=='ab'
    assert pc_fetch.scalar_for(float('nan'),'string') is None
    assert pc_fetch.scalar_for(True,'int') is None
    assert pc_fetch.snapshot('feed',{'error':'failed'},0)['status']=='error'
    assert pc_fetch.snapshot('feed',{},0)['message']=='等待数据'
    assert pc_fetch.snapshot('feed',{'rows':[],'fetched_ms':0,'ttl_ms':60000},60001)['message']=='数据已过期'

def test_fetch_mapping_is_bounded_and_never_invents_rows():
    mapping={'items':'/items','id':'/id','label':'/label'}
    assert pc_fetch.map_rows({},mapping) is None
    assert pc_fetch.map_rows({'items':[]},mapping)==[]
    items=[{'id':str(i),'label':'中'*30} for i in range(30)]
    rows=pc_fetch.map_rows({'items':items},mapping)
    assert len(rows)==20 and len(rows[0]['label'].encode())==36
    assert len(pc_fetch.map_rows({'items':[items[0],items[0],{'id':''}]},mapping))==1

def test_c_layout_and_pinyin_match_python(tmp_path):
    import ctypes as C
    import subprocess
    source=tmp_path/'d_core.c';library=tmp_path/'d_core.dylib'
    source.write_text('''#include "pet_p4_text_flow.h"
#include "pet_p4_form_v3.h"
#include "pet_p4_form_v3_json.h"
int parse_form(const char*s,const char*b){cJSON*r=cJSON_Parse(s),*buttons=cJSON_Parse(b);pet_form3_t f;int ok=pet_form3_parse(r,buttons,&f);cJSON_Delete(r);cJSON_Delete(buttons);return ok;}
unsigned flow(const char *s,unsigned w,unsigned f,pet_text_line_t *out,unsigned cap,bool *more){return pet_text_flow_lines(s,w,f,out,cap,more);}
''')
    subprocess.run(['cc','-std=c11','-shared','-fPIC','-O1','-I',str(ROOT/'firmware/main'),'-I',str(ROOT/'firmware/tests/vendor/cjson'),str(source),str(ROOT/'firmware/main/pet_p4_form_v3.c'),str(ROOT/'firmware/main/pet_p4_form_v2.c'),str(ROOT/'firmware/tests/vendor/cjson/cJSON.c'),'-o',str(library)],check=True)
    lib=C.CDLL(str(library))
    lib.parse_form.argtypes=[C.c_char_p,C.c_char_p];lib.parse_form.restype=C.c_int
    fixture=suite('app-form-v3');r=copy.deepcopy(fixture['runtime']);r['quota']='p4-quota-v3'
    for i in range(len(r['vars']),32):r['vars'][f'extra{i}']={'type':'string','init':'x'*127}
    assert lib.parse_form(json.dumps(r).encode(),json.dumps(fixture['buttons']).encode())==1
    q=r['form']['fields'][0];r['vars'][q['var']]['init']='x'*64
    assert lib.parse_form(json.dumps(r).encode(),json.dumps(fixture['buttons']).encode())==0

    class Line(C.Structure):_fields_=[('start',C.c_uint16),('end',C.c_uint16)]
    lib.flow.argtypes=[C.c_char_p,C.c_uint,C.c_uint,C.POINTER(Line),C.c_uint,C.POINTER(C.c_bool)];lib.flow.restype=C.c_uint
    for c in suite('longtext-v1')['flow']:
        raw=c['text'].encode();out=(Line*256)();more=C.c_bool()
        count=lib.flow(raw,c['width'],c['font_size'],out,c.get('max_lines',256),C.byref(more))
        assert [raw[out[i].start:out[i].end].decode() for i in range(count)]==c['lines']
        assert more.value==c['truncated']
    lib.pet_form3_candidates.argtypes=[C.c_char_p,C.POINTER(C.c_uint16),C.c_uint];lib.pet_form3_candidates.restype=C.c_uint
    for digits in ['2','23','94664','999999','0','2345678']:
        out=(C.c_uint16*64)();count=lib.pet_form3_candidates(digits.encode(),out,64)
        assert [chr(out[i]) for i in range(count)]==app_form_v3.candidates(digits)

def test_form_quota_v3_keeps_unrelated_long_strings_and_rejects_owned_overflow():
    r=copy.deepcopy(suite('app-form-v3')['runtime']);r['quota']='p4-quota-v3'
    for i in range(len(r['vars']),32):r['vars'][f'extra{i}']={'type':'string','init':'x'*127}
    assert not app_form_v3.errors(r)
    q=r['form']['fields'][0];r['vars'][q['var']]['init']='x'*64;r['vars'][q['draft_var']]['init']='x'*64
    assert app_form_v3.errors(r)

def test_shell_canvas_is_explicitly_unsupported():
    r=copy.deepcopy(suite('app-shell-v1')['runtime']);r['surface']['canvas']={}
    assert app_shell.errors(r)
