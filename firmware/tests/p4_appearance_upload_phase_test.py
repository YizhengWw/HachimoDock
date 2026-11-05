"""Real production phase/RX/native closures + real raw writer and resource broker.
Only SDK/OS boundary adapters are synthetic. No hardware, network or PIO.
"""
from pathlib import Path
import hashlib,json,os,re,subprocess
import pytest
ROOT=Path(os.environ.get('PET_APPEARANCE_TEST_ROOT',str(Path(__file__).resolve().parents[1])))
S=Path(os.environ.get('PET_APPEARANCE_TEST_MAIN',str(ROOT/'main')))
FIXTURES=Path(os.environ.get('PET_APPEARANCE_TEST_FIXTURES',str(Path(__file__).resolve().parent/'fixtures')))
def bounds(s,name):
 pat=r'(?m)^[A-Za-z_][\w \t*]*\b'+re.escape(name)+r'\s*\([^;{}]*?\)\s*\{'
 m=re.search(pat,s);assert m,name
 at=s.index('{',m.start());mask=re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',lambda m:' '*len(m[0]),s,flags=re.S);depth=1;i=at+1
 while depth:
  if mask[i]=='{':depth+=1
  elif mask[i]=='}':depth-=1
  i+=1
 return m.start(),i

def fn(t,n):
 a,b=bounds(t,n);return t[a:b]

