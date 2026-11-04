"""Production RX framing rejects corrupted prefixes/suffixes without a device."""
from pathlib import Path
import subprocess

from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def run_c(tmp_path, code):
    source, binary = tmp_path / 'rx.c', tmp_path / 'rx'
    source.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                    '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'tests/vendor/cjson'), str(source),
                    '-I', str(ROOT / 'main'),
                    str(ROOT / 'tests/vendor/cjson/cJSON.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_actual_jsonl_drops_whole_overflow_or_nul_frame_and_recovers(tmp_path):
    main = (ROOT / 'main/pet_p4_main.c').read_text()
    run_c(tmp_path, r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
static unsigned queued;
static char received[64];

static void transport_send_line(const char *line,void *ctx){(void)line;(void)ctx;}
static bool enqueue_protocol_line(const char *line,void(*send)(const char*,void*),void *reply,void *ctx){
 assert(send==transport_send_line&&!ctx&&reply==(void*)1);queued++;
 assert(strlen(line)<sizeof(received));strcpy(received,line);return true;
}

''' + function(main, 'consume_byte') + r'''
static char a[16],b[16];static size_t an,bn;
static bool feed(char *out,size_t *used,const char *bytes,size_t len){
 bool result=false;for(size_t i=0;i<len;i++)result=consume_byte(bytes[i],out,used,16,(void*)1);return result;
}
int main(void){
 assert(!consume_byte('x',NULL,&an,16,(void*)1));
 assert(!consume_byte('x',a,NULL,16,(void*)1));
 assert(!consume_byte('x',a,&an,1,(void*)1));
 /* Capacity includes the trailing NUL; the last valid byte remains usable. */
 assert(feed(a,&an,"123456789012345\r\n",17));
 assert(queued==1&&!strcmp(received,"123456789012345")&&!an);
 /* Old code reset length to zero and executed this malicious/garbled tail. */
 feed(a,&an,"1234567890123456{}\n",19);assert(queued==1&&!an);
 feed(a,&an,"1234567890123456",16);assert(an==16);
 /* A separate transport keeps its own state; a truncated frame cannot
  * swallow the other transport or splice its next valid frame. */
 feed(b,&bn,"{}\n",3);assert(queued==2&&!bn&&!strcmp(received,"{}"));
 assert(!feed(a,&an,"\r\0{}\n",5));assert(queued==2&&!an);
 assert(feed(a,&an,"{}\n",3)&&queued==3&&!strcmp(received,"{}"));
 const char embedded[]={ '{','}',0,'{','}','\n' };
 feed(a,&an,embedded,sizeof(embedded));assert(queued==3&&!an);
 feed(a,&an,"\n\r\n",3);assert(queued==3);
 /* Fragmentation at every byte and repeated rejected lines stay bounded. */
 for(unsigned n=0;n<64;n++){
  for(unsigned i=0;i<48;i++)consume_byte('x',a,&an,sizeof(a),(void*)1);
  assert(!consume_byte('\n',a,&an,sizeof(a),(void*)1)&&!an);
  assert(feed(a,&an,"{}\n",3));
 }
 assert(queued==67);
 /* Never delete an embedded CR to turn an invalid topic into a command. */
 assert(!feed(a,&an,"{\"x\r\":1}\n",9));assert(queued==67&&!an);
 assert(!feed(a,&an,"{}\r\r\n",5));assert(queued==67&&!an);
 assert(feed(a,&an,"{}\r\n",4));assert(queued==68&&!strcmp(received,"{}"));
 return 0;
}
''')


def test_native_json_uses_full_payload_and_never_accepts_nul_prefix(tmp_path):
    native = (ROOT / 'main/pet_p4_usb_native.c').read_text()
    protocol = (ROOT / 'main/pet_p4_protocol.c').read_text()
    # Lock the actual protocol dispatch to the same strict parser exercised below.
    wrapper = function(protocol, 'pet_p4_handle_line')
    assert 'return pet_p4_handle_line_with_reply(state, line, send_line, ctx, NULL);' in wrapper
    dispatch = function(protocol, 'pet_p4_handle_line_with_reply')
    assert 'root = cJSON_ParseWithOpts(line, NULL, true);' in dispatch
    run_c(tmp_path, r'''
#include "cJSON.h"
#include "pet_p4_json_source.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
''' + function(native, 'parse_payload_json') + r'''
static unsigned abort_checks,queued,executed;
static uint32_t g_native_dispatch_connection;
static void *g_state=(void*)1,*g_enqueue_protocol_ctx=(void*)2;
static void pet_p4_native_usb_send_json_line(const char *line,void *ctx){(void)line;(void)ctx;}
static bool strict_dispatch(const char *line){
 cJSON *root=pet_json_source_no_nul(line)?cJSON_ParseWithOpts(line,NULL,true):NULL;bool ok=cJSON_IsObject(root);
 cJSON_Delete(root);if(ok)executed++;return ok;
}
static bool handle_asset_abort_json(const uint8_t *payload,uint32_t len){
 abort_checks++;cJSON *root=parse_payload_json(payload,len);cJSON_Delete(root);return false;
}
static bool enqueue(const char *line,void(*send)(const char*,void*),void *reply,void *ctx){
 assert(send==pet_p4_native_usb_send_json_line&&
        reply==(void*)(uintptr_t)(g_native_dispatch_connection+1U)&&ctx==(void*)2);
 queued++;return strict_dispatch(line);
}
static bool(*g_enqueue_protocol)(const char*,void(*)(const char*,void*),void*,void*)=enqueue;
static void pet_p4_handle_line(void *state,const char *line,void(*send)(const char*,void*),void *ctx){
 assert(state==(void*)1&&send==pet_p4_native_usb_send_json_line&&
        ctx==(void*)(uintptr_t)(g_native_dispatch_connection+1U));strict_dispatch(line);
}
''' + function(native, 'handle_json_frame') + r'''
int main(void){
 const char *valid=" {\"topic\":\"test\",\"payload\":{}} \t\n";
 cJSON *root=parse_payload_json((const uint8_t*)valid,(uint32_t)strlen(valid));assert(cJSON_IsObject(root));cJSON_Delete(root);
 const char *tails[]={"{}garbage","{}{}","{}\x7f","{", ""};
 for(unsigned i=0;i<sizeof(tails)/sizeof(*tails);i++)assert(!parse_payload_json((const uint8_t*)tails[i],(uint32_t)strlen(tails[i])));
 assert(!parse_payload_json(NULL,0));
 const char *encoded="{\"topic\":\"test\\u0000ignored\",\"payload\":{}}";
 assert(!parse_payload_json((const uint8_t*)encoded,(uint32_t)strlen(encoded)));
 const uint8_t nul[]={ '{','}',0,'g','a','r','b','a','g','e' };
 assert(!parse_payload_json(nul,sizeof(nul)));
 handle_json_frame(nul,sizeof(nul));handle_json_frame(NULL,0);handle_json_frame((const uint8_t*)"",0);
 assert(!abort_checks&&!queued&&!executed);
 handle_json_frame((const uint8_t*)encoded,(uint32_t)strlen(encoded));assert(!abort_checks&&!queued&&!executed);
 handle_json_frame((const uint8_t*)valid,(uint32_t)strlen(valid));assert(abort_checks==1&&queued==1&&executed==1);
 /* Preserve the actual reply epoch on a nonzero connection as well. */
 g_native_dispatch_connection=41;
 handle_json_frame((const uint8_t*)tails[0],(uint32_t)strlen(tails[0]));assert(queued==2&&executed==1);
 g_enqueue_protocol=NULL;handle_json_frame(nul,sizeof(nul));assert(abort_checks==2&&executed==1);
 handle_json_frame((const uint8_t*)valid,(uint32_t)strlen(valid));assert(executed==2);
 /* Fallback must retain a route even for connection zero, just like enqueue. */
 g_native_dispatch_connection=0;handle_json_frame((const uint8_t*)valid,(uint32_t)strlen(valid));assert(executed==3);
 return 0;
}
''')


def test_binary_asset_payload_does_not_use_text_rejection():
    native = (ROOT / 'main/pet_p4_usb_native.c').read_text()
    data = function(native, 'handle_file_data')
    assert 'parse_payload_json' not in data and 'memchr' not in data
