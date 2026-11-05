/*
 * [Input] TinyUSB vendor frames for JSON control and raw asset payloads.
 * [Output] framed native-USB responses, callback-safe queued protocol work,
 *          serialized runtime mutations, transactional asset writes,
 *          restart-safe/idle-timeout slot cleanup, and legacy-pack migration.
 * [Pos] ESP32-P4 high-speed USB device transport.
 * [Sync] If this file changes, update firmware/protocol.md and .folder.md.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <limits.h>

#include "cJSON.h"
#include "pet_p4_json_source.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

#include "pet_p4_build_info.h"
#include "pet_p4_diagnostics.h"
#include "pet_p4_ota.h"
#include "pet_p4_raw_assets.h"
#include "pet_p4_renderer.h"
#include "pet_p4_usb_native.h"

static const char *TAG = "pet-p4-native-usb";

#define PET_P4_NATIVE_MAGIC "P4BU"
#define PET_P4_NATIVE_VERSION 1
#define PET_P4_NATIVE_HEADER_LEN 16
#define PET_P4_NATIVE_MAX_PAYLOAD (64 * 1024)
#define PET_P4_NATIVE_PATH_MAX 96
#define PET_P4_NATIVE_TRANSFER_MAX 64
#define PET_P4_NATIVE_RX_QUEUE_DEPTH 4

enum {
  PET_P4_NATIVE_KIND_JSON = 1,
  PET_P4_NATIVE_KIND_FILE_BEGIN = 2,
  PET_P4_NATIVE_KIND_FILE_DATA = 3,
  PET_P4_NATIVE_KIND_FILE_END = 4,
  PET_P4_NATIVE_KIND_COMMIT = 5,
  PET_P4_NATIVE_KIND_PING = 6,
};

static pet_p4_runtime_state_t *g_state;
static usb_phy_handle_t g_phy;
static bool g_started;
static uint32_t g_tx_seq;
static SemaphoreHandle_t g_tx_mutex;
static SemaphoreHandle_t g_state_mutex;
static TaskHandle_t g_native_worker_task;
static _Atomic uint32_t g_connection_generation;
static uint32_t g_native_transfer_connection;
static uint32_t g_native_dispatch_connection; /* immutable epoch of the currently dequeued frame */
static QueueHandle_t g_rx_queue;
static pet_p4_native_protocol_enqueue_fn g_enqueue_protocol;
static void *g_enqueue_protocol_ctx;

typedef struct {
  uint8_t kind;
  uint32_t connection;
  uint32_t seq;
  uint32_t payload_len;
  uint8_t *payload;
} pet_p4_native_rx_frame_t;

static uint8_t g_header[PET_P4_NATIVE_HEADER_LEN];
static size_t g_header_used;
static uint8_t *g_payload;
static uint32_t g_payload_len;
static uint32_t g_payload_used;
static uint8_t g_frame_kind;
static uint32_t g_frame_seq;
static uint32_t g_frame_connection;

static char g_transfer_id[PET_P4_NATIVE_TRANSFER_MAX];
static char g_cleaned_transfer_id[PET_P4_NATIVE_TRANSFER_MAX];
static char g_current_path[PET_P4_NATIVE_PATH_MAX];
static char g_expected_checksum[17];
static unsigned long long g_expected_size;
static unsigned long long g_received_size;
static unsigned long long g_transfer_last_activity_ms;
static int g_target_slot = -1;
static bool g_native_file_raw,g_native_failed,g_native_preserve_raw;
static bool g_native_write_attempted;
static _Atomic bool g_native_cleanup_due;
typedef struct {char id[64],path[96],phase[24];bool seen,ok,invalid;} native_phase_ack_t;
static native_phase_ack_t *g_native_phase_reply; /* only native worker accesses */
static uint64_t g_native_raw_limit,g_native_raw_bytes;
static uint32_t g_native_chunk_index,g_native_files;
static uint64_t g_native_total_bytes;
static _Atomic int g_mount_pending;
static _Atomic uint32_t g_observation_epoch;
static _Atomic unsigned g_pending_frames;
static _Atomic bool g_receiving;
static void observation_changed(void) {
  uint32_t old=atomic_load_explicit(&g_observation_epoch,memory_order_acquire);
  while(old!=UINT32_MAX&&!atomic_compare_exchange_weak_explicit(&g_observation_epoch,&old,old+1,memory_order_acq_rel,memory_order_acquire)){}
}
pet_p4_native_io_snapshot_t pet_p4_native_usb_io_snapshot(void) {
  pet_p4_native_io_snapshot_t s={
    .transfer_active=pet_p4_appearance_native_active(),
    .receiving=atomic_load_explicit(&g_receiving,memory_order_acquire),
    .worker_pending=atomic_load_explicit(&g_pending_frames,memory_order_acquire)!=0
      ||atomic_load_explicit(&g_mount_pending,memory_order_acquire)!=0,
    .epoch=atomic_load_explicit(&g_observation_epoch,memory_order_acquire)
  };
  if(s.epoch==UINT32_MAX)s.worker_pending=true; /* exhausted epochs fail closed */
  return s;
}



static void *native_route_context(void) {
  uint32_t c=atomic_load_explicit(&g_connection_generation,memory_order_acquire);
  return c==UINT32_MAX?NULL:(void*)(uintptr_t)(c+1U);
}
bool pet_p4_native_usb_route_current(void *route) {
  return route&&route==native_route_context();
}
static unsigned long long native_now_ms(void) {
  return (unsigned long long) (esp_timer_get_time() / 1000ULL);
}

static uint32_t le32(const uint8_t *bytes) {
  return (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

static void put_le32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t) (value & 0xff);
  bytes[1] = (uint8_t) ((value >> 8) & 0xff);
  bytes[2] = (uint8_t) ((value >> 16) & 0xff);
  bytes[3] = (uint8_t) ((value >> 24) & 0xff);
}

