"""Actual immutable query serialization/dispatch, no devices or sound.

Native transport/RTOS calls are assertions, not hardware latency evidence.
"""
from pathlib import Path
import configparser
import subprocess
from p4_control_integrity_test import _c_function as function

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'managed_components/espressif__cjson/cJSON'


def native(tmp_path, code, flags=(), cjson=False):
    source = tmp_path / 'query.c'
    source.write_text(code)
    binary = tmp_path / ('query-' + '-'.join(flags).replace('=', '_'))
    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
               '-I', str(ROOT / 'main'), '-I', str(VENDOR),
               *('-D' + flag for flag in flags), str(source)]
    if cjson:
        command.append(str(VENDOR / 'cJSON.c'))
    subprocess.run([*command, '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_actual_topic_serialization_owns_bytes_and_fails_without_replay(tmp_path):
    diagnostics = (ROOT / 'main/pet_p4_diagnostics.c').read_text()
    code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
static unsigned fail_at,allocations;
static void *allocate(size_t n){if(++allocations==fail_at)return NULL;return malloc(n);}
''' + function(diagnostics, 'prepare_topic') + r'''
int main(void){
 cJSON *payload=cJSON_CreateObject();assert(payload);
 cJSON_AddStringToObject(payload,"requestId","owned-query");
 cJSON_AddStringToObject(payload,"screen","before");
 char *line=prepare_topic("diagnostics/status",payload);assert(line);
 assert(strstr(line,"owned-query")&&strstr(line,"before"));
 cJSON *parsed=cJSON_Parse(line);assert(parsed);cJSON_Delete(parsed);cJSON_free(line);
 assert(!prepare_topic("diagnostics/status",NULL));
 /* Exercise EACH allocator failure, including final serialization realloc.
  * No transport callback is present; a failed preparation cannot dispatch. */
 for(unsigned offset=1;offset<18;offset++){
  cJSON_InitHooks(NULL);payload=cJSON_CreateObject();assert(payload);
  cJSON_AddStringToObject(payload,"requestId","owned-query");
  cJSON_Hooks hooks={allocate,free};allocations=0;fail_at=offset;cJSON_InitHooks(&hooks);
  line=prepare_topic("diagnostics/status",payload);if(line){
   cJSON_InitHooks(NULL);parsed=cJSON_Parse(line);assert(parsed);
   cJSON *topic=cJSON_GetObjectItemCaseSensitive(parsed,"topic");
   assert(cJSON_IsString(topic)&&!strcmp(topic->valuestring,"diagnostics/status"));
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(parsed,"payload")));
   cJSON_Delete(parsed);cJSON_free(line);
  }
 }
 cJSON_InitHooks(NULL);return 0;
}
'''
    native(tmp_path, code, cjson=True)


def test_actual_query_releases_diagnostics_before_sending_or_detaching(tmp_path):
    diagnostics = (ROOT / 'main/pet_p4_diagnostics.c').read_text()
    # The actual complete query branch, not the touch/reboot/input actions.
    branch = function(diagnostics, 'pet_p4_diagnostics_handle_topic_with_reply')
    branch = branch.split('  } else if (strcmp(topic, "diagnostics/touch-test")')[0]
    code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_query_reply.h"
#include "cJSON.h"
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
typedef void(*pet_p4_send_line_fn)(const char*,void*);
typedef struct{unsigned value;}pet_p4_runtime_state_t;
static unsigned held,sends,prepared;static int g_diagnostics_mutex=1;static bool fail_prepare;
static int xSemaphoreTake(int m,unsigned wait){assert(m==1&&wait==portMAX_DELAY&&!held);held=1;return pdTRUE;}
static void xSemaphoreGive(int m){assert(m==1&&held);held=0;}
static char *prepare_status(const cJSON *request,const pet_p4_runtime_state_t *state){
 assert(held&&request&&state->value==42);prepared++;
 if(fail_prepare)return NULL;
 cJSON *p=cJSON_CreateObject();assert(p);cJSON_AddNumberToObject(p,"value",state->value);
 char *s=cJSON_PrintUnformatted(p);cJSON_Delete(p);assert(s);return s;
}
static void send(const char *line,void *ctx){assert(!held&&ctx==(void*)17&&strstr(line,"42"));sends++;}
''' + branch + r'''
  }
  handled=false;xSemaphoreGive(g_diagnostics_mutex);return handled;
}
int main(void){
 pet_p4_runtime_state_t state={42};cJSON *p=cJSON_CreateObject();assert(p);
 pet_p4_query_reply_t reply={0};
 assert(pet_p4_diagnostics_handle_topic_with_reply("diagnostics/query",p,&state,send,(void*)17,&reply));
 assert(!held&&prepared==1&&reply.diagnostic_query);
#if PET_P4_QUERY_REPLY_DEFERRED
 assert(!sends&&reply.line&&strstr(reply.line,"42"));state.value=999;
 /* The state and request can change/die before TX; bytes stay immutable. */
 cJSON_Delete(p);p=cJSON_CreateObject();assert(p);assert(strstr(reply.line,"42"));
 send(reply.line,(void*)17);cJSON_free(reply.line);reply.line=NULL;state.value=42;
#else
 assert(sends==1&&!reply.line);
#endif
 fail_prepare=true;reply.diagnostic_query=false;
 assert(pet_p4_diagnostics_handle_topic_with_reply("diagnostics/query",p,&state,send,(void*)17,&reply));
 assert(!held&&prepared==2&&sends==1&&!reply.line&&reply.diagnostic_query);
 fail_prepare=false;
 assert(pet_p4_diagnostics_handle_topic_with_reply("diagnostics/query",p,&state,send,(void*)17,NULL));
 assert(!held&&prepared==3&&sends==2);
 reply.line=(char*)"occupied";
 assert(!pet_p4_diagnostics_handle_topic_with_reply("diagnostics/query",p,&state,send,(void*)17,&reply));
 assert(prepared==3&&sends==2&&!held&&!strcmp(reply.line,"occupied"));
 assert(!pet_p4_diagnostics_handle_topic_with_reply(NULL,p,&state,send,(void*)17,NULL));
 assert(!pet_p4_diagnostics_handle_topic_with_reply("diagnostics/query",NULL,&state,send,(void*)17,NULL));
 cJSON_Delete(p);return 0;
}
'''
    for mode in (0, 1):
        native(tmp_path, code, (f'PET_P4_QUERY_REPLY_DEFERRED={mode}',), cjson=True)


