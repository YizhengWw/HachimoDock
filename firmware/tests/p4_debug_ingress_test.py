"""Actual worker pre-dispatch expression rejects unsafe source before parsers.

The native source scanner is real; dispatch callbacks are counters. This tests
source-boundary placement, not JSON grammar, the RTOS task or acoustic timing.
"""
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def test_real_debug_dispatch_preflight_before_every_early_parser(tmp_path):
    main=(ROOT/'main/pet_p4_main.c').read_text()
    helper='static bool protocol_debug_source_bounded('+main.split('static bool protocol_debug_source_bounded(',1)[1].split('/* Input never waits',1)[0]
    protocol=(ROOT/'main/pet_p4_protocol.c').read_text()
    shared='bool pet_p4_protocol_source_valid('+protocol.split('bool pet_p4_protocol_source_valid(',1)[1].split('bool pet_p4_handle_line_with_reply(',1)[0]
    expression='bool bounded_debug_source='+main.split('bool bounded_debug_source=',1)[1].split('    if (!handled_debug) {',1)[0]
    code=r'''
#include "pet_p4_json_source.h"
#include <assert.h>
#include <stdio.h>
typedef void (*send_fn)(const char *,void *);
static unsigned calls;
static void transport_send_line(const char *s,void *ctx){(void)s;(void)ctx;}
static bool maybe_handle_transport_audit(const char *s,send_fn send,void *ctx){(void)s;(void)send;(void)ctx;calls++;return false;}
static bool pet_p4_sched_trace_rpc_handle(const char *s,send_fn send,void *ctx){(void)s;(void)send;(void)ctx;calls++;return false;}
static bool pet_storage_io_rpc_handle(const char *s,send_fn send,void *ctx){(void)s;(void)send;(void)ctx;calls++;return false;}
static bool maybe_handle_program_probe_line(const char *s,send_fn send,void *ctx){(void)s;(void)send;(void)ctx;calls++;return false;}
static bool maybe_handle_program_session_probe_line(const char *s,send_fn send,void *ctx){(void)s;(void)send;(void)ctx;calls++;return false;}
static bool maybe_handle_program_main_probe_line(const char *s,send_fn send,void *ctx){(void)s;(void)send;(void)ctx;calls++;return false;}
static bool maybe_handle_debug_line(const char *s,void *ctx){(void)s;(void)ctx;calls++;return false;}
static void check(const char *line,unsigned expected){
  struct {const char *line;send_fn send_line;void *reply_ctx;} request={line,transport_send_line,NULL};
  calls=0;
'''+expression+r'''
  assert(!handled_debug&&calls==expected);
}
int main(void){
  check(NULL,0);
  check("{\"topic\":\"audio/status\",\"payload\":{}}",0);
  check("{\"topic\":\"debug/program_probe\",\"payload\":{}}",7);
  check("{\"topic\":\"debug/storage_io_begin\",\"payload\":{}}",7);
  check("{\"topic\":\"debug/screenshot\",\"payload\":{\"text\":\"\xef\xbf\xbd\"}}",7);
  check("{\"topic\":\"debug/screenshot\",\"payload\":{\"text\":\"\xff\"}}",0);
  check("{\"topic\":\"debug/screenshot\",\"payload\":{\"text\":\"\\ud800\"}}",0);
  check("{\"topic\":\"debug/screenshot\",\"payload\":{\"text\":\"\\u0000\"}}",0);
  check("{\"topic\":\"debug/screenshot\",\"payload\":{\"text\":\"\\\\u0000\"}}",7);
  char text[4096];const char *prefix="{\"topic\":\"debug/transport_audit_read\",\"payload\":";
  for(unsigned depth=31;depth<=1000;depth++){
    size_t n=strlen(prefix);memcpy(text,prefix,n);
    memset(text+n,'[',depth);n+=depth;text[n++]='0';
    memset(text+n,']',depth);n+=depth;text[n++]='}';text[n]=0;
    check(text,depth==31?7:0);
  }
  return 0;
}
'''
    code=code.replace('static void check(',shared+'\n'+helper+'\nstatic void check(',1)
    source=tmp_path/'debug-source.c';source.write_text(code);exe=tmp_path/'debug-source'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pedantic',
        '-fsanitize=address,undefined','-I',str(ROOT/'main'),str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)

def test_screenshot_parser_consumes_the_entire_envelope():
    source=(ROOT/'main/pet_p4_main.c').read_text().split('static bool maybe_handle_debug_line(',1)[1].split('/* Only protocol_worker_task owns these scratch buffers.',1)[0]
    assert 'cJSON_ParseWithOpts(line,NULL,true)' in source
    assert 'cJSON_Parse(line)' not in source