static unsigned long long fnv1a64_update(unsigned long long hash, const unsigned char *bytes, size_t len) {
  for (size_t i = 0; i < len; i += 1) {
    hash ^= (unsigned long long) bytes[i];
    hash *= 0x00000100000001b3ULL;
  }
  return hash;
}

static unsigned long long fnv1a64_file(FILE *file, unsigned long long *size_out) {
  unsigned char buf[512];
  unsigned long long hash = 0xcbf29ce484222325ULL;
  unsigned long long size = 0;
  size_t n;
  if (!file) return hash;
  while ((n = fread(buf, 1, sizeof(buf), file)) > 0) {
    hash = fnv1a64_update(hash, buf, n);
    size += (unsigned long long) n;
  }
  if (size_out) *size_out = size;
  return hash;
}

static const char *json_string(const cJSON *object, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

static unsigned long long json_u64(const cJSON *object, const char *key, unsigned long long fallback) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  uint32_t value;
  return cJSON_IsNumber(item)&&pet_appearance_size(item->valuedouble,&value)?value:fallback;
}

static cJSON *parse_payload_json(const uint8_t *payload, uint32_t len) {
  if (!payload || !len || !pet_json_source_bounded_text_bytes(payload,len)) return NULL;
  char *text = (char *) malloc((size_t) len + 1);
  cJSON *root;
  if (!text) return NULL;
  memcpy(text, payload, len);
  text[len] = '\0';
  root = cJSON_ParseWithOpts(text, NULL, true);
  free(text);
  return root;
}

static bool native_write_all(const uint8_t *bytes, size_t len) {
  size_t offset = 0;
  int idle_ticks = 0;
  while (offset < len && idle_ticks < 200) {
    uint32_t written = tud_vendor_write(bytes + offset, len - offset);
    /* A driver cannot accept more bytes than offered. Reject before size_t
     * addition: on the 32-bit target an invalid return could wrap offset. */
    if (written > len - offset) return false;
    if (written == 0) {
      tud_vendor_write_flush();
      vTaskDelay(pdMS_TO_TICKS(1));
      idle_ticks += 1;
    } else {
      offset += written;
      idle_ticks = 0;
    }
  }
  return offset == len;
}

static bool native_send_frame(uint8_t kind, const uint8_t *payload, uint32_t len) {
  uint8_t header[PET_P4_NATIVE_HEADER_LEN];
  bool ok = false;
  if (!g_started || !tud_mounted() || !g_tx_mutex) return false;
  if (xSemaphoreTake(g_tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  if (!tud_mounted()) goto done;
  memcpy(header, PET_P4_NATIVE_MAGIC, 4);
  header[4] = PET_P4_NATIVE_VERSION;
  header[5] = kind;
  header[6] = 0;
  header[7] = 0;
  put_le32(&header[8], g_tx_seq++);
  put_le32(&header[12], len);
  ok = native_write_all(header, sizeof(header)) && native_write_all(payload, len);
  tud_vendor_write_flush();
done:
  xSemaphoreGive(g_tx_mutex);
  return ok;
}

void pet_p4_native_usb_send_json_line(const char *line, void *ctx) {
  if ((ctx&&!pet_p4_native_usb_route_current(ctx))||!line||!line[0]) return;
  (void) native_send_frame(PET_P4_NATIVE_KIND_JSON, (const uint8_t *) line, (uint32_t) strlen(line));
}

static void send_topic_payload(const char *topic, cJSON *payload) {
  cJSON *root = cJSON_CreateObject();
  char *line;
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  cJSON_AddStringToObject(root, "topic", topic);
  cJSON_AddItemToObject(root, "payload", payload);
  line = cJSON_PrintUnformatted(root);
  if (line) pet_p4_native_usb_send_json_line(line, NULL);
  cJSON_free(line);
  cJSON_Delete(root);
}

static void send_asset_ack(const char *transfer_id, const char *phase, const char *path, bool ok, const char *error) {
  if(g_native_phase_reply){
    native_phase_ack_t *r=g_native_phase_reply;
    if(r->seen){r->invalid=true;return;}r->seen=true;r->ok=ok;
    snprintf(r->id,sizeof(r->id),"%s",transfer_id?transfer_id:"");
    snprintf(r->path,sizeof(r->path),"%s",path?path:"");
    snprintf(r->phase,sizeof(r->phase),"%s",phase?phase:"");return;
  }
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "transferId", transfer_id ? transfer_id : "");
  cJSON_AddStringToObject(payload, "phase", phase ? phase : "");
  if (path && path[0]) cJSON_AddStringToObject(payload, "path", path);
  cJSON_AddBoolToObject(payload, "ok", ok);
  if (error && error[0]) cJSON_AddStringToObject(payload, "error", error);
  send_topic_payload("asset/ack", payload);
}

static bool clear_slot_files(int slot) {
  pet_p4_renderer_set_asset_preparation_suspended(true);
  if (slot < 0 || slot > 1) return false;
  char prefix[8];
  snprintf(prefix, sizeof(prefix), "s%d_", slot);
  while (true) {
    DIR *dir = opendir("/spiffs");
    struct dirent *entry;
    char fs_path[96];
    bool found = false;
    if (!dir) return false;
    while ((entry = readdir(dir)) != NULL) {
      if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) continue;
      size_t name_size = strlen(entry->d_name);
      const char base[] = "/spiffs" "/";
      if (name_size >= sizeof(fs_path) - (sizeof(base)-1U)) { closedir(dir); return false; }
      memcpy(fs_path, base, sizeof(base)-1U);
      memcpy(fs_path+sizeof(base)-1U, entry->d_name, name_size+1U);
      found = true;
      break;
    }
    closedir(dir);
    if (!found) {
      if (slot == PET_P4_RAW_APPEARANCE_SLOT && pet_p4_raw_assets_available()&&!g_native_preserve_raw) {
        char error[96];
        if (!pet_p4_raw_assets_invalidate(error, sizeof(error))) {
          ESP_LOGE(TAG, "failed to invalidate native raw slot: %s", error);
          return false;
        }
      }
      return true;
    }
    errno = 0;
    if (remove(fs_path) != 0 && errno != ENOENT) {
      ESP_LOGE(TAG, "failed to clear appearance slot=%d path=%s error=%s",
               slot, fs_path, errno ? strerror(errno) : "unknown");
      return false;
    }
  }
}

