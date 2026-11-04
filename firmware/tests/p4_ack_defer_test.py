"""R134 deferred install ACK: one held line per request, flushed in order, nothing held without a reply slot."""
from pathlib import Path
import configparser
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]


def test_send_topic_payload_defers_only_the_install_ack_and_keeps_wire_order(tmp_path):
    text=(ROOT/'main/pet_p4_protocol.c').read_text()
    helpers=text[text.index('#if PET_P4_MUTATION_ACK_DEFERRED\nstatic pet_p4_query_reply_t *g_deferred_reply;'):text.index('/* Best-effort host wall clock')]
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_query_reply.h"
typedef uintptr_t TaskHandle_t;
static TaskHandle_t current_task=1;
static TaskHandle_t xTaskGetCurrentTaskHandle(void){return current_task;}
typedef void (*pet_p4_send_line_fn)(const char *line,void *ctx);
static char sent[8][256];static unsigned count;
static void capture(const char *line,void *ctx){(void)ctx;assert(count<8);snprintf(sent[count++],256,"%s",line);}
'''+helpers+function(text,'send_topic_payload')+r'''
static cJSON *payload(const char *k){cJSON *p=cJSON_CreateObject();cJSON_AddStringToObject(p,"k",k);return p;}
int main(void){
 pet_p4_query_reply_t reply={0};
 /* No reply slot: every line goes out immediately. */
 g_deferred_reply=NULL;send_topic_payload(capture,NULL,"widget-install-ack",payload("a"));assert(count==1&&strstr(sent[0],"\"k\":\"a\""));
 /* Reply slot: the install ACK is held, nothing sent. */
 count=0;g_deferred_reply=&reply;g_deferred_reply_owner=xTaskGetCurrentTaskHandle();send_topic_payload(capture,NULL,"widget-install-ack",payload("b"));
 assert(count==0&&reply.line&&strstr(reply.line,"widget-install-ack")&&strstr(reply.line,"\"k\":\"b\""));
 /* A later non-deferrable line flushes the held ACK first: order preserved. */
 send_topic_payload(capture,NULL,"widget/inventory",payload("c"));
 assert(count==2&&strstr(sent[0],"\"k\":\"b\"")&&strstr(sent[1],"\"k\":\"c\"")&&reply.line==NULL);
 /* A second deferrable line while one is held: flush, then send normally. */
 send_topic_payload(capture,NULL,"widget-install-ack",payload("d"));assert(count==2&&reply.line&&strstr(reply.line,"\"k\":\"d\""));
 send_topic_payload(capture,NULL,"widget-install-ack",payload("e"));assert(count==4&&strstr(sent[2],"\"k\":\"d\"")&&strstr(sent[3],"\"k\":\"e\"")&&reply.line==NULL);
 /* protocol/ack and status topics are never deferred. */
 send_topic_payload(capture,NULL,"protocol/ack",payload("f"));assert(count==5&&reply.line==NULL);
 send_topic_payload(capture,NULL,"widget/program-status",payload("g"));assert(count==6&&reply.line==NULL);
 /* A NULL send_line still never leaks the held line. */
 send_topic_payload(NULL,NULL,"widget-install-ack",payload("h"));assert(count==6&&reply.line==NULL);
 /* A different task cannot adopt or flush this worker's deferred reply. */
 count=0;send_topic_payload(capture,NULL,"widget-install-ack",payload("owner"));assert(!count&&reply.line);
 current_task=2;send_topic_payload(capture,NULL,"widget-install-ack",payload("other"));
 assert(count==1&&strstr(sent[0],"other")&&reply.line&&strstr(reply.line,"owner"));
 current_task=1;send_topic_payload(capture,NULL,"protocol/ack",payload("flush"));
 assert(count==3&&strstr(sent[1],"owner")&&strstr(sent[2],"flush")&&!reply.line);
 g_deferred_reply=NULL;return 0;
}
'''
    src=tmp_path/'ack.c';exe=src.with_suffix('');src.write_text(code);vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-fsanitize=address,undefined','-DPET_P4_MUTATION_ACK_DEFERRED=1',
        '-I',str(ROOT/'main'),'-I',str(vendor),str(src),str(vendor/'cJSON.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=60)


def test_dispatcher_wrapper_and_worker_route_are_flag_gated(tmp_path):
    text=(ROOT/'main/pet_p4_protocol.c').read_text();main=(ROOT/'main/pet_p4_main.c').read_text()
    assert text.count('bool pet_p4_handle_line_with_reply(\n  pet_p4_runtime_state_t *state,')==1 and 'handle_line_with_reply_dispatch' not in text
    setter=text[text.index('void pet_p4_protocol_set_deferred_reply(pet_p4_query_reply_t *reply){'):]
    assert 'g_deferred_reply=reply;' in setter
    worker=function(main,'protocol_worker_task')
    assert worker.index('pet_p4_protocol_set_deferred_reply(&query_reply);')<worker.index('pet_p4_handle_line_with_reply(&g_state, request.line')<worker.index('pet_p4_protocol_set_deferred_reply(NULL);')<worker.index('xSemaphoreGive(g_state_mutex);')
    assert 'return pet_p4_handle_line_with_reply(state, line, send_line, ctx, NULL);' in text
    assert '#if PET_P4_QUERY_REPLY_DEFERRED || PET_P4_QUERY_REPLY_TRACE || PET_P4_MUTATION_ACK_DEFERRED' in main
    # A retired ingress is freed before dispatch and must never reach a reply.
    # Compare lock/ACK/free order only along the ordinary dispatch path.
    retired=worker.index('if (request.send_line == pet_p4_native_usb_send_json_line')
    retired_end=worker.index('#if PET_P4_QUERY_REPLY_TRACE',retired)
    retired_guard=worker[retired:retired_end]
    assert '!pet_p4_native_usb_route_current(request.reply_ctx)' in retired_guard
    assert 'free(request.line); continue;' in retired_guard
    assert retired_end<worker.index('bool handled_debug')<worker.index('if (!handled_debug) {')
    dispatch=worker[worker.index('if (!handled_debug) {'):]
    assert dispatch.index('xSemaphoreGive(g_state_mutex);')<dispatch.index('if (query_reply.line) {')<dispatch.index('free(request.line);')
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_ack_defer_parent':
            assert config[section]['extends']=='env:esp32_p4_gap_context_parent' and config[section]['custom_p4_project_version']=='0.7.103-p4-r134-ackdefer'
            assert 'PET_P4_MUTATION_ACK_DEFERRED=1' in flags and not any(v in flags for v in ('TASK_PRIORITY','F_CPU','BUDGET_US','WATCHDOG','AFFINITY'))
        else:assert 'PET_P4_MUTATION_ACK_DEFERRED=1' not in flags
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_query_reply.h"\nint main(void){return PET_P4_MUTATION_ACK_DEFERRED;}\n')
    assert subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-fsyntax-only',str(src)],capture_output=True,text=True).returncode==0
