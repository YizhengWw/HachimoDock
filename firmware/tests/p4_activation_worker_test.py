"""Execute production cold-navigation dispatch; no device, audio or user data."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def execute(tmp_path, source):
    path, binary = tmp_path / 'dispatch.c', tmp_path / 'dispatch'
    path.write_text(source)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_actual_worker_queue_navigation_commit_and_late_cancel(tmp_path):
    main = (ROOT / 'main/pet_p4_main.c').read_text()
    body = main.split('static bool enqueue_component_activation(', 1)[1]
    body = 'static bool enqueue_component_activation(' + body.split('/* Fixed trusted vectors', 1)[0]
    execute(tmp_path, r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum {PET_MINIAPP_ACTIVATION_FAILED, PET_MINIAPP_ACTIVATION_OPENED,
 PET_MINIAPP_ACTIVATION_OPENED_UNSAVED, PET_MINIAPP_ACTIVATION_CANCELLED};
typedef int pet_p4_miniapp_activation_result_t;
typedef struct {char *line;void(*send_line)(void);void *reply_ctx;uint64_t activation_token,cursor_token;} pet_p4_protocol_request_t;
static struct {char screen_page[16];bool asset_transfer_active;int stats;unsigned last_update_ms;} g_state;
static int g_state_mutex=1,g_protocol_queue=2,holder,enters,leaves,runs,warnings;
static bool queue_full,enter_failed,cancelled,conversation,ota,navigate_away,away_and_back;
static int outcome,cursor_runs;
static pet_p4_protocol_request_t queued;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
#define configASSERT(v) assert(v)
#define TAG "test"
#define ESP_LOGW(tag,message) do {(void)(tag);(void)(message);warnings++;}while(0)
static int xQueueSend(int queue,const void *item,unsigned wait){
 assert(queue==2&&wait==0);queued=*(const pet_p4_protocol_request_t*)item;
 assert(!queued.line&&!queued.send_line&&!queued.reply_ctx);return !queue_full;
}
static void xSemaphoreTake(int mutex,unsigned wait){assert(mutex==1&&wait==portMAX_DELAY&&!holder);holder=1;}
static void xSemaphoreGive(int mutex){assert(mutex==1&&holder);holder=0;}
static bool pet_p4_conversation_active(const void *state){assert(state==&g_state);return conversation;}
static bool pet_p4_ota_transfer_active(void){return ota;}
static void pet_p4_miniapp_activation_guard(bool visible){assert(holder);if(!visible)cancelled=true;}
static bool pet_p4_miniapp_storage_lane_enter(void){assert(holder);enters++;return !enter_failed;}
static bool pet_p4_miniapp_storage_lane_leave(void){assert(holder);leaves++;return true;}
static bool pet_p4_miniapp_catalog_cursor_run(uint64_t token){assert(holder&&token==9);cursor_runs++;return false;}
static bool pet_p4_miniapp_activation_reject_not_started(uint64_t token){assert(holder&&token==9);cancelled=true;return true;}
static bool pet_p4_miniapp_activation_navigation_current(uint64_t token){assert(holder&&token==9);return !cancelled;}
static void pet_p4_miniapp_sync_stats(int *stats){assert(holder);*stats=7;}
static int pet_p4_miniapp_activation_run(uint64_t token){
 assert(holder&&token==9);runs++;
 if(cancelled)return PET_MINIAPP_ACTIVATION_CANCELLED;
 if(navigate_away)strcpy(g_state.screen_page,"main");
 if(away_and_back){strcpy(g_state.screen_page,"main");pet_p4_miniapp_activation_guard(false);strcpy(g_state.screen_page,"components");}
 return outcome;
}
''' + body + r'''
static void reset(void){
 memset(&g_state,0,sizeof(g_state));strcpy(g_state.screen_page,"components");
 cancelled=enter_failed=conversation=ota=navigate_away=away_and_back=false;
 enters=leaves=runs=warnings=cursor_runs=0;outcome=PET_MINIAPP_ACTIVATION_OPENED;
}
int main(void){
 assert(!enqueue_component_activation(0,0));g_protocol_queue=0;assert(!enqueue_component_activation(9,0));
 g_protocol_queue=2;queue_full=true;assert(!enqueue_component_activation(9,0));
 queue_full=false;assert(enqueue_component_activation(9,0)&&queued.activation_token==9);
 reset();run_component_activation(9);assert(!strcmp(g_state.screen_page,"app")&&g_state.stats==7&&g_state.last_update_ms==1);
 assert(!holder&&enters==1&&leaves==1&&runs==1&&!warnings);
 reset();outcome=PET_MINIAPP_ACTIVATION_OPENED_UNSAVED;run_component_activation(9);
 assert(!strcmp(g_state.screen_page,"app")&&warnings==1&&runs==1);
 reset();outcome=PET_MINIAPP_ACTIVATION_FAILED;run_component_activation(9);
 assert(!strcmp(g_state.screen_page,"components")&&!g_state.stats&&warnings==1&&runs==1);
 reset();navigate_away=true;run_component_activation(9);assert(!strcmp(g_state.screen_page,"main")&&!g_state.stats&&cancelled);
 reset();away_and_back=true;run_component_activation(9);assert(!strcmp(g_state.screen_page,"components")&&!g_state.stats&&cancelled);
 reset();enter_failed=true;run_component_activation(9);assert(!runs&&!leaves&&cancelled&&!holder);
 for(int i=0;i<3;i++){reset();conversation=i==0;ota=i==1;g_state.asset_transfer_active=i==2;
  run_component_activation(9);assert(cancelled&&!strcmp(g_state.screen_page,"components")&&!g_state.stats&&!holder);}
 assert(!enqueue_catalog_cursor(0,0));g_protocol_queue=0;assert(!enqueue_catalog_cursor(9,0));
 g_protocol_queue=2;queue_full=true;assert(!enqueue_catalog_cursor(9,0));queue_full=false;
 assert(enqueue_catalog_cursor(9,0)&&queued.cursor_token==9&&!queued.activation_token);
 reset();run_catalog_cursor(9);
 assert(!holder&&enters==1&&leaves==1&&cursor_runs==1&&!runs&&!warnings);
 assert(!strcmp(g_state.screen_page,"components")&&!g_state.stats&&!g_state.last_update_ms);
 reset();enter_failed=true;run_catalog_cursor(9);assert(!cursor_runs&&!leaves&&!holder);
 return 0;
}
''')


def test_actual_physical_enter_does_not_open_on_queued_or_failed_prepare(tmp_path):
    source = (ROOT / 'main/pet_p4_input.c').read_text()
    source = function(source, 'apply_local_action')
    body = source.split('if (strcmp(binding->action, "page_enter") == 0) {', 1)[1]
    body = body.split('if (strcmp(binding->action, "page_back") == 0)', 1)[0].rstrip()
    assert body.endswith('}')
    body = body[:-1]
    execute(tmp_path, r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum {PET_MINIAPP_ACTIVATION_REJECTED,PET_MINIAPP_ACTIVATION_READY,PET_MINIAPP_ACTIVATION_QUEUED};
typedef int pet_p4_miniapp_activation_request_t;
typedef struct {char screen_page[16];unsigned last_update_ms;int stats;} state_t;
static int request_result,calls;
static int pet_p4_miniapp_activation_request_selected(void){calls++;return request_result;}
static void pet_p4_miniapp_sync_stats(int *stats){*stats=7;}
static void copy_text(char *out,size_t size,const char *text){snprintf(out,size,"%s",text);}
static bool enter(state_t *state,char *action_override,size_t action_override_size,unsigned ts_ms){
''' + body + r'''
}
int main(void){
 char action[48];state_t state;
 for(int outcome=0;outcome<3;outcome++){
  memset(&state,0,sizeof(state));strcpy(state.screen_page,"components");action[0]=0;request_result=outcome;calls=0;
  bool ok=enter(&state,action,sizeof(action),99);assert(calls==1&&ok==(outcome!=0));
  assert(!strcmp(state.screen_page,outcome==1?"app":"components"));assert(state.stats==(outcome==1?7:0));
  if(outcome==2)assert(!strcmp(action,"component_open_pending"));
 }
 strcpy(state.screen_page,"main");calls=0;assert(!enter(&state,action,sizeof(action),100)&&!calls);
 return 0;
}
''')


def test_internal_work_never_enters_json_parser_or_replays_wire_request():
    main = (ROOT / 'main/pet_p4_main.c').read_text()
    worker = main.split('static void protocol_worker_task(', 1)[1].split('static bool consume_byte(', 1)[0]
    assert worker.index('if (request.activation_token)') < worker.index('maybe_handle_transport_audit(')
    branch = worker.split('if (request.activation_token)', 1)[1].split('bool handled_debug', 1)[0]
    assert 'run_component_activation(request.activation_token)' in branch and 'continue;' in branch
    assert main.index('pet_p4_miniapp_activation_configure(') < main.index('ESP_ERROR_CHECK(init_usb())')
    for name in ('pet_p4_input.c', 'pet_p4_touch.c'):
        source = (ROOT / 'main' / name).read_text()
        assert 'pet_p4_miniapp_catalog_activate_selected()' not in source
        assert 'PET_MINIAPP_ACTIVATION_READY' in source
        assert 'pet_p4_miniapp_activation_guard(' in source