static bool ensure_transfer_slot(const char *transfer_id) {
  if (g_target_slot < 0 || strcmp(g_cleaned_transfer_id, transfer_id ? transfer_id : "") != 0) {
    g_target_slot = PET_P4_RAW_APPEARANCE_SLOT;
    if (!clear_slot_files(g_target_slot)) return false;
    snprintf(g_cleaned_transfer_id, sizeof(g_cleaned_transfer_id), "%s", transfer_id ? transfer_id : "");
  }
  return g_target_slot >= 0;
}

/* Route/id checks precede every storage effect. A rejected request never
 * becomes an implicit new transfer and never clears another owner's slot. */
static bool native_owned(const char *id){
  return g_native_transfer_connection!=UINT32_MAX
    &&g_native_transfer_connection==atomic_load_explicit(&g_connection_generation,memory_order_acquire)
    &&pet_p4_appearance_owned(pet_p4_native_usb_send_json_line,NULL,id,PET_APPEARANCE_NATIVE);
}
static void native_retire(void){
  if(pet_p4_appearance_mutation_blocked())return;
  (void)pet_p4_appearance_end(pet_p4_native_usb_send_json_line,NULL,g_transfer_id,PET_APPEARANCE_NATIVE);
  pet_p4_raw_assets_reset_transfer();
  g_transfer_id[0]=g_cleaned_transfer_id[0]=g_current_path[0]=g_expected_checksum[0]=0;
  g_expected_size=g_received_size=g_transfer_last_activity_ms=g_native_total_bytes=0;
  g_native_chunk_index=g_native_files=0;g_target_slot=-1;g_native_failed=g_native_file_raw=g_native_preserve_raw=false;
  g_native_raw_limit=g_native_raw_bytes=0;g_native_transfer_connection=0;
  if(g_state)g_state->asset_transfer_active=false;
}
static void handle_file_begin(const uint8_t *payload,uint32_t len){
  cJSON *root=parse_payload_json(payload,len);
  const char *id=json_string(root,"transferId"),*path=json_string(root,"path"),*checksum=json_string(root,"checksum");
  uint64_t size=json_u64(root,"size",UINT64_MAX),raw_bytes=json_u64(root,"rawBytes",0);
  const cJSON *raw=cJSON_GetObjectItemCaseSensitive(root,"rawBytes");char tmp[96];
  bool valid=atomic_load_explicit(&g_connection_generation,memory_order_acquire)!=UINT32_MAX
    &&cJSON_IsObject(root)&&pet_appearance_id_valid(id)&&strlen(path)<sizeof(g_current_path)
    &&pet_p4_asset_fs_path_for_slot(PET_P4_RAW_APPEARANCE_SLOT,path,true,tmp,sizeof(tmp))
    &&size>0&&size<=UINT32_MAX&&pet_appearance_checksum(checksum)
    &&(!raw||(cJSON_IsNumber(raw)&&json_u64(root,"rawBytes",UINT64_MAX)!=UINT64_MAX))
    &&(!pet_p4_raw_assets_available()||raw_bytes<=pet_p4_raw_assets_capacity_bytes());
  bool raw_file=pet_p4_raw_assets_available()&&pet_p4_raw_assets_supports_path(path);
  if(raw_file){
    uint64_t limit=pet_p4_appearance_active()?g_native_raw_limit:(raw?raw_bytes:pet_p4_raw_assets_capacity_bytes());
    valid=valid&&size<=limit&&g_native_raw_bytes<=limit&&size<=limit-g_native_raw_bytes;
  }
  if(!valid){send_asset_ack(id,"file-begin",path,false,"invalid bounded file metadata");goto done;}
  if(pet_p4_diagnostics_reboot_pending()||pet_p4_ota_transfer_active()){
    send_asset_ack(id,"file-begin",path,false,"device firmware update or reboot is active");goto done;
  }
  bool first=!pet_p4_appearance_active();
  if((!first&&!native_owned(id))||g_current_path[0]||g_native_failed){
    send_asset_ack(id,"file-begin",path,false,"appearance owner mismatch, failed transfer or unfinished file");goto done;
  }
  g_native_file_raw=pet_p4_raw_assets_available()&&pet_p4_raw_assets_supports_path(path);
  pet_p4_renderer_set_asset_preparation_suspended(true);
  if(!g_native_file_raw){
    g_native_write_attempted=true;
    FILE *file=fopen(tmp,"wb");
    if(!file){g_native_failed=true;send_asset_ack(id,"file-begin",path,false,strerror(errno));goto done;}
    if(fclose(file)!=0){g_native_failed=true;send_asset_ack(id,"file-begin",path,false,"staged file close failed");goto done;}
  }
  snprintf(g_current_path,sizeof(g_current_path),"%s",path);
  snprintf(g_expected_checksum,sizeof(g_expected_checksum),"%s",checksum);
  g_expected_size=size;g_received_size=0;g_native_chunk_index=0;g_transfer_last_activity_ms=native_now_ms();
  send_asset_ack(id,"file-begin",path,true,NULL);
done:cJSON_Delete(root);
}

