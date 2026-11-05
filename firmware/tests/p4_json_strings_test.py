"""Actual source gates and shared string vectors; no device or user records."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import pytest
from p4_render_trace_test import function
from p4_program_host_test import run

ROOT=Path(__file__).resolve().parents[2]
SCRIPTS=ROOT/'pc/skills/petui/scripts'
sys.path.insert(0,str(SCRIPTS))
from device_json import strings_no_nul,strings_valid_text,value_within_depth,DEVICE_JSON_MAX_DEPTH
from bounded_app import initial
from validate_generated_widget import validate_runtime,validate_buttons,validate_manifest,validate_dashboard,DEFAULT_CAPABILITIES,ALLOWED_SPRITES
SUITE=ROOT/'pc/skills/petui/contracts/device-json-strings-v1.json'
UNICODE=ROOT/'pc/skills/petui/contracts/device-json-unicode-v1.json'
DEPTH=ROOT/'pc/skills/petui/contracts/device-json-depth-v1.json'

def test_decoded_depth_agrees_with_source_contract_and_handles_shared_graphs():
    suite=json.loads(DEPTH.read_text());assert DEVICE_JSON_MAX_DEPTH==suite['maximumContainerDepth']
    for c in suite['cases']:
        value=json.loads(c['source']);old=copy.deepcopy(value)
        assert value_within_depth(value)==c['allowed'],c['name'];assert value==old
    shared={'v':0};deep=shared
    for _ in range(31):deep=[deep]
    assert value_within_depth({'first':shared}) and value_within_depth(deep)
    assert not value_within_depth({'first':shared,'deeper':deep})
    cycle=[];cycle.append(cycle);assert not value_within_depth(cycle)
    shared={};graph=shared
    for _ in range(30):graph={'left':graph,'right':graph}
    assert value_within_depth(graph)

def test_python_depth_refusal_precedes_sizing_and_preserves_all_entry_inputs(tmp_path):
    r=json.loads((ROOT/'pc/skills/petui/contracts/app-navigation-v1.json').read_text())['runtime']
    r['dashboard'].update(visualStyle='clean',visualPalette='mint',visualLayout='tool',visualSprite=sorted(ALLOWED_SPRITES)[0])
    nested=0
    for _ in range(32):nested=[nested]
    bad=copy.deepcopy(r);bad['extra']=nested;before=copy.deepcopy(bad);errors=[]
    validate_runtime(bad,'tool',tmp_path,DEFAULT_CAPABILITIES,errors)
    assert any('嵌套层级' in e for e in errors) and bad==before
    with pytest.raises(ValueError,match='depth budget'):initial(bad)
    for fn,value in ((validate_manifest,{'extra':nested}),):
        before=copy.deepcopy(value);errors=[];fn(value,errors)
        assert any('嵌套层级' in e for e in errors) and value==before
    errors=[];validate_dashboard({'extra':nested},'fixture','tool',runtime_dashboard=False,errors=errors)
    assert any('嵌套层级' in e for e in errors)
    errors=[];validate_buttons([{'extra':nested}],set(),DEFAULT_CAPABILITIES,r,errors)
    assert any('嵌套层级' in e for e in errors)

def test_native_bounded_preflight_rejects_before_recursive_parser(tmp_path):
    code=r'''
#include "pet_p4_json_source.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
static char raw[32768],deep[4096];
int main(int argc,char**argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);size_t n=fread(raw,1,sizeof(raw)-1,f);assert(feof(f));fclose(f);raw[n]=0;
 cJSON*suite=cJSON_ParseWithOpts(raw,NULL,true);assert(suite);
 assert(cJSON_GetObjectItemCaseSensitive(suite,"maximumContainerDepth")->valueint==PET_JSON_SOURCE_MAX_DEPTH);
 const cJSON*c;cJSON_ArrayForEach(c,cJSON_GetObjectItemCaseSensitive(suite,"cases")){
  const char*source=cJSON_GetObjectItemCaseSensitive(c,"source")->valuestring;
  bool expected=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"allowed"));
  assert(pet_json_source_bounded_text(source)==expected);
  assert(pet_json_source_bounded_text_bytes(source,strlen(source))==expected);
  if(expected){cJSON*value=cJSON_ParseWithOpts(source,NULL,true);assert(value);cJSON_Delete(value);}
 }
 memset(deep,'[',1000);deep[1000]='0';memset(deep+1001,']',1000);deep[2001]=0;
 assert(!pet_json_source_bounded_text(deep)); /* No recursive parse of this source. */
 assert(!pet_json_source_bounded_text("}")&&!pet_json_source_bounded_text("[0"));
 cJSON_Delete(suite);return 0;
}
'''
    source=tmp_path/'depth.c';source.write_text(code);binary=tmp_path/'depth'
    vendor=ROOT/'firmware/managed_components/espressif__cjson/cJSON'
    run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
         '-fsanitize=address,undefined','-I',ROOT/'firmware/main','-I',vendor,source,vendor/'cJSON.c','-o',binary])
    run([binary,DEPTH])

def test_shared_unicode_text_model_and_raw_utf8_have_no_silent_replacement():
    suite=json.loads(UNICODE.read_text())
    for c in suite['cases']:
        try:value=json.loads(c['source']);allowed=strings_valid_text(value)
        except (json.JSONDecodeError,UnicodeDecodeError):allowed=False
        assert allowed==c['allowed'],c['name']
    for c in suite['byteCases']:
        try:value=json.loads(bytes.fromhex(c['hex']));allowed=strings_valid_text(value)
        except (json.JSONDecodeError,UnicodeDecodeError):allowed=False
        assert allowed==c['allowed'],c['name']

def test_actual_python_entries_refuse_surrogates_before_utf8_size_or_state_publication(tmp_path):
    r=json.loads((ROOT/'pc/skills/petui/contracts/app-navigation-v1.json').read_text())['runtime']
    r['dashboard'].update(visualStyle='clean',visualPalette='mint',visualLayout='tool',visualSprite=sorted(ALLOWED_SPRITES)[0])
    for text in ('\ud800','\udfff','valid\ud800','\ud83d\udc3e'):
        bad=copy.deepcopy(r);bad['vars']['label']={'type':'string','init':text};old=copy.deepcopy(bad);errors=[]
        validate_runtime(bad,'tool',tmp_path,DEFAULT_CAPABILITIES,errors)
        assert any('Unicode' in e for e in errors) and bad==old
        with pytest.raises(ValueError,match='Unicode'):initial(bad)
    # A real paired JSON escape decodes to one scalar, not two Python surrogates.
    valid=copy.deepcopy(r);valid['vars']['label']={'type':'string','init':json.loads('"\\ud83d\\udc3e"')};errors=[]
    validate_runtime(valid,'tool',tmp_path,DEFAULT_CAPABILITIES,errors);assert not errors
    assert initial(valid)['vars']['label']=='🐾'
    errors=[];validate_manifest({'id':'synthetic','name':'\ud800','version':'1','kind':'tool','description':'fixture'},errors)
    assert any('Unicode' in e for e in errors)
    errors=[];validate_buttons([{'event':'button.sw1.short_press','action':'next','label':'\ud800'}],{'next'},DEFAULT_CAPABILITIES,r,errors)
    assert any('Unicode' in e for e in errors)

def test_native_unicode_preflight_actual_cjson_vectors_and_all_scalar_encodings(tmp_path):
    code=r'''
#include "pet_p4_json_source.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
static char raw[32768];
static unsigned hex(unsigned c){return c<='9'?c-'0':c-'a'+10;}
int main(int argc,char**argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);size_t n=fread(raw,1,sizeof(raw)-1,f);assert(feof(f));fclose(f);raw[n]=0;
 cJSON*suite=cJSON_ParseWithOpts(raw,NULL,true);assert(suite);const cJSON*c;
 cJSON_ArrayForEach(c,cJSON_GetObjectItemCaseSensitive(suite,"cases")){
  const char*source=cJSON_GetObjectItemCaseSensitive(c,"source")->valuestring;
  bool expected=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"allowed"));
  assert(pet_json_source_text(source)==expected);
  if(expected){cJSON*value=cJSON_ParseWithOpts(source,NULL,true);assert(value);cJSON_Delete(value);}
 }
 cJSON_ArrayForEach(c,cJSON_GetObjectItemCaseSensitive(suite,"byteCases")){
  const char*s=cJSON_GetObjectItemCaseSensitive(c,"hex")->valuestring;size_t size=strlen(s)/2;unsigned char source[256];assert(size<sizeof(source));
  for(size_t i=0;i<size;i++)source[i]=(unsigned char)((hex((unsigned char)s[i*2])<<4)|hex((unsigned char)s[i*2+1]));
  source[size]=0;
  bool expected=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"allowed"));
  assert(pet_json_source_text_bytes(source,size)==expected);
  if(expected){cJSON*value=cJSON_ParseWithOpts((const char*)source,NULL,true);assert(value);cJSON_Delete(value);}
 }
 /* Exhaustive host-only UTF-8 scalar encoding check, not device glyph/timing. */
 for(uint32_t cp=32;cp<=0x10ffff;cp++){
  if((cp>=0xd800&&cp<=0xdfff)||cp=='"'||cp=='\\')continue;
  unsigned char text[8]={'"'};size_t p=1;
  if(cp<0x80)text[p++]=(uint8_t)cp;
  else if(cp<0x800){text[p++]=(uint8_t)(0xc0|(cp>>6));text[p++]=(uint8_t)(0x80|(cp&63));}
  else if(cp<0x10000){text[p++]=(uint8_t)(0xe0|(cp>>12));text[p++]=(uint8_t)(0x80|((cp>>6)&63));text[p++]=(uint8_t)(0x80|(cp&63));}
  else{text[p++]=(uint8_t)(0xf0|(cp>>18));text[p++]=(uint8_t)(0x80|((cp>>12)&63));text[p++]=(uint8_t)(0x80|((cp>>6)&63));text[p++]=(uint8_t)(0x80|(cp&63));}
  text[p++]='"';assert(pet_json_source_text_bytes(text,p));
 }
 const char*escapes[]={"\"\\\"\"","\"\\\\\"","\"\\u0001\"","\"\\uD83D\\uDC3E\""};
 for(unsigned i=0;i<sizeof(escapes)/sizeof(escapes[0]);i++)assert(pet_json_source_text(escapes[i]));
 assert(!pet_json_source_text_bytes(NULL,0));cJSON_Delete(suite);return 0;
}
'''
    source=tmp_path/'unicode.c';source.write_text(code);binary=tmp_path/'unicode'
    vendor=ROOT/'firmware/managed_components/espressif__cjson/cJSON'
    run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
         '-fsanitize=address,undefined','-I',ROOT/'firmware/main','-I',vendor,source,vendor/'cJSON.c','-o',binary])
    run([binary,UNICODE])

def test_python_source_values_match_shared_c_string_contract_and_preserve_inputs():
    suite=json.loads(SUITE.read_text())
    for c in suite['cases']:
        old=copy.deepcopy(c['value'])
        assert strings_no_nul(c['value'])==c['allowed'],c['name']
        assert c['value']==old
    for c in suite['rawCases']:
        assert strings_no_nul(json.loads(c['json']))==c['allowed']

def test_full_python_entry_points_reject_ambiguous_device_strings_without_repair(tmp_path):
    r=json.loads((ROOT/'pc/skills/petui/contracts/app-navigation-v1.json').read_text())['runtime']
    r['dashboard'].update(visualStyle='clean',visualPalette='mint',visualLayout='tool',visualSprite=sorted(ALLOWED_SPRITES)[0])
    errors=[];validate_runtime(r,'tool',tmp_path,DEFAULT_CAPABILITIES,errors);assert not errors
    for text in ('A\0B','\0','end\0'):
        bad=copy.deepcopy(r);bad['vars']['label']={'type':'string','init':text};old=copy.deepcopy(bad);errors=[]
        validate_runtime(bad,'tool',tmp_path,DEFAULT_CAPABILITIES,errors)
        assert any('NUL' in e for e in errors) and bad==old
        with pytest.raises(ValueError,match='NUL'):initial(bad)
    literal=copy.deepcopy(r);literal['vars']['label']={'type':'string','init':'A\\u0000B'};errors=[]
    validate_runtime(literal,'tool',tmp_path,DEFAULT_CAPABILITIES,errors);assert not errors
    assert initial(literal)['vars']['label']=='A\\u0000B'
    errors=[];validate_buttons([{'event':'button.sw1.short_press','action':'next','label':'A\0B'}],{'next'},DEFAULT_CAPABILITIES,r,errors)
    assert any('NUL' in e for e in errors)
    errors=[];validate_manifest({'id':'synthetic','name':'A\0B','version':'1','kind':'tool','description':'fixture'},errors)
    assert any('NUL' in e for e in errors)
    errors=[];validate_dashboard({'title':'A\0B'},'negative-screen.json.dashboard','tool',runtime_dashboard=False,errors=errors)
    assert any('NUL' in e for e in errors)

def test_native_source_scanner_shared_vectors_escape_parity_and_all_raw_nul_positions(tmp_path):
    suite=json.loads(SUITE.read_text());cases=list(suite['rawCases'])
    for c in suite['cases']:
        for ascii_only in (False,True):
            cases.append({'json':json.dumps(c['value'],ensure_ascii=ascii_only),'allowed':c['allowed']})
    for n in range(13):
        cases.append({'json':'{"v":"'+('\\'*n)+'u0000"}','allowed':n%2==0})
    vectors=tmp_path/'vectors.json';vectors.write_text(json.dumps(cases,ensure_ascii=False))
    code=r'''
#include "pet_p4_json_source.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static char raw[32768],large[65536];
int main(int argc,char**argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);size_t n=fread(raw,1,sizeof(raw)-1,f);assert(feof(f));fclose(f);raw[n]=0;
 cJSON*suite=cJSON_ParseWithOpts(raw,NULL,true);assert(cJSON_IsArray(suite));const cJSON*c;
 cJSON_ArrayForEach(c,suite){
  const char*source=cJSON_GetObjectItemCaseSensitive(c,"json")->valuestring;
  bool expected=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"allowed"));
  assert(pet_json_source_no_nul(source)==expected);
  assert(pet_json_source_no_nul_bytes(source,strlen(source))==expected);
 }
 const char*valid="{\"v\":\"literal \\\\u0000 and \\n text\"}";size_t length=strlen(valid);
 char damaged[128];for(size_t i=0;i<length;i++){memcpy(damaged,valid,length);damaged[i]=0;assert(!pet_json_source_no_nul_bytes(damaged,length));}
 const char edge[]={'\\','u','0','0','0','0'};
 for(size_t i=0;i<sizeof(edge);i++)assert(pet_json_source_no_nul_bytes(edge,i));
 assert(!pet_json_source_no_nul_bytes(edge,sizeof(edge)));
 assert(!pet_json_source_no_nul(NULL)&&!pet_json_source_no_nul_bytes(NULL,0));
 memset(large,'a',sizeof(large));assert(pet_json_source_no_nul_bytes(large,sizeof(large)));
 large[65535]=0;assert(!pet_json_source_no_nul_bytes(large,sizeof(large)));
 cJSON_Delete(suite);return 0;
}
'''
    source=tmp_path/'source.c';source.write_text(code);binary=tmp_path/'source'
    vendor=ROOT/'firmware/managed_components/espressif__cjson/cJSON'
    run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
         '-fsanitize=address,undefined','-I',ROOT/'firmware/main','-I',vendor,source,vendor/'cJSON.c','-o',binary])
    run([binary,vectors])

def test_actual_protocol_entry_refuses_before_transport_registration_or_topic_dispatch(tmp_path):
    protocol=(ROOT/'firmware/main/pet_p4_protocol.c').read_text()
    native=(ROOT/'firmware/main/pet_p4_usb_native.c').read_text()
    body=function(protocol,'pet_p4_handle_line_with_reply').split('  topic = json_string(root, "topic");')[0]
    code=r'''
#include "pet_p4_json_source.h"
#include "pet_p4_query_reply.h"
#include "cJSON.h"
#include <assert.h>
typedef void(*pet_p4_send_line_fn)(const char*,void*);
typedef struct{char board_device_id[48];} pet_p4_runtime_state_t;
static unsigned transports,acks;
static void pet_app_host_transport(pet_p4_send_line_fn f,void*c,const char*id){(void)f;(void)c;(void)id;transports++;}
static void pet_p4_media_transport(pet_p4_send_line_fn f,void*c){(void)f;(void)c;transports++;}
static void send_protocol_ack(pet_p4_send_line_fn f,void*c,const char*t,const cJSON*p,bool ok,const char*code,const char*message){
 (void)f;(void)c;(void)p;(void)message;assert(!ok&&!t[0]&&!strcmp(code,"invalid_json"));acks++;
}
'''+r'''
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
static _Atomic uint32_t g_connection_generation=7;
static void pet_p4_native_usb_send_json_line(const char*line,void*ctx){(void)line;(void)ctx;abort();}
'''+'\n'+function(native,'native_route_context')+'\n'+function(native,'pet_p4_native_usb_route_current')+'\n'+function(protocol,'asset_phase_send_current')+'\n'+function(protocol,'pet_p4_protocol_source_valid')+'\n'+body+r'''
  (void)payload;(void)topic;cJSON_Delete(root);return true;
}
'''+function(protocol,'pet_p4_handle_line')+r'''
int main(void){
 pet_p4_runtime_state_t state={0};
 assert(!pet_p4_handle_line(&state,"{\"topic\":\"widget/delete\\u0000ignored\",\"payload\":{}}",NULL,NULL));
 assert(!transports&&acks==1);
 const char invalid[]={ '{','"','t','o','p','i','c','"',':','"',0xc0,0x80,'"','}',0 };
 assert(!pet_p4_handle_line(&state,invalid,NULL,NULL)&&!transports&&acks==2);
 assert(!pet_p4_handle_line(&state,"{\"topic\":\"\\ud800\",\"payload\":{}}",NULL,NULL)&&!transports&&acks==3);
 char deep[80];memset(deep,'[',33);deep[33]='0';memset(deep+34,']',33);deep[67]=0;
 assert(!pet_p4_handle_line(&state,deep,NULL,NULL)&&!transports&&acks==4);
 assert(pet_p4_handle_line(&state,"{\"topic\":\"test\",\"payload\":{\"label\":\"A\\\\u0000B\"}}",NULL,NULL)&&transports==2);
 pet_p4_query_reply_t reply={0};
 assert(!pet_p4_handle_line_with_reply(&state,invalid,NULL,NULL,&reply)&&transports==2&&acks==5&&!reply.line);
 reply.line=(char*)"still-owned";
 assert(!pet_p4_handle_line_with_reply(&state,"{}",NULL,NULL,&reply)&&transports==2&&acks==5&&!strcmp(reply.line,"still-owned"));
 /* Actual new route preflight: retired ingress is consumed before transport,
  * ACK or parsing; current ingress still follows the strict source checks. */
 assert(pet_p4_handle_line(&state,"malformed",pet_p4_native_usb_send_json_line,(void*)(uintptr_t)7));
 assert(transports==2&&acks==5);
 assert(pet_p4_handle_line(&state,"{\"topic\":\"test\",\"payload\":{}}",pet_p4_native_usb_send_json_line,(void*)(uintptr_t)8));
 assert(transports==4&&acks==5);
 assert(!pet_p4_handle_line(&state,invalid,pet_p4_native_usb_send_json_line,(void*)(uintptr_t)8));
 assert(transports==4&&acks==6);

 return 0;
}
'''
    source=tmp_path/'entry.c';source.write_text(code);binary=tmp_path/'entry'
    vendor=ROOT/'firmware/managed_components/espressif__cjson/cJSON'
    run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
         '-fsanitize=address,undefined','-I',ROOT/'firmware/main','-I',vendor,source,vendor/'cJSON.c','-o',binary])
    run([binary])

def test_runtime_guard_precedes_parse_allocation_and_keeps_legacy_text_readers_separate():
    source=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text();runtime=function(source,'parse_runtime')
    assert runtime.index('pet_json_source_bounded_text(widget_json)')<runtime.index('root = cJSON_ParseWithOpts(widget_json,NULL,true);')<runtime.index('calloc(')
    assert 'pet_json_source_bounded_text(buttons_json?buttons_json:"[]")' in runtime
    assert 'read_json_file(widget_path, widget_json' in source and 'read_json_file(buttons_path, buttons_json' in source
    assert 'read_file(MINIAPP_ID_PATH' in source and 'read_file(MINIAPP_BUILTIN_MARKER_PATH' in source
    assert 'pet_json_source' not in function(source,'read_binary_file')

def test_p4_string_guard_has_no_io_or_allocation_and_bounded_own_frame(tmp_path):
    from p4_program_test import p4_compiler,WARNINGS,FREESTANDING,INCLUDES
    compiler=p4_compiler()
    if not compiler:pytest.skip('requires existing P4 compiler, never invokes PlatformIO')
    source=tmp_path/'guard.c'
    source.write_text('#include "pet_p4_json_source.h"\n'
                      'bool inspect(const void*p,size_t n){return pet_json_source_no_nul_bytes(p,n);}\n'
                      'bool inspect_text(const void*p,size_t n){return pet_json_source_text_bytes(p,n);}\n'
                      'bool inspect_text_bounded(const void*p,size_t n){return pet_json_source_bounded_text_bytes(p,n);}\n')
    obj=tmp_path/'guard.o'
    run([compiler,*WARNINGS,'-Os','-march=rv32imafc_zicsr_zifencei_xesppie','-mabi=ilp32f',
         *FREESTANDING,'-fstack-usage',*INCLUDES,'-c',source,'-o',obj])
    assert not run([str(compiler).removesuffix('gcc')+'nm','-u',obj]).strip()
    frames=obj.with_suffix('.su').read_text().splitlines();assert frames
    for row in frames:
        name,amount,kind=row.split('\t');assert kind=='static' and int(amount)<=(128 if 'text' in name or 'hex4' in name else 64)