def build_upload_binary(B):
 code=(S/'pet_p4_protocol.c').read_text();native=(S/'pet_p4_usb_native.c').read_text();header=(S/'pet_p4_protocol.h').read_text()
 fixture=(FIXTURES/'appearance_upload_prefix.inc').read_text()
 # Actual private production struct/API definitions.
 a=header.index('typedef struct {\n  pet_resource_owner_t owner;');b=header.index('bool pet_p4_appearance_phase_begin',a);fixture+=header[a:b]
 fixture+='\n'.join((fn(code,n).replace('{','{if(len>128){assert(!held);hash_calls++;}',1) if n=='fnv1a64_bytes' else fn(code,n))+'\n' for n in ['copy_text','fnv1a64_update','fnv1a64_bytes','fnv1a64_file','json_string','json_u64','path_is_p4_asset','slot_is_valid','pet_p4_appearance_claim','pet_p4_appearance_owned','pet_p4_appearance_end','pet_p4_appearance_active','pet_p4_appearance_native_active','pet_p4_appearance_mutation_blocked','close_asset_chunk_file','reset_asset_chunk_tracking','reset_raw_asset_chunk','run_spiffs_gc_yielding','clear_slot_files','ensure_asset_transfer_slot','note_asset_transfer_activity','abort_serial_asset_transfer','pet_p4_asset_transfer_process','p4_asset_fs_path_for_slot','pet_p4_asset_fs_path_for_slot','read_active_slot_marker','pet_p4_asset_active_slot','p4_asset_fs_path','read_file_stat','send_asset_ack_indexed','send_asset_ack','send_asset_stat_ack','appearance_phase_try_begin','pet_p4_appearance_phase_begin','serial_raw_phase_begin_wait','pet_p4_appearance_phase_end','appearance_io_observe','appearance_io_busy','asset_query_refuse','asset_query_modern','asset_query_bound','asset_query_not_proof','asset_query_snapshot_videos','asset_query_video_proof','send_asset_slot_query_ack','write_asset_chunk_bytes','handle_asset_chunk','begin_raw_asset_chunk','pet_p4_raw_asset_chunk_active_for_route','pet_p4_raw_asset_chunk_active','handle_asset_stat','handle_asset_file'] if n not in ['appearance_phase_try_begin','serial_raw_phase_begin_wait'] or re.search(r'(?m)^static bool '+n+r'\(',code))
 if 'finish_unpublished_raw_header' in code:
  a=code.index('enum {\n  RAW_PHASE_OPERATION=')
  b=code.index('static void asset_phase_capture(',a)
  fixture+=code[a:b]+'\n'
 else:
  fixture+='\ntypedef struct {char *line;bool ok,seen,invalid;const char *error_code;}asset_phase_reply_t;\n'
 fixture+='\n'.join(fn(code,n)+'\n' for n in ['asset_phase_capture','asset_phase_send_current'])
 if 'raw_ready_rejection_snapshot' in code:
  a=code.index('typedef struct {\n  bool owner_matched,phase_current,raw_idle,header_accepted;')
  b=code.index('static raw_ready_rejection_t raw_ready_rejection_snapshot',a)
  fixture+=code[a:b]+'\n'
  fixture+='\n'.join(fn(code,n)+'\n' for n in ['raw_ready_rejection_snapshot','raw_ready_rejection_message','send_raw_ready_rejection'])
 fixture+='\n'.join(fn(code,n)+'\n' for n in ['asset_phase_deliver','serial_retire_locked','handle_asset_topic','pet_p4_asset_transfer_cleanup_worker','pet_p4_consume_raw_asset_bytes_scoped'])
 # Native and serial helpers share simple names; isolate native writer names via namespacing, keeping bodies unchanged.
 native_names=['pet_p4_native_usb_io_snapshot','native_now_ms','native_owned','native_retire','parse_payload_json','send_asset_ack','clear_slot_files','ensure_transfer_slot','handle_file_begin','handle_file_data','handle_file_end','native_commit_current','native_commit_io_begin','native_commit_io_end','handle_asset_abort_json','handle_json_frame','handle_identity_ping','native_failure_locked','native_idle_cleanup_worker','handle_complete_frame','pet_p4_native_usb_process']
 main=(S/'pet_p4_main.c').read_text()
 fixture+='\nvoid pet_p4_asset_transfer_cleanup_worker(pet_p4_runtime_state_t*);'
 fixture+='\nstatic void storage_command_unlock(void*c){assert(xSemaphoreGive(g_state_mutex)==pdTRUE);}'
 fixture+='\nstatic void storage_command_lock(void*c){assert(xSemaphoreTake(g_state_mutex,portMAX_DELAY)==pdTRUE);}'

 fixture+='\n'.join(re.sub(r'&g_state\b','&state',fn(main,n))+'\n' for n in ['raw_asset_rx_io_begin','raw_asset_rx_io_end','raw_asset_rx_current','consume_byte','consume_transport_bytes','asset_idle_worker'])
 fixture+='\ntypedef struct {uint32_t connection;}native_commit_context_t;\n'
 for n in native_names:
  t=fn(native,n)
  # JSON and PING aren't under test and require unrelated service wrappers: use
  # declarations in prefix; do not link fake implementations of FILE/abort/idle paths.
  if n in ['handle_json_frame','handle_identity_ping']:continue
  for a,b in [('fnv1a64_file','native_hash_file'),('send_asset_ack','native_send_asset_ack'),('clear_slot_files','native_clear_slot_files')]:t=re.sub(r'\b'+a+r'\b',b,t)
  if n=='send_asset_ack':t=t.replace('send_topic_payload("asset/ack", payload);','native_send_topic_payload("asset/ack", payload);')
  fixture+=t+'\n'
 fixture+=(FIXTURES/'appearance_upload_suffix.inc').read_text()
 (B/'upload.c').write_text(fixture)
 (B/'freertos').mkdir()
 (B/'freertos/FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\ntypedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\nvoid host_critical_enter(void*);void host_critical_leave(void*);\n#define portENTER_CRITICAL(m) host_critical_enter(m)\n#define portEXIT_CRITICAL(m) host_critical_leave(m)\n#define pdMS_TO_TICKS(n) (n)\n')
 (B/'freertos/task.h').write_text('#pragma once\nvoid vTaskDelay(unsigned);\n')
 (B/'esp_partition.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\n#define ESP_OK 0\n#define ESP_PARTITION_TYPE_DATA 1\n#define ESP_PARTITION_SUBTYPE_ANY 2\ntypedef struct {uint32_t address,size;unsigned slot;}esp_partition_t;\nconst esp_partition_t*esp_partition_find_first(unsigned,unsigned,const char*);\nint esp_partition_read(const esp_partition_t*,size_t,void*,size_t);\nint esp_partition_write(const esp_partition_t*,size_t,const void*,size_t);\nint esp_partition_erase_range(const esp_partition_t*,size_t,size_t);\n')
 cmd=['/usr/bin/cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(B),'-I',str(S),'-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(B/'upload.c'),str(S/'pet_p4_raw_assets.c'),str(ROOT/'main/pet_p4_resources.c'),str(ROOT/'tests/vendor/cjson/cJSON.c'),'-o',str(B/'upload')]
 (B/'compile-invocation.json').write_text(json.dumps({'argv':cmd,'cwd':str(B),'sources':{'upload.c':hashlib.sha256(fixture.encode()).hexdigest(),'raw':hashlib.sha256((S/'pet_p4_raw_assets.c').read_bytes()).hexdigest()}},indent=2))
 r=subprocess.run(cmd,cwd=B,env={**os.environ,'HOME':str(B),'TMPDIR':str(B)},stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60)
 (B/'compile.log').write_text(r.stdout)
 assert r.returncode==0,r.stdout
 return B/'upload'

CASES=['cue-owner-lifetime','cue-state-and-quarantine','header-lease-preempt','header-preempt-capture-invalid','header-body-before-grant','header-revision-invalid','header-already-granted','header-busy-grant','header-busy-timeout','header-busy-owner-change','header-busy-same-id-new-generation','header-busy-route-change','header-protected','header-foreign','header-capture-invalid','header-no-memory','header-invalid','header-active-zero-remaining','raw-storage-busy','native-fs-begin-close','native-fs-data-close','native-fs-data-write','query-idle', 'query-reborrow-fail', 'serial-admission', 'serial-raw-oom', 'serial-file-ok', 'serial-abort-fail', 'raw-route', 'native-fs-ok', 'native-fs-rename-fail', 'native-fs-close-fail', 'native-stale-frame', 'native-invalid-existing', 'serial-begin', 'serial-begin-erase', 'serial-begin-gc', 'serial-chunk', 'serial-file-close', 'serial-abort', 'serial-idle', 'serial-stat', 'raw-8192', 'raw-65536', 'raw-write-fail', 'raw-stale-revision', 'native-begin', 'native-begin-erase', 'native-data', 'native-data-fail', 'native-disconnect', 'native-abort', 'native-idle']