static void handle_file_data(const uint8_t *payload,uint32_t len){
  char tmp[96],error[128];
  if(!native_owned(g_transfer_id)||g_native_failed||!g_current_path[0]||!payload||!len
      ||g_received_size>g_expected_size||len>g_expected_size-g_received_size){
    send_asset_ack(g_transfer_id,"chunk",g_current_path,false,"no matching open file or invalid chunk size");return;
  }
  pet_p4_renderer_set_asset_preparation_suspended(true);
  bool ok=false;
  if(g_native_file_raw){
    char index[12];snprintf(index,sizeof(index),"%u",(unsigned)g_native_chunk_index);
    g_native_write_attempted=true;
    ok=pet_p4_raw_assets_write_chunk(g_current_path,index,payload,len,error,sizeof(error));
  }else if(pet_p4_asset_fs_path_for_slot(g_target_slot,g_current_path,true,tmp,sizeof(tmp))){
    g_native_write_attempted=true;
    FILE *file=fopen(tmp,"ab");
    if(file){ok=fwrite(payload,1,len,file)==len;if(fclose(file)!=0)ok=false;}
    if(!ok)snprintf(error,sizeof(error),"staged file write failed");
  }else snprintf(error,sizeof(error),"invalid staged file path");
  if(!ok){g_native_failed=true;send_asset_ack(g_transfer_id,"chunk",g_current_path,false,error);return;}
  g_native_chunk_index++;g_received_size+=len;g_transfer_last_activity_ms=native_now_ms();
}

static void handle_file_end(const uint8_t *payload,uint32_t len){
  cJSON *root=parse_payload_json(payload,len);
  const char *id=json_string(root,"transferId"),*path=json_string(root,"path");
  if(!cJSON_IsObject(root)||!native_owned(id)||!g_current_path[0]||strcmp(path,g_current_path)){
    send_asset_ack(id,"file",path,false,"file transaction id/path mismatch");goto done;
  }
  if(g_native_failed||g_received_size!=g_expected_size){
    send_asset_ack(id,"file",path,false,"failed/incomplete file");goto done;
  }
  pet_p4_renderer_set_asset_preparation_suspended(true);
  char error[128]={0};bool ok=false;
  if(g_native_file_raw){
    ok=pet_p4_raw_assets_finish_file(path,g_expected_size,g_expected_checksum,error,sizeof(error));
  }else{
    char tmp[96],final[96];unsigned long long size=0;char checksum[17];
    if(pet_p4_asset_fs_path_for_slot(g_target_slot,path,true,tmp,sizeof(tmp))
        &&pet_p4_asset_fs_path_for_slot(g_target_slot,path,false,final,sizeof(final))){
      FILE *file=fopen(tmp,"rb");
      if(file){uint64_t hash=fnv1a64_file(file,&size);bool closed=!ferror(file);if(fclose(file)!=0)closed=false;
        snprintf(checksum,sizeof(checksum),"%016llx",(unsigned long long)hash);
        ok=closed&&size==g_expected_size&&!strcmp(checksum,g_expected_checksum);
      }
      if(ok){g_native_write_attempted=true;ok=rename(tmp,final)==0;}
    }
    if(!ok)snprintf(error,sizeof(error),"staged file checksum/rename failed");
  }
  if(ok){if(g_native_file_raw)g_native_raw_bytes+=g_expected_size;g_native_files++;g_native_total_bytes+=g_expected_size;g_current_path[0]=0;g_expected_checksum[0]=0;g_expected_size=g_received_size=0;}
  else g_native_failed=true;
  g_transfer_last_activity_ms=native_now_ms();send_asset_ack(id,"file",path,ok,ok?NULL:error);
done:cJSON_Delete(root);
}

typedef struct {uint32_t connection;} native_commit_context_t;
static bool native_commit_current(void *context) {
  const native_commit_context_t *c=context;
  return c&&c->connection!=UINT32_MAX&&c->connection==atomic_load_explicit(&g_connection_generation,memory_order_acquire);
}
static bool native_commit_io_begin(void *context) {
  (void)context;
  if(!g_state_mutex||xTaskGetCurrentTaskHandle()!=g_native_worker_task
      ||xSemaphoreGetMutexHolder(g_state_mutex)!=g_native_worker_task)return false;
  pet_p4_renderer_set_asset_preparation_suspended(true);
  return xSemaphoreGive(g_state_mutex)==pdTRUE;
}
static void native_commit_io_end(void *context) {
  (void)context;
  BaseType_t held=xSemaphoreTake(g_state_mutex,portMAX_DELAY);
  configASSERT(held==pdTRUE);(void)held;
}
static void native_commit_ack(native_commit_context_t *context,const char *id,
    bool ok,const char *error) {
  if(!native_commit_current(context)||!native_commit_io_begin(context))return;
  if(native_commit_current(context))send_asset_ack(id,"commit",NULL,ok,error);
  native_commit_io_end(context);
}
static void handle_commit(const uint8_t *payload,uint32_t len){
  cJSON *root=parse_payload_json(payload,len);const char *id=json_string(root,"transferId");
  native_commit_context_t context={atomic_load_explicit(&g_connection_generation,memory_order_acquire)};
  if(!cJSON_IsObject(root)||!native_owned(id)||g_native_failed||g_current_path[0]
      ||pet_p4_appearance_mutation_blocked()
      ||!g_native_files||json_u64(root,"fileCount",UINT64_MAX)!=g_native_files
      ||json_u64(root,"totalBytes",UINT64_MAX)!=g_native_total_bytes){
    native_commit_ack(&context,id,false,"invalid/incomplete or quarantined appearance transaction");goto done;
  }
  pet_p4_renderer_set_asset_preparation_suspended(true);char error[128]={0};
  const pet_appearance_commit_io_t io={native_commit_io_begin,native_commit_io_end,
    native_commit_current,&context};
  pet_appearance_commit_result_t result=pet_p4_appearance_commit_slot(g_state,g_target_slot,
    pet_p4_native_usb_send_json_line,NULL,id,PET_APPEARANCE_NATIVE,false,&io,error,sizeof(error));
  if(result==PET_APPEARANCE_COMMIT_OK)native_retire();
  else g_native_failed=true;
  native_commit_ack(&context,id,result==PET_APPEARANCE_COMMIT_OK,
    result==PET_APPEARANCE_COMMIT_OK?NULL:error);
done:cJSON_Delete(root);
}