def test_actual_worker_dispatch_sends_once_after_state_unlock_and_preserves_route(tmp_path):
    main = (ROOT / 'main/pet_p4_main.c').read_text()
    worker = function(main, 'protocol_worker_task')
    block = 'if (!handled_debug) {' + worker.split('if (!handled_debug) {', 1)[1].split('    free(request.line);', 1)[0]
    # The real worker now starts observation BEFORE early native/debug paths.
    # Retain that exact outer declaration when extracting the normal branch.
    at=worker.index('    uint64_t query_begin=')
    begin=worker.rfind('#if PET_P4_QUERY_REPLY_TRACE',0,at)
    end=worker.index('    /* Debug handlers',at)
    assert begin>=0 and begin<at<end
    block=worker[begin:end]+block
    code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_query_reply.h"
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define configASSERT assert
typedef void(*send_fn)(const char*,void*);
typedef struct{char screen_page[16];bool asset_transfer_active;}state_t;
static state_t g_state={"main",false};static unsigned held,dispatches,sends,freed,leaves;
static bool g_protocol_storage_command_entered;
static int g_state_mutex=1;static unsigned long long clock_us;
#if PET_P4_QUERY_REPLY_TRACE
static pet_p4_query_timing_t g_query_timing;
static unsigned long long esp_timer_get_time(void){return ++clock_us;}
#endif
static int xSemaphoreTake(int m,unsigned w){assert(m==1&&w==portMAX_DELAY&&!held);held=1;return pdTRUE;}
static void xSemaphoreGive(int m){assert(m==1&&held);held=0;}
static bool pet_p4_miniapp_storage_lane_enter(void){assert(held);return true;}
static bool pet_p4_miniapp_storage_lane_leave(void){assert(held);leaves++;return true;}
static bool pet_p4_conversation_active(state_t *s){assert(s==&g_state&&held);return false;}
static bool pet_p4_ota_transfer_active(void){assert(held);return false;}
static void pet_p4_miniapp_activation_guard(bool b){assert(held&&!b);}
static void cJSON_free(char *p){assert(!held&&p);freed++;free(p);}
static bool mutation;
static void tx(const char *line,void *ctx){assert(ctx==(void*)123&&line);sends++;
#if PET_P4_QUERY_REPLY_DEFERRED
 assert(held==mutation);
#else
 assert(held);
#endif
}
static bool dispatch(state_t *s,const char *line,send_fn send,void *ctx,pet_p4_query_reply_t *reply){
 assert(s==&g_state&&held&&g_protocol_storage_command_entered&&!strcmp(line,"unchanged-request")&&send==tx&&ctx==(void*)123);dispatches++;
 if(reply)reply->diagnostic_query=!mutation;
 if(reply&&PET_P4_QUERY_REPLY_DEFERRED&&!mutation){
  reply->line=malloc(32);assert(reply->line);strcpy(reply->line,"immutable-response");
 }else send("direct-response",ctx);
 return true;
}
#if PET_P4_QUERY_REPLY_DEFERRED || PET_P4_QUERY_REPLY_TRACE
static bool pet_p4_handle_line_with_reply(state_t*s,const char*l,send_fn f,void*c,pet_p4_query_reply_t*r){return dispatch(s,l,f,c,r);}
#else
static bool pet_p4_handle_line(state_t*s,const char*l,send_fn f,void*c){return dispatch(s,l,f,c,NULL);}
#endif
static void test_dispatch(bool handled_debug){
 struct{const char *line;send_fn send_line;void *reply_ctx;}request={"unchanged-request",tx,(void*)123};
''' + block + r'''
 (void)request;
}
int main(void){
 for(unsigned m=0;m<2;m++){
  mutation=m;test_dispatch(false);assert(dispatches==m+1&&sends==m+1&&leaves==m+1&&!held&&!g_protocol_storage_command_entered);
 }
 assert(freed==PET_P4_QUERY_REPLY_DEFERRED);
#if PET_P4_QUERY_REPLY_TRACE
 assert(g_query_timing.sequence==1&&g_query_timing.detached==PET_P4_QUERY_REPLY_DEFERRED&&!g_query_timing.invalid_clock);
#endif
 (void)clock_us;return 0;
}
'''
    for deferred, trace in ((0, 0), (0, 1), (1, 0), (1, 1)):
        native(tmp_path, code, (f'PET_P4_QUERY_REPLY_DEFERRED={deferred}', f'PET_P4_QUERY_REPLY_TRACE={trace}'))


def test_query_timing_is_bounded_numeric_previous_completion_and_owner_scoped(tmp_path):
    native(tmp_path, r'''
