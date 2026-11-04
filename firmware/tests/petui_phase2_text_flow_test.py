"""Real C line layout and real surface copy preserve every bounded UTF-8 line."""
import ctypes as C
import json
import subprocess
from pathlib import Path
import pytest
ROOT=Path(__file__).resolve().parents[2];FW=ROOT/'firmware';VENDOR=FW/'tests/vendor/cjson'

class Line(C.Structure):_fields_=[('start',C.c_uint16),('end',C.c_uint16)]

@pytest.fixture(scope='module')
def core(tmp_path_factory):
    tmp=tmp_path_factory.mktemp('flow-byte-bound');src=tmp/'flow.c';lib=tmp/'flow.dylib'
    src.write_text(r'''
#include <stdlib.h>
#include "pet_p4_surface.h"
#include "pet_p4_text_flow.h"
unsigned flow(const char*t,unsigned w,unsigned f,pet_text_line_t*l,unsigned cap,bool*more){return pet_text_flow_lines(t,w,f,l,cap,more);}
unsigned copy_line(const char*t,const pet_text_line_t*l,char*out){pet_text_flow_copy(t,l,out,121);return (unsigned)strlen(out);}
unsigned surface_page(const char*t,unsigned w,unsigned font,unsigned page,char*out){
 pet_surface_t*s=calloc(1,sizeof(*s));if(!s)return 0;s->text_count=1;s->count=1;
 pet_surface_longtext_t*l=&s->texts[0];l->used=true;l->w=w;l->font_size=font;l->line_height=20;l->lines_per_page=1;
 memcpy(l->text,t,strlen(t)+1);s->nodes[0].text_id=0;s->nodes[0].type=1;
 unsigned pages=pet_surface_resolve_longtext(s,0,(int32_t)page);memcpy(out,s->nodes[0].text,121);free(s);return pages;
}
''')
    subprocess.run(['cc','-std=c11','-shared','-fPIC','-DPET_P4_LONGTEXT=1','-I'+str(FW/'main'),'-I'+str(VENDOR),str(src),str(FW/'main/pet_p4_surface.c'),str(FW/'main/pet_p4_canvas.c'),str(VENDOR/'cJSON.c'),'-lm','-o',str(lib)],check=True)
    loaded=C.CDLL(str(lib));loaded.flow.argtypes=[C.c_char_p,C.c_uint,C.c_uint,C.POINTER(Line),C.c_uint,C.POINTER(C.c_bool)];loaded.flow.restype=C.c_uint
    loaded.copy_line.argtypes=[C.c_char_p,C.POINTER(Line),C.c_void_p];loaded.copy_line.restype=C.c_uint
    loaded.surface_page.argtypes=[C.c_char_p,C.c_uint,C.c_uint,C.c_uint,C.c_void_p];loaded.surface_page.restype=C.c_uint
    return loaded

@pytest.mark.parametrize('text',[
    'á'*60,'á'*60+'x','á'*80,'á'*120,'ࠀ'*80,'😀'*60,
    'a '+'ࠀ'*39+'bࠀ','á'*59+'ࠀ','😀'*29+'ááá','á'*60+'\n'+'😀'*31,
])
def test_byte_ceiling_copy_and_real_surface_pages_preserve_tail(core,text):
    raw=text.encode();lines=(Line*256)();more=C.c_bool();count=core.flow(raw,640,16,lines,256,C.byref(more));assert not more.value
    strings=[];page_strings=[]
    for i in range(count):
        expected=raw[lines[i].start:lines[i].end];assert len(expected)<=120;expected.decode('utf-8')
        out=C.create_string_buffer(122);out[121]=b'Z'
        assert core.copy_line(raw,C.byref(lines[i]),out)==len(expected)
        assert out.value==expected and out[121]==b'Z'
        pages=C.create_string_buffer(121);assert core.surface_page(raw,640,16,i,pages)==count
        assert pages.value==expected;strings.append(out.value.decode());page_strings.append(pages.value.decode())
    # Space/newline normalization is the existing contract, not loss of glyphs.
    assert ''.join(strings).replace(' ','')==text.replace(' ','').replace('\n','')
    assert strings==page_strings
    if text=='á'*80:assert strings==['á'*60,'á'*20]
    if text=='a '+'ࠀ'*39+'bࠀ':assert strings==['a','ࠀ'*39+'b','ࠀ']


def test_shared_existing_and_new_flow_vectors(core):
    suite=json.loads((ROOT/'pc/skills/petui/contracts/longtext-v1.json').read_text())
    for case in suite['flow']:
        raw=case['text'].encode();out=(Line*256)();more=C.c_bool()
        count=core.flow(raw,case['width'],case['font_size'],out,case.get('max_lines',256),C.byref(more))
        assert [raw[out[i].start:out[i].end].decode()for i in range(count)]==case['lines'],case
        assert more.value==case['truncated']


def test_text_flow_boundaries_under_actual_c_sanitizers(tmp_path):
    src=tmp_path/'bounds.c';exe=tmp_path/'bounds'
    src.write_text(r'''
#include <assert.h>
#include "pet_p4_text_flow.h"
int main(void){char text[4097]={0};for(unsigned i=0;i<1024;i++){text[i*4]=(char)0xf0;text[i*4+1]=(char)0x9f;text[i*4+2]=(char)0x98;text[i*4+3]=(char)0x80;}
 assert(pet_text_flow_valid(text,4096));pet_text_line_t lines[256];bool more=false;
 unsigned count=pet_text_flow_lines(text,640,16,lines,256,&more);assert(!more&&count==35);
 unsigned total=0;for(unsigned i=0;i<count;i++){char out[122]={0};out[121]='Z';pet_text_flow_copy(text,&lines[i],out,121);assert(out[121]=='Z'&&strlen(out)<=120);total+=strlen(out);}
 assert(total==4096);pet_text_flow_copy(text,&lines[0],NULL,0);
 assert(pet_text_flow_lines(text,640,16,lines,1,&more)==1&&more);return 0;}
''')
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I'+str(FW/'main'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