static bool handle_asset_abort_json(const uint8_t *payload,uint32_t len){
  cJSON *root=parse_payload_json(payload,len);
  if(!cJSON_IsObject(root)||strcmp(json_string(root,"topic"),"asset/abort")){cJSON_Delete(root);return false;}
  const cJSON *body=cJSON_GetObjectItemCaseSensitive(root,"payload");const char *id=json_string(body,"transferId");
  if(pet_p4_appearance_owned(pet_p4_native_usb_send_json_line,native_route_context(),id,PET_APPEARANCE_SERIAL)){cJSON_Delete(root);return false;}
  native_commit_context_t c={atomic_load_explicit(&g_connection_generation,memory_order_acquire)};
  bool ok=false;pet_appearance_commit_result_t result=PET_APPEARANCE_COMMIT_FAILED;
  if(!pet_p4_appearance_mutation_blocked()&&native_owned(id)){
    pet_appearance_phase_t op={0};int slot=g_target_slot;
    const pet_appearance_commit_io_t io={native_commit_io_begin,native_commit_io_end,native_commit_current,&c};
    if(pet_p4_appearance_phase_begin(&op,g_state,pet_p4_native_usb_send_json_line,NULL,id,PET_APPEARANCE_NATIVE,slot,&io)){
      ok=slot<0||clear_slot_files(slot);
      result=pet_p4_appearance_phase_end(&op,g_state,slot,ok,slot>=0);
      if(result==PET_APPEARANCE_COMMIT_OK)native_retire();else g_native_failed=true;
    }
  }
  if(native_commit_current(&c)&&native_commit_io_begin(&c)){
    if(native_commit_current(&c))send_asset_ack(id,"abort",NULL,result==PET_APPEARANCE_COMMIT_OK,
      result==PET_APPEARANCE_COMMIT_OK?NULL:"appearance cleanup busy or outcome unknown; no replay");
    cJSON_Delete(root);native_commit_io_end(&c);
  }else cJSON_Delete(root);
  return true;
}

void pet_p4_native_usb_process(unsigned long long now_ms) {
  /* Main-loop call: bounded RAM observation only, never filesystem cleanup. */
  if(!pet_p4_appearance_mutation_blocked()&&native_owned(g_transfer_id)&&g_state
      &&g_state->asset_transfer_active&&g_target_slot>=0&&g_transfer_last_activity_ms
      &&now_ms>=g_transfer_last_activity_ms
      &&now_ms-g_transfer_last_activity_ms>=PET_P4_ASSET_TRANSFER_IDLE_TIMEOUT_MS)
    atomic_store_explicit(&g_native_cleanup_due,true,memory_order_release);
}

static void handle_json_frame(const uint8_t *payload, uint32_t len) {
  char *line;
  if (!payload || !len || !pet_json_source_bounded_text_bytes(payload,len)) return;
  if (handle_asset_abort_json(payload, len)) return;
  line = (char *) malloc((size_t) len + 1);
  if (!line) return;
  memcpy(line, payload, len);
  line[len] = '\0';
  if (g_state) {
    if (g_enqueue_protocol) {
      (void) g_enqueue_protocol(
        line,
        pet_p4_native_usb_send_json_line,
        (void*)(uintptr_t)(g_native_dispatch_connection+1U),
        g_enqueue_protocol_ctx
      );
    } else {
      pet_p4_handle_line(g_state, line, pet_p4_native_usb_send_json_line, (void*)(uintptr_t)(g_native_dispatch_connection+1U));
    }
  }
  free(line);
}

static void handle_identity_ping(const uint8_t *payload, uint32_t len) {
  cJSON *request = parse_payload_json(payload, len);
  const char *nonce;
  cJSON *response;
  if (!cJSON_IsObject(request)) {
    ESP_LOGW(TAG, "ignoring native identity ping with invalid JSON");
    cJSON_Delete(request);
    return;
  }
  nonce = json_string(request, "nonce");
  if (!nonce[0] || !g_state || !g_state->board_device_id[0] ||
      strcmp(g_state->board_device_id, "p4-unknown") == 0) {
    ESP_LOGW(TAG, "ignoring native identity ping without nonce or board identity");
    cJSON_Delete(request);
    return;
  }
  response = cJSON_CreateObject();
  if (!response) {
    cJSON_Delete(request);
    return;
  }
  cJSON_AddStringToObject(response, "protocol", "pet-usb-native-v1");
  cJSON_AddStringToObject(response, "boardDeviceId", g_state->board_device_id);
  cJSON_AddStringToObject(response, "nonce", nonce);
  cJSON_AddNumberToObject(response, "protocolSchema", PET_P4_PROTOCOL_SCHEMA);
  cJSON_AddStringToObject(response, "buildId", PET_P4_BUILD_ID);
  send_topic_payload("native/pong", response);
  cJSON_Delete(request);
}