@pytest.fixture(scope='module')
def appearance_upload_binary(tmp_path_factory):
 return build_upload_binary(tmp_path_factory.mktemp('appearance-upload-c'))

def run_upload_case(binary,tmp_path,case):
 work=tmp_path/case;work.mkdir()
 argv=[str(binary),case]
 env={'PATH':'/usr/bin:/bin','HOME':str(work),'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0'}
 (work/'invocation.json').write_text(json.dumps({'argv':argv,'cwd':str(work),'env':env},indent=2))
 r=subprocess.run(argv,cwd=work,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=15)
 (work/'output.log').write_text(r.stdout)
 assert r.returncode==0,r.stdout

@pytest.mark.parametrize('case',CASES)
def test_actual_upload_phase_lock_owner_fault_and_delivery(appearance_upload_binary,tmp_path,case):
 run_upload_case(appearance_upload_binary,tmp_path,case)

def test_actual_native_partial_frame_and_queue_keep_original_connection(tmp_path):
 source=(S/'pet_p4_usb_native.c').read_text()
 code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#define PET_P4_NATIVE_HEADER_LEN 16
#define PET_P4_NATIVE_MAGIC "P4BU"
#define PET_P4_NATIVE_VERSION 1
#define PET_P4_NATIVE_MAX_PAYLOAD 65536
#define PET_P4_NATIVE_KIND_JSON 1
#define PET_P4_NATIVE_KIND_PING 6
#define ESP_LOGW(...) ((void)0)
#define pdTRUE 1
typedef struct{uint8_t kind;uint32_t connection,seq,payload_len;uint8_t*payload;}pet_p4_native_rx_frame_t;
static uint8_t g_header[16];static size_t g_header_used;static uint8_t*g_payload;
static uint32_t g_payload_len,g_payload_used,g_frame_seq,g_frame_connection;
static uint8_t g_frame_kind;static _Atomic uint32_t g_connection_generation=1;
static _Atomic unsigned g_pending_frames;static unsigned epochs,queued;static int g_rx_queue=1;
static pet_p4_native_rx_frame_t saved;
static void observation_changed(void){epochs++;}
static int xQueueSend(int q,const pet_p4_native_rx_frame_t*f,unsigned wait){assert(q==1&&!wait);saved=*f;queued++;return pdTRUE;}
'''
 for n in ['le32','put_le32','enqueue_complete_frame','reset_rx_frame','start_payload_from_header','feed_rx_bytes']:
  code+=fn(source,n)+'\n'
 code+=r'''
int main(void){
 uint8_t packet[24]={0};memcpy(packet,"P4BU",4);packet[4]=1;packet[5]=2;put_le32(packet+8,9);put_le32(packet+12,8);memcpy(packet+16,"12345678",8);
 feed_rx_bytes(packet,20);assert(!queued&&g_header_used==16&&g_payload_used==4&&g_frame_connection==1);
 atomic_store(&g_connection_generation,2);
 /* Fresh connection starts a fresh frame; no old prefix/payload is adopted. */
 feed_rx_bytes(packet,24);assert(queued==1&&saved.connection==2&&saved.payload_len==8&&!memcmp(saved.payload,"12345678",8));
 assert(!g_payload&&!g_header_used&&g_pending_frames==1);free(saved.payload);
 atomic_store(&g_connection_generation,3);assert(saved.connection!=atomic_load(&g_connection_generation));
 return 0;
}
'''
 src=tmp_path/'epoch.c';src.write_text(code);exe=tmp_path/'epoch'
 argv=['/usr/bin/cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(exe)]
 subprocess.run(argv,cwd=tmp_path,check=True,timeout=30)
 subprocess.run([str(exe)],cwd=tmp_path,env={'HOME':str(tmp_path),'PATH':'/usr/bin:/bin'},check=True,timeout=10)

def test_actual_phase_extractor_skips_helper_forward_declaration():
 from p4_control_integrity_test import _c_function
 source=(S/'pet_p4_protocol.c').read_text()
 assert fn(source,'asset_phase_send_current').strip()==_c_function(source,'asset_phase_send_current').strip()

def test_cue_gate_precedes_actual_standalone_storage_admission():
 code=(S/'pet_p4_protocol.c').read_text()
 for name in ['send_asset_slot_query_ack','handle_asset_activate']:
  actual=fn(code,name)
  assert actual.index('pet_p4_audio_status_cues_defer(true)')<actual.index('pet_resources_device_owner_open')
 worker=fn((S/'pet_p4_audio.c').read_text(),'playback_task')
 assert 'status_cue_realtime_begin(&file_playback_work,&request)' in worker
 assert 'pet_resources_device_realtime_begin(&file_playback_work)' not in worker
