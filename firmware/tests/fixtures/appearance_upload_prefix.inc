#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <dirent.h>
#include "esp_partition.h"
#include "pet_p4_raw_assets.h"
#include "pet_p4_resources.h"
#include "pet_p4_appearance_session.h"
#include "cJSON.h"
#include "pet_p4_assets.h"
#include "pet_p4_raw_assets.h"
#include "pet_p4_json_source.h"
#define PET_P4_PACK_ID_MAX 65
#define PET_P4_DEVICE_ID_MAX 64
#define PET_P4_ASSET_MANIFEST_MAX 65536
#define PET_P4_FS_PATH_MAX 96
#define PET_P4_LOGICAL_PATH_MAX 128
#define PET_P4_ASSET_SLOT_COUNT 2
#define PET_P4_JSON_SAFE_INTEGER_MAX 9007199254740991ULL
#define PET_P4_SPIFFS_PREFIX "."
#define PET_P4_ASSET_SLOT_MARKER "./p4slot.txt"
#define PET_P4_ASSET_BEGIN_GC_SLICE_BYTES 4096
#define PET_P4_ASSET_BEGIN_GC_BYTES 65536
#define PET_P4_DECODED_CHUNK_LIMIT 65536
#define PET_P4_RAW_ASSET_CHUNK_LIMIT 65536
#define PET_P4_ASSET_TRANSFER_IDLE_TIMEOUT_MS 15000
#define PET_APPEARANCE_SERIAL 1
#define PET_APPEARANCE_NATIVE 2
#define PET_P4_NATIVE_KIND_JSON 1
#define PET_P4_NATIVE_KIND_FILE_BEGIN 2
#define PET_P4_NATIVE_KIND_FILE_DATA 3
#define PET_P4_NATIVE_KIND_FILE_END 4
#define PET_P4_NATIVE_KIND_COMMIT 5
#define PET_P4_NATIVE_KIND_PING 6
#define pdMS_TO_TICKS(n) (n)
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define configASSERT assert
#define PET_P4_GAP_PEAK_HOLDER 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef unsigned TickType_t;typedef int BaseType_t;typedef void*TaskHandle_t;typedef void(*pet_p4_send_line_fn)(const char*,void*);
typedef struct{pet_p4_asset_catalog_t asset_catalog;unsigned asset_revision;bool asset_transfer_active;char board_device_id[64];}pet_p4_runtime_state_t;
typedef struct{bool(*begin)(void*);void(*end)(void*);bool(*current)(void*);void*context;}pet_appearance_commit_io_t;
typedef enum{PET_APPEARANCE_COMMIT_FAILED,PET_APPEARANCE_COMMIT_OK,PET_APPEARANCE_COMMIT_UNKNOWN}pet_appearance_commit_result_t;
typedef struct{bool transfer_active,receiving,worker_pending;uint32_t epoch;}pet_p4_native_io_snapshot_t;
typedef struct{bool asset_transfer,raw_receiving,appearance_owner,file_open,slot_pending,native_transfer,native_receiving,native_pending,worker_pending,storage_pending,firmware_ota,reboot_pending;uint32_t native_epoch;}appearance_io_observation_t;
static pet_p4_runtime_state_t state,*g_state=&state;
static struct{char id[17];}g_boot_identity;
static char last_ack_line[2048];
static pet_resource_owner_t admission_owner;static pet_resource_lease_t admission_lease;
static unsigned admission_delays,ack_owner_pins;static bool admission_release,admission_owner_change,admission_same_id,admission_route_change,fail_raw_buffer,fail_capture_alloc;
static pet_appearance_session_t g_appearance_session;
static pet_resource_broker_t broker;
static pet_resource_realtime_t header_realtime;static bool header_preempt;static unsigned header_fault;
static _Atomic int g_asset_active_slot=0;
static int g_asset_transfer_slot=-1;static char g_asset_transfer_id[64];static unsigned long long g_asset_transfer_last_activity_ms;
static pet_p4_send_line_fn g_asset_sender;static void*g_asset_route;
static bool g_asset_phase_write_attempted;
static FILE*g_asset_chunk_file;static char g_asset_chunk_logical_path[128],g_asset_chunk_error[128];
static unsigned long long g_asset_chunk_size,g_asset_chunk_checksum=0xcbf29ce484222325ULL;
static bool g_asset_chunk_stat_valid;
static struct{bool active;unsigned char*data;size_t expected_size,received_size;unsigned long long expected_checksum;char transfer_id[64],logical_path[128],index[24];}g_raw_asset_chunk;
static _Atomic bool g_asset_cleanup_due,g_native_cleanup_due;
static char g_transfer_id[64],g_cleaned_transfer_id[64],g_current_path[96],g_expected_checksum[17];
static unsigned long long g_expected_size,g_received_size,g_transfer_last_activity_ms;
static int g_target_slot=-1;static bool g_native_file_raw,g_native_failed,g_native_preserve_raw,g_native_write_attempted;
static uint64_t g_native_raw_limit,g_native_raw_bytes,g_native_total_bytes;
static uint32_t g_native_chunk_index,g_native_files;
static _Atomic uint32_t g_connection_generation=1;static uint32_t g_native_transfer_connection;static uint32_t g_native_dispatch_connection=1;
typedef struct{char id[64],path[96],phase[24];bool seen,ok,invalid;}native_phase_ack_t;
static native_phase_ack_t*g_native_phase_reply;
static void*g_state_mutex=(void*)9,*g_native_worker_task=(void*)2,*g_protocol_task=(void*)1;
static void*task=(void*)1;static bool held=true,interleaving,probe,quarantine_injected;
static bool g_protocol_storage_command_entered;static uint32_t g_worker_locked_topic_hash;
static unsigned trace_begins,trace_ends;
static void worker_locked_begin(void){assert(held);trace_begins++;}
static void worker_locked_end(void){assert(held);trace_ends++;}
static uint32_t pet_topic_hash(const char*s,size_t n){uint32_t v=2166136261U;for(size_t i=0;i<n;i++)v=(v^(uint8_t)s[i])*16777619U;return v;}
static bool pet_p4_miniapp_storage_lane_enter(void){assert(held&&task==(void*)1);return true;}
static bool pet_p4_miniapp_storage_lane_leave(void){assert(held&&g_protocol_storage_command_entered);return true;}
static unsigned json_allocations,json_frees;
static void*json_malloc(size_t n){void*p=malloc(n);if(p)json_allocations++;return p;}
static void json_free(void*p){if(p)json_frees++;free(p);}
static unsigned mode,io_calls,frames,queued,borrow_count,acks,negative_acks,positive_acks,erase_calls,write_calls,rename_calls,hash_calls;
static bool fail_close,fail_gc,fail_erase,fail_write,fail_rename,fail_open,fail_alloc;static unsigned fail_borrow;
static _Atomic bool protocol_window=true,query_window=true;
static unsigned long long clock_us=1000000;
static const esp_partition_t partitions[2]={{0,262144,0},{262144,262144,1}};
static uint8_t flash_bytes[2][262144];static unsigned raw_critical;
static void interleave(void);
static void* xTaskGetCurrentTaskHandle(void){return task;}
static void* xSemaphoreGetMutexHolder(void*m){return held?task:NULL;}
static int xSemaphoreGive(void*m){assert(held);held=false;if(!probe)interleave();return pdTRUE;}
static int xSemaphoreTake(void*m,unsigned wait){assert(!held);held=true;return pdTRUE;}
static bool begin(void*c){if(!probe)borrow_count++;if(!probe&&fail_borrow&&borrow_count==fail_borrow)return false;if(!held)return false;return xSemaphoreGive(g_state_mutex)==pdTRUE;}
static void end(void*c){assert(xSemaphoreTake(g_state_mutex,portMAX_DELAY)==pdTRUE);}
static bool current(void*c){return true;}
static bool(*g_asset_query_io_begin)(void*)=begin;static void(*g_asset_query_io_end)(void*)=end;static void*g_asset_query_io_context;
static bool pending(void*c){return false;}
static bool(*g_asset_query_pending)(void*)=pending;static void*g_asset_query_pending_context;
#define APPEARANCE_OBSERVATION_SCHEMA "p4-appearance-quiescent-v1"
static const char*pet_p4_boot_identity(void){return "0000000000000001";}
static bool pet_p4_diagnostics_reboot_pending(void){return false;}
static bool pet_p4_ota_transfer_active(void){return false;}
static bool pet_p4_miniapp_storage_busy(void){return false;}
static void pet_p4_renderer_set_asset_preparation_suspended(bool b){assert(b);}
static long long esp_timer_get_time(void){return (long long)clock_us;}
static const char*esp_err_to_name(int e){return "host-error";}
typedef int esp_err_t;
static void boundary(void){assert(!held);io_calls++;interleave();}
void host_critical_enter(void*m){assert(!raw_critical++);}
void host_critical_leave(void*m){assert(raw_critical--==1);}
void vTaskDelay(unsigned ticks){
  assert(!held&&!raw_critical);clock_us+=(unsigned long long)ticks*1000;admission_delays++;
  if(admission_lease.serial&&admission_release){
    assert(pet_resources_finish(&broker,admission_lease)==PET_RESOURCE_FINISH_CURRENT);
    assert(pet_resources_owner_revoke(&broker,admission_owner));
    memset(&admission_lease,0,sizeof(admission_lease));memset(&admission_owner,0,sizeof(admission_owner));
  }
  if(admission_owner_change){
    pet_appearance_sender_t sender=g_appearance_session.sender;void*route=(void*)g_appearance_session.route;
    assert(pet_appearance_release(&g_appearance_session,sender,route,g_appearance_session.id,PET_APPEARANCE_SERIAL));
    assert(pet_appearance_claim(&g_appearance_session,sender,route,admission_same_id?"tx":"foreign",PET_APPEARANCE_SERIAL));
    state.asset_transfer_active=false;admission_owner_change=false;
  }
  if(admission_route_change){atomic_fetch_add(&g_connection_generation,1);state.asset_transfer_active=false;admission_route_change=false;}
  boundary();
}
const esp_partition_t*esp_partition_find_first(unsigned t,unsigned s,const char*l){assert(!held&&!raw_critical);return !strcmp(l,"appearance0")?&partitions[0]:!strcmp(l,"appearance1")?&partitions[1]:NULL;}
int esp_partition_read(const esp_partition_t*p,size_t off,void*out,size_t n){boundary();assert(off+n<=p->size);memcpy(out,flash_bytes[p->slot]+off,n);return ESP_OK;}
int esp_partition_write(const esp_partition_t*p,size_t off,const void*in,size_t n){boundary();write_calls++;assert(off+n<=p->size);memcpy(flash_bytes[p->slot]+off,in,n);interleave();return fail_write?1:ESP_OK;}
int esp_partition_erase_range(const esp_partition_t*p,size_t off,size_t n){boundary();erase_calls++;assert(off+n<=p->size);memset(flash_bytes[p->slot]+off,255,n);return fail_erase?1:ESP_OK;}
static int esp_spiffs_gc(const char*l,size_t n){boundary();return fail_gc?1:ESP_OK;}
static const char*disk_path(const char*p){return !strncmp(p,"/spiffs/",8)?p+8:!strcmp(p,"/spiffs")?".":p;}
static FILE*io_open(const char*p,const char*m){boundary();if(fail_open){errno=EIO;return NULL;}return fopen(disk_path(p),m);}
static int io_close(FILE*f){boundary();int r=fclose(f);return fail_close?EOF:r;}
static size_t io_write(const void*p,size_t z,size_t n,FILE*f){boundary();write_calls++;size_t r=fwrite(p,z,n,f);return fail_write&&r?r-1:r;}
static size_t io_read(void*p,size_t z,size_t n,FILE*f){boundary();return fread(p,z,n,f);}
static int io_remove(const char*p){boundary();return remove(disk_path(p));}
static int io_rename(const char*a,const char*b){boundary();rename_calls++;int r=rename(disk_path(a),disk_path(b));return fail_rename?-1:r;}
static DIR*io_opendir(const char*p){boundary();return opendir(disk_path(p));}
static struct dirent*io_readdir(DIR*d){boundary();return readdir(d);}
static int io_closedir(DIR*d){boundary();return closedir(d);}
static void*io_malloc(size_t n){
  if(n<512&&g_raw_asset_chunk.active&&header_fault){
    if(header_fault==1)g_raw_asset_chunk.received_size=1;
    if(header_fault==2)state.asset_revision++;
    header_fault=0;
  }
  return fail_alloc||(fail_raw_buffer&&n==8192)||(fail_capture_alloc&&n<512)?NULL:malloc(n);
}
#define malloc io_malloc
#define fopen io_open
#define fclose io_close
#define fwrite io_write
#define fread io_read
#define remove io_remove
#define rename io_rename
#define opendir io_opendir
#define readdir io_readdir
#define closedir io_closedir
static pet_resource_result_t pet_resources_device_owner_open(uint32_t m,pet_resource_owner_t*out){return pet_resources_owner_open(&broker,m,out);}
static pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t o,pet_resource_kind_t k,uint32_t u,pet_resource_lease_t*out){return pet_resources_try_acquire(&broker,o,k,u,out);}
static bool pet_resources_device_current(pet_resource_lease_t l){return pet_resources_current(&broker,l);}
static void pet_resources_device_revoke(pet_resource_owner_t o){(void)pet_resources_owner_revoke(&broker,o);}
static pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t l){return pet_resources_finish(&broker,l);}
static bool pet_p4_appearance_native_active(void);
static _Atomic uint32_t g_observation_epoch=1;static _Atomic unsigned g_pending_frames;static _Atomic int g_mount_pending;static _Atomic bool g_receiving;
static pet_p4_native_io_snapshot_t pet_p4_native_usb_io_snapshot(void);
static void transport_send_line(const char*l,void*c);
static void sender(const char*l,void*c){assert(!held);if(!probe){snprintf(last_ack_line,sizeof(last_ack_line),"%s",l);acks++;cJSON*r=cJSON_Parse(l);const cJSON*p=cJSON_GetObjectItemCaseSensitive(r,"payload");
if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"rawIdle"))&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"phaseCurrent"))){
  assert(g_appearance_session.operation_active&&!g_raw_asset_chunk.active);ack_owner_pins++;
  assert(!pet_appearance_release(&g_appearance_session,g_appearance_session.sender,g_appearance_session.route,g_appearance_session.id,g_appearance_session.kind));
  assert(!pet_appearance_claim(&g_appearance_session,g_appearance_session.sender,g_appearance_session.route,"swap-at-ack",PET_APPEARANCE_SERIAL));
}
bool ok=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"ok"));if(ok)positive_acks++;else negative_acks++;cJSON_Delete(r);}}
static void transport_send_line(const char*l,void*c){sender(l,c);}
static bool enqueue_protocol_line(const char*l,pet_p4_send_line_fn f,void*r,void*c){assert(!held&&f==transport_send_line&&!strcmp(l,"{}"));queued++;return true;}
static void*native_route_context(void){uint32_t c=atomic_load(&g_connection_generation);return c==UINT32_MAX?NULL:(void*)(uintptr_t)(c+1);}
bool pet_p4_native_usb_route_current(void*r){return r&&r==native_route_context();}
static void pet_p4_native_usb_send_json_line(const char*l,void*c){sender(l,c);}
static void send_topic_payload(pet_p4_send_line_fn send,void*ctx,const char*topic,cJSON*p){cJSON*r=cJSON_CreateObject();cJSON_AddStringToObject(r,"topic",topic);cJSON_AddItemToObject(r,"payload",p);char*s=cJSON_PrintUnformatted(r);if(s&&send)send(s,ctx);cJSON_free(s);cJSON_Delete(r);}
static void native_send_topic_payload(const char*t,cJSON*p){send_topic_payload(pet_p4_native_usb_send_json_line,NULL,t,p);}
static bool read_ready_slot_pack_id(int s,char*out,size_t n){boundary();return false;}
static int mbedtls_base64_decode(unsigned char*out,size_t cap,size_t*n,const unsigned char*in,size_t size){assert(!held);/* Only synthetic fixed AQIDBA== is a fixture. Production decode itself isn't replaced by an acceptance claim. */if(size!=8||memcmp(in,"AQIDBA==",8)||cap<4)return -1;memcpy(out,"\1\2\3\4",4);*n=4;return 0;}
static bool pet_p4_appearance_mutation_blocked(void);
static void handle_asset_activate(pet_p4_runtime_state_t*s,const cJSON*p,pet_p4_send_line_fn f,void*c){assert(!"activate is separate reviewed engine");}
static void handle_serial_asset_commit(pet_p4_runtime_state_t*s,pet_p4_send_line_fn f,void*c,const char*i,bool p){assert(!"commit is separate reviewed engine");}
static void handle_commit(const uint8_t*p,uint32_t n){assert(!"commit is separate reviewed engine");}
static void handle_json_frame(const uint8_t*,uint32_t);
static void handle_identity_ping(const uint8_t*p,uint32_t n){assert(!"identity not this test");}
static unsigned long long native_hash_file(FILE*f,unsigned long long*n);

/* SDK boundary probe: actual protocol functions below publish the cue gate. */
static bool cue_deferred;static unsigned cue_true_calls;
void pet_p4_audio_status_cues_defer(bool busy){cue_deferred=busy;if(busy)cue_true_calls++;}

/* Audio worker is outside this appearance-only fixture. Unexpected audio
 * mutation paths fail rather than fabricate a receipt. */
static bool audio_patch_idle_due(uint64_t now){(void)now;return false;}
static bool audio_patch_protocol_active(void){return false;}
static void handle_audio_patch_topic(pet_p4_runtime_state_t*s,const char*t,const cJSON*p,pet_p4_send_line_fn send,void*c){(void)s;(void)t;(void)p;(void)send;(void)c;assert(!"audio patch is not an appearance transaction");}
static void audio_patch_timeout_worker(pet_p4_runtime_state_t*s){(void)s;assert(!"audio patch worker is not active in appearance tests");}

typedef struct {char path[PET_P4_ASSET_PATH_MAX];uint32_t size;} asset_query_video_t;
static asset_query_video_t g_asset_query_videos[PET_P4_ASSET_CATALOG_MAX_FAMILIES];
static unsigned g_asset_query_video_count;