static void native_failure_locked(const char *id,const char *phase,const char *path,const char *error){
  native_commit_context_t c={atomic_load_explicit(&g_connection_generation,memory_order_acquire)};
  if(native_commit_current(&c)&&native_commit_io_begin(&c)){
    if(native_commit_current(&c))send_asset_ack(id,phase,path,false,error);
    native_commit_io_end(&c);
  }
}
static void native_idle_cleanup_worker(void){
  if(!atomic_exchange_explicit(&g_native_cleanup_due,false,memory_order_acq_rel))return;
  unsigned long long now=native_now_ms();
  if(pet_p4_appearance_mutation_blocked()||!native_owned(g_transfer_id)
      ||!g_transfer_last_activity_ms||now<g_transfer_last_activity_ms
      ||now-g_transfer_last_activity_ms<PET_P4_ASSET_TRANSFER_IDLE_TIMEOUT_MS)return;
  native_commit_context_t c={g_native_transfer_connection};
  const pet_appearance_commit_io_t io={native_commit_io_begin,native_commit_io_end,native_commit_current,&c};
  pet_appearance_phase_t op={0};int slot=g_target_slot;
  if(!pet_p4_appearance_phase_begin(&op,g_state,pet_p4_native_usb_send_json_line,NULL,g_transfer_id,PET_APPEARANCE_NATIVE,slot,&io))return;
  bool ok=clear_slot_files(slot);
  pet_appearance_commit_result_t result=pet_p4_appearance_phase_end(&op,g_state,slot,ok,true);
  if(result==PET_APPEARANCE_COMMIT_OK)native_retire();else g_native_failed=true;
}
static void handle_complete_frame(uint8_t kind,const uint8_t *payload,uint32_t len){
  if(g_native_dispatch_connection==UINT32_MAX||g_native_dispatch_connection!=atomic_load_explicit(&g_connection_generation,memory_order_acquire))return;
  if(kind==PET_P4_NATIVE_KIND_JSON){handle_json_frame(payload,len);return;}
  if(kind==PET_P4_NATIVE_KIND_COMMIT){handle_commit(payload,len);return;}
  if(kind==PET_P4_NATIVE_KIND_PING){
    if(native_commit_io_begin(NULL)){handle_identity_ping(payload,len);native_commit_io_end(NULL);}return;
  }
  if(kind!=PET_P4_NATIVE_KIND_FILE_BEGIN&&kind!=PET_P4_NATIVE_KIND_FILE_DATA&&kind!=PET_P4_NATIVE_KIND_FILE_END)return;
  bool first=false;uint64_t raw_size=0;
  if(pet_p4_appearance_mutation_blocked()){
    native_failure_locked("",kind==PET_P4_NATIVE_KIND_FILE_BEGIN?"file-begin":kind==PET_P4_NATIVE_KIND_FILE_END?"file":"chunk",NULL,"appearance IO active or result unknown; no replay");return;
  }
  if(kind==PET_P4_NATIVE_KIND_FILE_BEGIN){
    cJSON *root=parse_payload_json(payload,len);
    const char *id=json_string(root,"transferId"),*path=json_string(root,"path"),*checksum=json_string(root,"checksum");
    const cJSON *raw=cJSON_GetObjectItemCaseSensitive(root,"rawBytes");char tmp[96];
    uint64_t size=json_u64(root,"size",UINT64_MAX);raw_size=raw?json_u64(root,"rawBytes",UINT64_MAX):0;
    bool valid=cJSON_IsObject(root)&&pet_appearance_id_valid(id)&&strlen(path)<sizeof(g_current_path)
      &&pet_p4_asset_fs_path_for_slot(PET_P4_RAW_APPEARANCE_SLOT,path,true,tmp,sizeof(tmp))
      &&size>0&&size<=UINT32_MAX&&pet_appearance_checksum(checksum)
      &&(!raw||(cJSON_IsNumber(raw)&&raw_size!=UINT64_MAX))
      &&atomic_load_explicit(&g_connection_generation,memory_order_acquire)!=UINT32_MAX;
    first=!pet_p4_appearance_active();
    if(pet_p4_raw_assets_supports_path(path)){
      uint64_t limit=first?(raw?raw_size:UINT32_MAX):g_native_raw_limit;
      valid=valid&&size<=limit&&g_native_raw_bytes<=limit&&size<=limit-g_native_raw_bytes;
    }
    if(!valid||(!first&&!native_owned(id))||g_current_path[0]||g_native_failed
        ||pet_p4_diagnostics_reboot_pending()||pet_p4_ota_transfer_active()){
      native_failure_locked(id,"file-begin",path,"invalid, busy or unfinished appearance transaction");cJSON_Delete(root);return;
    }
    if(first){
      if(!pet_p4_appearance_claim(pet_p4_native_usb_send_json_line,NULL,id,PET_APPEARANCE_NATIVE)){
        native_failure_locked(id,"file-begin",path,"appearance owner unavailable");cJSON_Delete(root);return;
      }
      g_native_transfer_connection=g_native_dispatch_connection;
      g_native_preserve_raw=raw&&raw_size==0;g_native_raw_limit=raw?raw_size:UINT32_MAX;
      snprintf(g_transfer_id,sizeof(g_transfer_id),"%s",id);g_target_slot=PET_P4_RAW_APPEARANCE_SLOT;
      g_state->asset_transfer_active=true;g_transfer_last_activity_ms=native_now_ms();
    }
    cJSON_Delete(root);
  }else if(!native_owned(g_transfer_id)){
    native_failure_locked("",kind==PET_P4_NATIVE_KIND_FILE_END?"file":"chunk",NULL,"no matching appearance owner");return;
  }
  native_commit_context_t c={g_native_transfer_connection};
  const pet_appearance_commit_io_t io={native_commit_io_begin,native_commit_io_end,native_commit_current,&c};
  pet_appearance_phase_t op={0};int slot=g_target_slot;
  if(!pet_p4_appearance_phase_begin(&op,g_state,pet_p4_native_usb_send_json_line,NULL,g_transfer_id,PET_APPEARANCE_NATIVE,slot,&io)){
    native_failure_locked(g_transfer_id,kind==PET_P4_NATIVE_KIND_FILE_BEGIN?"file-begin":kind==PET_P4_NATIVE_KIND_FILE_END?"file":"chunk",g_current_path,"appearance storage unavailable");return;
  }
  native_phase_ack_t reply={0};g_native_phase_reply=&reply;g_native_write_attempted=false;
  bool prepared=true;
  if(first){
    bool available=pet_p4_raw_assets_available();
    if(available&&g_native_raw_limit==UINT32_MAX)g_native_raw_limit=pet_p4_raw_assets_capacity_bytes();
    prepared=!available||raw_size<=pet_p4_raw_assets_capacity_bytes();
    if(prepared){g_native_write_attempted=true;prepared=ensure_transfer_slot(g_transfer_id);}
    if(prepared&&available&&!g_native_preserve_raw){char error[128];prepared=pet_p4_raw_assets_prepare(raw_size,error,sizeof(error));}
    if(!prepared){g_native_failed=true;send_asset_ack(g_transfer_id,"file-begin",NULL,false,"appearance prepare failed");}
  }
  if(prepared){
    if(kind==PET_P4_NATIVE_KIND_FILE_BEGIN)handle_file_begin(payload,len);
    else if(kind==PET_P4_NATIVE_KIND_FILE_DATA)handle_file_data(payload,len);
    else handle_file_end(payload,len);
  }
  bool ok=!g_native_failed&&(!reply.seen||reply.ok)&&!reply.invalid;
  g_native_phase_reply=NULL;
  pet_appearance_commit_result_t result=pet_p4_appearance_phase_end(&op,g_state,slot,ok,g_native_write_attempted);
  if(result!=PET_APPEARANCE_COMMIT_OK&&(g_native_write_attempted||result==PET_APPEARANCE_COMMIT_UNKNOWN))g_native_failed=true;
  if(first&&result==PET_APPEARANCE_COMMIT_FAILED&&!g_native_write_attempted)native_retire();
  if(reply.seen||result!=PET_APPEARANCE_COMMIT_OK){
    if(native_commit_current(&c)&&native_commit_io_begin(&c)){
      if(native_commit_current(&c))send_asset_ack(reply.seen?reply.id:g_transfer_id,
        reply.seen?reply.phase:(kind==PET_P4_NATIVE_KIND_FILE_BEGIN?"file-begin":kind==PET_P4_NATIVE_KIND_FILE_END?"file":"chunk"),
        reply.seen?reply.path:NULL,result==PET_APPEARANCE_COMMIT_OK,
        result==PET_APPEARANCE_COMMIT_OK?NULL:result==PET_APPEARANCE_COMMIT_UNKNOWN?"appearance phase outcome unknown; do not replay":"appearance phase rejected");
      native_commit_io_end(&c);
    }
  }
}