#include <assert.h>
#include "pet_p4_query_reply.h"
int main(void){
 pet_p4_query_timing_t m={0};
 pet_p4_query_timing_record(&m,10,20,120,220,false);
 assert(m.sequence==1&&m.lock_wait_us==10&&m.state_held_us==100&&m.post_unlock_tx_us==100&&!m.detached&&!m.invalid_clock);
 pet_p4_query_timing_record(&m,230,235,240,245,true);
 assert(m.sequence==2&&m.lock_wait_us==5&&m.max_lock_wait_us==10&&m.max_state_held_us==100&&m.detached);
 m.sequence=UINT64_MAX;
 pet_p4_query_timing_record(&m,1,((uint64_t)UINT32_MAX)+10,((uint64_t)UINT32_MAX)+11,((uint64_t)UINT32_MAX)+12,true);
 assert(m.sequence==UINT64_MAX&&m.lock_wait_us==UINT32_MAX);
 pet_p4_query_timing_record(&m,100,90,80,70,false);
 assert(m.invalid_clock&&!m.lock_wait_us&&!m.state_held_us&&!m.post_unlock_tx_us&&m.max_state_held_us==100);
 return 0;
}
''', ('PET_P4_QUERY_REPLY_TRACE=1',))
    main = (ROOT / 'main/pet_p4_main.c').read_text()
    getter = function(main, 'pet_p4_protocol_query_timing')
    assert getter.index('xTaskGetCurrentTaskHandle() != g_protocol_task') < getter.index('*out = g_query_timing')
    diagnostics = (ROOT / 'main/pet_p4_diagnostics.c').read_text()
    status = function(diagnostics, 'prepare_status')
    assert 'previousDiagnosticQuery' in status and 'send_line' not in status


def test_query_detachment_is_explicit_private_comparison_only():
    header = (ROOT / 'main/pet_p4_query_reply.h').read_text()
    assert '#define PET_P4_QUERY_REPLY_DEFERRED 0' in header
    assert '#define PET_P4_QUERY_REPLY_TRACE 0' in header
    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / 'platformio.ini')
    for section in config.sections():
        flags = config[section].get('build_flags', '')
        if section == 'env:esp32_p4_query_lock_probe':
            assert config[section]['extends'] == 'env:esp32_p4_framework_mixed_acceptance'
            assert 'PET_P4_QUERY_REPLY_DEFERRED=1' in flags and 'PET_P4_QUERY_REPLY_TRACE=1' in flags
        elif section == 'env:esp32_p4_query_lock_control':
            assert config[section]['extends'] == 'env:esp32_p4_framework_mixed_acceptance'
            assert 'PET_P4_QUERY_REPLY_TRACE=1' in flags and 'PET_P4_QUERY_REPLY_DEFERRED' not in flags
        else:
            assert 'PET_P4_QUERY_REPLY_DEFERRED=1' not in flags and 'PET_P4_QUERY_REPLY_TRACE=1' not in flags
    protocol = (ROOT / 'main/pet_p4_protocol.c').read_text()
    assert 'pet_p4_diagnostics_handle_topic_with_reply(topic, payload, state, send_line, ctx, reply)' in protocol
    assert 'pet_app_host_transport(send_line,ctx,state->board_device_id)' in function(protocol, 'pet_p4_handle_line_with_reply')
