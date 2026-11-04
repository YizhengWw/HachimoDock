"""R136 large reply deferral: widget/inventory and hello use the held reply slot; everything else is unchanged."""
from pathlib import Path
import configparser
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_query_reply.h"
typedef uintptr_t TaskHandle_t;
static TaskHandle_t xTaskGetCurrentTaskHandle(void){return 1;}
typedef void (*pet_p4_send_line_fn)(const char *line,void *ctx);
static char sent[8][256];static unsigned count;
static void capture(const char *line,void *ctx){(void)ctx;assert(count<8);snprintf(sent[count++],256,"%s",line);}
'''
BODY=r'''
static cJSON *payload(const char *k){cJSON *p=cJSON_CreateObject();cJSON_AddStringToObject(p,"k",k);return p;}
int main(void){
 pet_p4_query_reply_t reply={0};g_deferred_reply_owner=1;
 /* No reply slot (unsolicited hello from the main task): sent immediately. */
 g_deferred_reply=NULL;send_topic_payload(capture,NULL,"hello",payload("a"));assert(count==1&&strstr(sent[0],"\"k\":\"a\""));
 count=0;g_deferred_reply=&reply;
 send_topic_payload(capture,NULL,"widget/inventory",payload("b"));
 send_topic_payload(capture,NULL,"hello",payload("c"));
 if(PET_P4_LARGE_REPLY_DEFERRED){
  /* One slot per request: the inventory is held, the hello that follows flushes it and goes out
   * normally (order b, c), exactly like a second install ACK. A lone inventory stays held. */
  assert(count==2&&strstr(sent[0],"\"k\":\"b\"")&&strstr(sent[1],"\"k\":\"c\"")&&reply.line==NULL);
  count=0;send_topic_payload(capture,NULL,"widget/inventory",payload("d"));assert(count==0&&reply.line&&strstr(reply.line,"\"k\":\"d\""));
  send_topic_payload(capture,NULL,"protocol/ack",payload("d2"));assert(count==2&&strstr(sent[0],"\"k\":\"d\"")&&strstr(sent[1],"\"k\":\"d2\"")&&reply.line==NULL);
 }else{
  assert(count==2&&strstr(sent[0],"\"k\":\"b\"")&&strstr(sent[1],"\"k\":\"c\"")&&reply.line==NULL);
 }
 /* Catalog metadata has the same single post-unlock slot, even at 10KB.
  * Small reconcile results use it too; no second send under the state lock. */
 const char *catalog_topics[]={"widget/catalog_snapshot","widget/catalog_reconcile_result"};
 for(unsigned i=0;i<2;i++){
  count=0;cJSON *large=cJSON_CreateObject();char bytes[10001];memset(bytes,'x',10000);bytes[10000]=0;
  cJSON_AddStringToObject(large,"items",bytes);send_topic_payload(capture,NULL,catalog_topics[i],large);
  if(PET_P4_LARGE_REPLY_DEFERRED){
   assert(count==0&&reply.line&&strlen(reply.line)>10000);char *held=reply.line;reply.line=NULL;
   capture(held,NULL);cJSON_free(held);assert(count==1);
  }else assert(count==1&&reply.line==NULL);
 }
 /* The install ACK deferral itself is unchanged. */
 count=0;send_topic_payload(capture,NULL,"widget-install-ack",payload("e"));assert(count==0&&reply.line&&strstr(reply.line,"\"k\":\"e\""));
 send_topic_payload(capture,NULL,"widget/program-status",payload("f"));assert(count==2&&strstr(sent[0],"\"k\":\"e\"")&&reply.line==NULL);
 g_deferred_reply=NULL;return 0;
}
'''


def _build_and_run(tmp_path,flags,name):
    text=(ROOT/'main/pet_p4_protocol.c').read_text()
    helpers=text[text.index('#if PET_P4_MUTATION_ACK_DEFERRED\nstatic pet_p4_query_reply_t *g_deferred_reply;'):text.index('/* Best-effort host wall clock')]
    src=tmp_path/(name+'.c');exe=src.with_suffix('');src.write_text(HARNESS+helpers+function(text,'send_topic_payload')+BODY);vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-fsanitize=address,undefined',*flags,
        '-I',str(ROOT/'main'),'-I',str(vendor),str(src),str(vendor/'cJSON.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=60)


def test_inventory_and_hello_are_held_only_with_the_flag(tmp_path):
    _build_and_run(tmp_path,['-DPET_P4_MUTATION_ACK_DEFERRED=1','-DPET_P4_LARGE_REPLY_DEFERRED=1'],'large')
    _build_and_run(tmp_path,['-DPET_P4_MUTATION_ACK_DEFERRED=1'],'ack_only')


def test_flag_gating_and_single_reply_sites(tmp_path):
    text=(ROOT/'main/pet_p4_protocol.c').read_text();main=(ROOT/'main/pet_p4_main.c').read_text()
    assert text.count('#if PET_P4_LARGE_REPLY_DEFERRED\n  if(!strcmp(topic,"widget/inventory")||!strcmp(topic,"hello")\n      ||!strcmp(topic,"widget/catalog_snapshot")||!strcmp(topic,"widget/catalog_reconcile_result"))return true;\n#endif')==1
    # Exactly one inventory sender and one hello sender exist; the request handlers emit nothing after them.
    assert text.count('send_topic_payload(send_line, ctx, "widget/inventory", payload);')==1 and text.count('send_topic_payload(send_line, ctx, "hello", payload);')==1
    bind=text[text.index('if (strcmp(topic, "ack") == 0 || strcmp(topic, "bind") == 0) {'):text.index('} else if (strcmp(topic,"debug/identity")==0){')]
    assert bind.count('pet_p4_send_hello(state, send_line, ctx);')==1 and 'send_protocol_ack_if_requested' not in bind
    # Unsolicited hello lines are sent by the main task without a reply slot (unchanged path).
    assert main.count('pet_p4_send_hello_for_board(g_hello_board_id,transport_send_line,')>=1 and 'pet_p4_protocol_set_deferred_reply' not in main[:main.index('static void protocol_worker_task')]
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_large_reply_parent':
            assert config[section]['extends']=='env:esp32_p4_gap_holder_parent' and config[section]['custom_p4_project_version']=='0.7.103-p4-r136-largereply'
            assert 'PET_P4_LARGE_REPLY_DEFERRED=1' in flags and not any(v in flags for v in ('TASK_PRIORITY','F_CPU','BUDGET_US','WATCHDOG','AFFINITY'))
        else:assert 'PET_P4_LARGE_REPLY_DEFERRED=1' not in flags
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_query_reply.h"\nint main(void){return PET_P4_LARGE_REPLY_DEFERRED;}\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_LARGE_REPLY_DEFERRED=1','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'Large reply deferral requires' in result.stderr
    assert subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsyntax-only','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True).returncode==0