static bool enqueue_complete_frame(void) {
  pet_p4_native_rx_frame_t frame = {
    .kind = g_frame_kind,
    .connection=g_frame_connection,
    .seq = g_frame_seq,
    .payload_len = g_payload_len,
    .payload = g_payload,
  };
  atomic_fetch_add_explicit(&g_pending_frames,1,memory_order_acq_rel);
  observation_changed();
  if (!g_rx_queue || xQueueSend(g_rx_queue, &frame, 0) != pdTRUE) {
    atomic_fetch_sub_explicit(&g_pending_frames,1,memory_order_acq_rel);
    observation_changed();
    ESP_LOGW(TAG, "dropping native frame because protocol queue is full seq=%u",
             (unsigned int) g_frame_seq);
    return false;
  }
  g_payload = NULL;
  return true;
}

static void reset_rx_frame(void) {
  free(g_payload);
  g_payload = NULL;
  g_payload_len = 0;
  g_payload_used = 0;
  g_header_used = 0;
  g_frame_kind = 0;
  g_frame_seq = 0;g_frame_connection=UINT32_MAX;
}

static bool start_payload_from_header(void) {
  if (memcmp(g_header, PET_P4_NATIVE_MAGIC, 4) != 0 || g_header[4] != PET_P4_NATIVE_VERSION) {
    reset_rx_frame();
    return false;
  }
  g_frame_kind = g_header[5];
  g_frame_seq = le32(&g_header[8]);
  g_payload_len = le32(&g_header[12]);
  if (g_frame_kind<PET_P4_NATIVE_KIND_JSON||g_frame_kind>PET_P4_NATIVE_KIND_PING||g_payload_len > PET_P4_NATIVE_MAX_PAYLOAD) {
    ESP_LOGW(TAG, "dropping oversized native frame seq=%u len=%u", (unsigned int) g_frame_seq, (unsigned int) g_payload_len);
    reset_rx_frame();
    return false;
  }
  if (g_payload_len == 0) {
    (void) enqueue_complete_frame();
    reset_rx_frame();
    return false;
  }
  g_payload = (uint8_t *) malloc(g_payload_len);
  if (!g_payload) {
    reset_rx_frame();
    return false;
  }
  g_payload_used = 0;
  return true;
}

