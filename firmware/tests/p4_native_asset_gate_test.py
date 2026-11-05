"""Production native storage phases close preparation before each write.

No USB: actual handlers run against a temporary cwd and the real resource core.
File wrappers fail if a mutation starts with the preparation latch still open.
"""
from pathlib import Path
import subprocess

from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def test_native_reused_transfer_id_closes_gate_before_storage_mutations(tmp_path):
    from p4_appearance_safety_test import native_harness, run_c
    code=native_harness()
    native=(ROOT/'main/pet_p4_usb_native.c').read_text()
    protocol=(ROOT/'main/pet_p4_protocol.c').read_text()
    header=(ROOT/'main/pet_p4_protocol.h').read_text()
    # Reuse only the established synthetic filesystem/SDK boundary and actual
    # commit closure. New upload admission/phase/ACK logic is extracted below,
    # never replaced with successful phase/resource stubs.
    a=header.index('typedef struct {\n  pet_resource_owner_t owner;')
    phase_type=header[a:header.index('bool pet_p4_appearance_phase_begin',a)]
    end=native.index('native_phase_ack_t;')+len('native_phase_ack_t;')
    reply_type=native[native.rfind('typedef struct',0,end):end]
    declarations=phase_type+reply_type+r'''
void pet_p4_audio_status_cues_defer(bool busy){(void)busy;}
static native_phase_ack_t *g_native_phase_reply;
static bool g_native_write_attempted;
static _Atomic bool g_native_cleanup_due;
static uint32_t g_native_dispatch_connection=1;
#define PET_P4_NATIVE_KIND_JSON 1
#define PET_P4_NATIVE_KIND_FILE_BEGIN 2
#define PET_P4_NATIVE_KIND_FILE_DATA 3
#define PET_P4_NATIVE_KIND_FILE_END 4
#define PET_P4_NATIVE_KIND_COMMIT 5
#define PET_P4_NATIVE_KIND_PING 6
/* Unexpected generic protocol/PING dispatch is forbidden in this focused case. */
static void (*g_enqueue_protocol)(const char*,pet_p4_send_line_fn,void*,void*);
static void *g_enqueue_protocol_ctx;
static void pet_p4_handle_line(pet_p4_runtime_state_t*s,const char*l,pet_p4_send_line_fn f,void*c){assert(!"unexpected generic protocol dispatch");}
static void handle_identity_ping(const uint8_t*p,uint32_t n){assert(!"unexpected identity ping");}
/* TX boundary decodes the ACTUAL production ACK payload, never forces OK. */
static void send_topic_payload(const char*t,cJSON*p){
 assert(!held&&!strcmp(t,"asset/ack"));ack_ok=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"ok"));
 snprintf(ack_phase,sizeof(ack_phase),"%s",cJSON_GetObjectItemCaseSensitive(p,"phase")->valuestring);cJSON_Delete(p);
}
'''
    anchor=function(code,'send_asset_ack')
    code=code.replace(anchor,declarations+function(native,'send_asset_ack'),1)
    # native_harness already extracts both actual native route helpers once.
    for name in ('native_route_context','pet_p4_native_usb_route_current'):
        assert code.count(function(native,name))==1
    extra='\n'.join(function(protocol,n) for n in ['appearance_phase_try_begin','pet_p4_appearance_phase_begin','pet_p4_appearance_phase_end'])
    code=code.replace(function(code,'clear_slot_files'),extra+'\n'+function(code,'clear_slot_files'),1)
    at=code.index('static const unsigned char data[]=')
    extra='\n'.join(function(native,n) for n in ['native_failure_locked','handle_json_frame','handle_complete_frame'])
    code=code[:at]+extra+'\n'+code[at:]
    # These test helpers now invoke the real locked entry point, not its
    # intentionally unlocked leaf handlers.
    code=code.replace('handle_file_begin((const uint8_t*)meta,(uint32_t)strlen(meta));','handle_complete_frame(PET_P4_NATIVE_KIND_FILE_BEGIN,(const uint8_t*)meta,(uint32_t)strlen(meta));')
    code=code.replace('handle_file_end((const uint8_t*)meta,(uint32_t)strlen(meta));','handle_complete_frame(PET_P4_NATIVE_KIND_FILE_END,(const uint8_t*)meta,(uint32_t)strlen(meta));')
    code=code.replace('handle_commit((const uint8_t*)meta,(uint32_t)strlen(meta));','handle_complete_frame(PET_P4_NATIVE_KIND_COMMIT,(const uint8_t*)meta,(uint32_t)strlen(meta));')
    code=code.replace('assert(handle_asset_abort_json((const uint8_t*)meta,(uint32_t)strlen(meta)));','handle_complete_frame(PET_P4_NATIVE_KIND_JSON,(const uint8_t*)meta,(uint32_t)strlen(meta));')
    code=code[:code.index('int main(void){')]+r'''
int main(void){
 snprintf(manifest,sizeof(manifest),"{\"packId\":\"%s\",\"format\":\"p4-h264-v1\",\"width\":640,\"height\":480,\"fps\":10,\"families\":[{\"family\":\"idle\",\"path\":\"p4/clip.h264\",\"frames\":1,\"streamBytes\":3}]}",PACK);
 raw_available=false;fail_at=0;
 for(unsigned round=0;round<2;round++){
  suspended=false;worker_begin();begin("same-id","p4/test.bin",data,3);assert(ack_ok&&held);worker_retire();
  handle_complete_frame(PET_P4_NATIVE_KIND_FILE_DATA,data,3);end("same-id","p4/test.bin");assert(ack_ok&&held);
  begin("same-id","p4/manifest.json",(const uint8_t*)manifest,strlen(manifest));assert(ack_ok);
  handle_complete_frame(PET_P4_NATIVE_KIND_FILE_DATA,(const uint8_t*)manifest,(uint32_t)strlen(manifest));end("same-id","p4/manifest.json");assert(ack_ok);
  commit("same-id",2,3+(unsigned)strlen(manifest));assert(ack_ok&&!state.asset_transfer_active&&!pet_p4_appearance_active()&&held);
 }
 suspended=false;worker_begin();fail_at=7;
 begin("open-failure","p4/test.bin",data,3);assert(!ack_ok&&g_native_failed&&held);worker_retire();
 // A write-attempt failure is UNKNOWN, not permission to falsely abort/reset.
 assert(g_appearance_session.recovery_blocked);unsigned effects=writes+erases;
 fail_at=0;abort_id("open-failure");assert(!ack_ok&&pet_p4_appearance_mutation_blocked()&&effects==writes+erases);
 pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);assert(!usage.leases[PET_RESOURCE_STORAGE_WORK]&&!usage.live_owners);
 return 0;
}
'''
    # The established 'native' target links the actual asset parser and its
    # explicit host heap boundary in addition to the real resource broker.
    run_c(tmp_path,code,'native',resources=True)


def test_native_and_main_gate_transitions_share_existing_state_mutex():
    native=(ROOT/'main/pet_p4_usb_native.c').read_text()
    worker=function(native,'native_protocol_worker_task')
    # The timed idle-cleanup take/give is a separate, earlier branch. Match
    # dispatch's own take through its own release; never use first-give index.
    dispatch=worker.index('    if (g_state_mutex) xSemaphoreTake(g_state_mutex, portMAX_DELAY);')
    assert dispatch < worker.index('g_native_dispatch_connection=frame.connection;',dispatch) < worker.index('handle_complete_frame(',dispatch) < worker.index('xSemaphoreGive(g_state_mutex',dispatch)
    cleanup=worker[:dispatch]
    assert cleanup.index('xSemaphoreTake(g_state_mutex') < cleanup.index('native_idle_cleanup_worker();') < cleanup.index('xSemaphoreGive(g_state_mutex')
    assert worker.index('if(frame.connection!=atomic_load_explicit(') < dispatch
    main=(ROOT/'main/pet_p4_main.c').read_text()
    loop=main[main.index('    pet_p4_asset_transfer_process(&g_state, now_ms);'):]
    compact = ''.join(loop.split())
    suspension = ('pet_p4_renderer_set_asset_preparation_suspended('
                  'transfer_active||pet_p4_miniapp_storage_busy()||pet_p4_appearance_active());')
    assert compact.index('booltransfer_active=g_state.asset_transfer_active||firmware_transfer_active;') < compact.index(suspension) < compact.index('xSemaphoreGive(g_state_mutex);')
    # This is the wiring guard. Real locked mutation/suspension is exercised by
    # test_native_reused_transfer_id_closes_gate_before_storage_mutations, and
    # p4_phase2_transaction_test's actual activation/main-lock interleavings.