static void feed_rx_bytes(const uint8_t *data, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    uint32_t connection=atomic_load_explicit(&g_connection_generation,memory_order_acquire);
    if(g_header_used&&g_frame_connection!=connection)reset_rx_frame();
    if(!g_header_used)g_frame_connection=connection;
    if (g_header_used < PET_P4_NATIVE_HEADER_LEN) {
      size_t n = PET_P4_NATIVE_HEADER_LEN - g_header_used;
      if (n > len - offset) n = len - offset;
      memcpy(g_header + g_header_used, data + offset, n);
      g_header_used += n;
      offset += n;
      if (g_header_used == PET_P4_NATIVE_HEADER_LEN) {
        (void) start_payload_from_header();
      }
      continue;
    }
    if (!g_payload) {
      reset_rx_frame();
      continue;
    }
    size_t n = g_payload_len - g_payload_used;
    if (n > len - offset) n = len - offset;
    memcpy(g_payload + g_payload_used, data + offset, n);
    g_payload_used += (uint32_t) n;
    offset += n;
    if (g_payload_used == g_payload_len) {
      (void) enqueue_complete_frame();
      reset_rx_frame();
    }
  }
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize) {
  (void) buffer;
  (void) bufsize;
  static uint8_t rx_buf[512];
  atomic_store_explicit(&g_receiving,true,memory_order_release);
  observation_changed();
  while (tud_vendor_n_available(itf)) {
    int n = tud_vendor_n_read(itf, rx_buf, sizeof(rx_buf));
    if (n > 0) feed_rx_bytes(rx_buf, (size_t) n);
  }
  atomic_store_explicit(&g_receiving,g_header_used!=0||g_payload!=NULL,memory_order_release);
  observation_changed();
}

static void tusb_device_task(void *arg) {
  (void) arg;
  while (true) {
    tud_task();
  }
}

static void native_mount_poll(void){
  int mounted=atomic_exchange_explicit(&g_mount_pending,0,memory_order_acq_rel);
  if(!mounted||!g_state)return;
  if(g_state_mutex)xSemaphoreTake(g_state_mutex,portMAX_DELAY);
  pet_p4_state_set_native_usb_mounted(g_state,mounted>0,native_now_ms());
  if(g_state_mutex)xSemaphoreGive(g_state_mutex);
  if(mounted>0)pet_p4_send_hello_for_board(g_state->board_device_id,pet_p4_native_usb_send_json_line,NULL);
}

static void native_protocol_worker_task(void *arg) {
  (void) arg;
  pet_p4_native_rx_frame_t frame;
  while (true) {
    if(g_state_mutex)xSemaphoreTake(g_state_mutex,portMAX_DELAY);
    native_idle_cleanup_worker();
    if(g_state_mutex)xSemaphoreGive(g_state_mutex);
    if (xQueueReceive(g_rx_queue, &frame, pdMS_TO_TICKS(1000)) != pdTRUE){native_mount_poll();continue;}
    native_mount_poll();
    if(!frame.kind)continue;
    if(frame.connection!=atomic_load_explicit(&g_connection_generation,memory_order_acquire)){
      atomic_fetch_sub_explicit(&g_pending_frames,1,memory_order_acq_rel);free(frame.payload);continue;
    }
    if (g_state_mutex) xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_native_dispatch_connection=frame.connection;
    handle_complete_frame(frame.kind, frame.payload, frame.payload_len);
    atomic_fetch_sub_explicit(&g_pending_frames,1,memory_order_acq_rel);
    observation_changed();
    if (g_state_mutex) xSemaphoreGive(g_state_mutex);
    free(frame.payload);
  }
}

esp_err_t pet_p4_native_usb_init(
  pet_p4_runtime_state_t *state,
  SemaphoreHandle_t state_mutex,
  pet_p4_native_protocol_enqueue_fn enqueue_protocol,
  void *enqueue_ctx
) {
  if (g_started) return ESP_OK;
  g_state = state;
  g_state_mutex = state_mutex;
  g_enqueue_protocol = enqueue_protocol;
  g_enqueue_protocol_ctx = enqueue_ctx;
  if (!g_tx_mutex) g_tx_mutex = xSemaphoreCreateMutex();
  if (!g_tx_mutex) return ESP_ERR_NO_MEM;
  usb_phy_config_t phy_conf = {
    .controller = USB_PHY_CTRL_OTG,
    .target = USB_PHY_TARGET_INT,
    .otg_mode = USB_OTG_MODE_DEVICE,
  };
  esp_err_t err = usb_new_phy(&phy_conf, &g_phy);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "USB PHY init failed: %s", esp_err_to_name(err));
    return err;
  }
  if (!tusb_init()) {
    ESP_LOGW(TAG, "TinyUSB init failed");
    return ESP_FAIL;
  }
  g_rx_queue = xQueueCreate(PET_P4_NATIVE_RX_QUEUE_DEPTH, sizeof(pet_p4_native_rx_frame_t));
  if (!g_rx_queue) return ESP_ERR_NO_MEM;
  if (xTaskCreate(
        native_protocol_worker_task,
        "pet_p4_usb_exec",
        8192,
        NULL,
        9,
        &g_native_worker_task
      ) != pdPASS) {
    vQueueDelete(g_rx_queue);
    g_rx_queue = NULL;
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreate(tusb_device_task, "pet_p4_tusb", 4096, NULL, 8, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  g_started = true;
  ESP_LOGI(TAG, "P4 native USB vendor bulk ready VID=0x303A PID=0x4040 protocol=pet-usb-native-v1");
  return ESP_OK;
}

static void native_mount_changed(int state){
  uint32_t generation=atomic_load_explicit(&g_connection_generation,memory_order_acquire);
  while(generation!=UINT32_MAX&&!atomic_compare_exchange_weak_explicit(&g_connection_generation,
      &generation,generation+1,memory_order_acq_rel,memory_order_acquire)){}

  atomic_store_explicit(&g_mount_pending,state,memory_order_release);
  observation_changed();
  pet_p4_native_rx_frame_t wake={0};
  if(g_rx_queue)(void)xQueueSend(g_rx_queue,&wake,0);
}
void tud_mount_cb(void){native_mount_changed(1);}
void tud_umount_cb(void){native_mount_changed(-1);}

void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
}

void tud_resume_cb(void) {
}
