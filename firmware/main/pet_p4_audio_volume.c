#include "pet_p4_audio_volume.h"
#include "pet_p4_audio_volume_deadline.h"
#include "pet_p4_resources_device.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_timer.h"
#include "nvs.h"

#define MASTER_NAMESPACE "p4_audio"
#define MASTER_KEY "master_v1"
#define MASTER_DEBOUNCE_MS UINT64_C(1000)
#define MASTER_WRITE_SPACING_MS UINT64_C(10000)
#define MASTER_RETRY_MS UINT64_C(30000)

static atomic_bool ready;
static esp_codec_dev_handle_t output;
static SemaphoreHandle_t stream_mutex;
static i2c_master_dev_handle_t readback;
static uint8_t level_regs[101];
/* Init owns this until ready release; then the single protocol worker owns it. */
static unsigned boot_volume = 100, durable_volume, dirty_volume;
static bool boot_loaded, durable_known, dirty, attempted;
static uint64_t dirty_since, last_attempt, retry_at;
static esp_err_t save_error;
static int last_success = -1;

unsigned pet_audio_volume_boot_load(void) {
  if (boot_loaded) return boot_volume;
  boot_loaded = true;
  nvs_handle_t handle;
  esp_err_t err = nvs_open(MASTER_NAMESPACE, NVS_READONLY, &handle);
  uint8_t stored = 100;
  if (err == ESP_OK) {
    err = nvs_get_u8(handle, MASTER_KEY, &stored);
    nvs_close(handle);
  }
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    durable_known = true; /* Absent key deterministically boots at 100. */
    durable_volume = 100;
  } else if (err == ESP_OK && stored <= 100) {
    boot_volume = durable_volume = stored;
    durable_known = true;
  } else {
    save_error = err == ESP_OK ? ESP_ERR_INVALID_ARG : err;
  }
  return boot_volume;
}

bool pet_audio_volume_ready(void) {
  return atomic_load_explicit(&ready, memory_order_acquire);
}
bool pet_audio_volume_pending(void) {
  /* Single protocol-worker caller, never an audio/render-loop callback. */
  return pet_audio_volume_ready() && dirty;
}
static void unknown(pet_audio_volume_status_t *out, esp_err_t err) {
  *out = (pet_audio_volume_status_t){.volume=-1,.error=err};
  if (pet_audio_volume_ready()) {out->save_pending=dirty;out->save_error=save_error;}
}
static esp_err_t read_register(uint8_t reg, uint8_t *value) {
  int ms = pet_audio_volume_i2c_timeout_ms(100);
  if (ms <= 0) return ESP_ERR_TIMEOUT;
  return i2c_master_transmit_receive(readback, &reg, 1, value, 1, ms);
}
/* Real DAC readback, not esp_codec_dev_get_out_vol/get_out_mute caches. */
static esp_err_t observe(int *volume) {
  uint8_t mute, level;
  esp_err_t err = read_register(0x31, &mute);
  if (err != ESP_OK) return err;
  err = read_register(0x32, &level);
  if (err != ESP_OK) return err;
  if ((mute & 0x60) == 0x60) {*volume=0;return ESP_OK;}
  if (mute & 0x60) return ESP_ERR_INVALID_STATE;
  for (unsigned v=1;v<=100;v++) if (level_regs[v]==level) {*volume=(int)v;return ESP_OK;}
  return ESP_ERR_INVALID_STATE;
}
static esp_err_t enter(void) {
  TickType_t ticks = pdMS_TO_TICKS(5);
  if (!ticks) ticks=1;
  if (xSemaphoreTakeRecursive(stream_mutex,ticks)!=pdTRUE) return ESP_ERR_TIMEOUT;
  if (!pet_audio_volume_deadline_begin(20000)) {
    xSemaphoreGiveRecursive(stream_mutex);return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}
static void leave(void) {
  pet_audio_volume_deadline_end();
  xSemaphoreGiveRecursive(stream_mutex);
}
static void snapshot(int actual, pet_audio_volume_status_t *out) {
  bool persisted=durable_known && durable_volume==(unsigned)actual && !dirty;
  *out=(pet_audio_volume_status_t){.volume=actual,.confirmed=true,
    .persisted=persisted,.save_pending=dirty,.save_error=persisted ? ESP_OK : save_error};
}
static esp_err_t apply(unsigned volume) {
  esp_err_t err;
  if (!volume) {
    err=esp_codec_dev_set_out_mute(output,true);
    if (err==ESP_OK) err=esp_codec_dev_set_out_vol(output,0);
  } else {
    err=esp_codec_dev_set_out_vol(output,(int)volume);
    if (err==ESP_OK) err=esp_codec_dev_set_out_mute(output,false);
  }
  if (err==ESP_OK) {
    int actual=-1;
    err=observe(&actual);
    if (err==ESP_OK && actual!=(int)volume) err=ESP_ERR_INVALID_STATE;
  }
  return err;
}
esp_err_t pet_audio_volume_attach(esp_codec_dev_handle_t speaker,
    SemaphoreHandle_t lock, i2c_master_bus_handle_t bus) {
  if (pet_audio_volume_ready() || !speaker || !lock || !bus || !boot_loaded
      || pet_audio_volume_codec_patch_v1!=1) return ESP_ERR_INVALID_STATE;
  output=speaker;stream_mutex=lock;
  i2c_device_config_t cfg={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
    .device_address=0x18,.scl_speed_hz=100000};
  esp_err_t err=i2c_master_bus_add_device(bus,&cfg,&readback);
  if (err!=ESP_OK) return err;
  /* Same unchanged ES8311 DAC range/default curve/BSP gain in both BSP paths.
   * Use the SDK's real conversion functions so quantization is identical. */
  esp_codec_dev_hw_gain_t gain={.pa_voltage=5.0,.codec_dac_voltage=3.3};
  float hw_gain=esp_codec_dev_col_calc_hw_gain(&gain);
  const esp_codec_dev_vol_range_t range={.min_vol={.vol=0,.db_value=-95.5},
    .max_vol={.vol=255,.db_value=32.0}};
  for (unsigned v=0;v<=100;v++) {
    float db=v ? -50.0f+0.5f*v : -96.0f;
    level_regs[v]=(uint8_t)esp_codec_dev_vol_calc_reg(&range,db-hw_gain);
  }
  err=enter();
  if (err==ESP_OK) {err=apply(boot_volume);leave();}
  if (err!=ESP_OK) {
    i2c_master_bus_rm_device(readback);readback=NULL;output=NULL;stream_mutex=NULL;
    return err;
  }
  last_success=(int)boot_volume;
  atomic_store_explicit(&ready,true,memory_order_release);
  return ESP_OK;
}
esp_err_t pet_audio_volume_get(pet_audio_volume_status_t *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  unknown(out,ESP_ERR_INVALID_STATE);
  if (!pet_audio_volume_ready()) return out->error;
  esp_err_t err=enter();
  int actual=-1;
  if (err==ESP_OK) {err=observe(&actual);leave();}
  if (err!=ESP_OK) {unknown(out,err);return err;}
  /* A reopen/failed command may have changed hardware. Never save an old dirty
   * target over observed current state. Query itself never queues a repair. */
  if (dirty && dirty_volume!=(unsigned)actual) dirty=false;
  snapshot(actual,out);
  return ESP_OK;
}
esp_err_t pet_audio_volume_set(unsigned volume, uint64_t now_ms, pet_audio_volume_status_t *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  unknown(out,ESP_ERR_INVALID_ARG);
  if (volume>100) return out->error;
  if (!pet_audio_volume_ready()) {unknown(out,ESP_ERR_INVALID_STATE);return out->error;}
  esp_err_t err=enter();
  if (err!=ESP_OK) {unknown(out,err);return err;}
  int actual=-1;
  err=observe(&actual);
  if (err==ESP_OK && (actual!=(int)volume || last_success!=(int)volume)) err=apply(volume);
  leave();
  if (err!=ESP_OK) {
    /* No retry/compensation and no stale persistence after a partial apply. */
    dirty=false;last_success=-1;unknown(out,err);return err;
  }
  last_success=(int)volume;
  if (durable_known && durable_volume==volume) dirty=false;
  else if (!dirty || dirty_volume!=volume) {
    dirty=true;dirty_volume=volume;dirty_since=now_ms;
  }
  snapshot((int)volume,out);
  return ESP_OK;
}
static bool io_current(pet_resource_lease_t lease, bool (*still_idle)(void)) {
  return pet_resources_device_current(lease) && still_idle && still_idle();
}
void pet_audio_volume_idle_commit(uint64_t now_ms, bool idle, bool (*still_idle)(void)) {
  if (!pet_audio_volume_pending() || !idle || !still_idle || !still_idle()
      || now_ms<dirty_since || now_ms-dirty_since<MASTER_DEBOUNCE_MS
      || now_ms<retry_at || (attempted && (now_ms<last_attempt
        || now_ms-last_attempt<MASTER_WRITE_SPACING_MS))) return;
  pet_resource_usage_t usage;
  pet_resources_device_usage(&usage);
  if (usage.realtime_reservations) return;
  const uint32_t conflicts=PET_RESOURCE_DEBUG_MASK | PET_RESOURCE_MASK(PET_RESOURCE_ASSET_PREPARE)
    | PET_RESOURCE_MASK(PET_RESOURCE_CACHED_VIDEO)
    | PET_RESOURCE_MASK(PET_RESOURCE_NATIVE_PROBE) | PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM)
    | PET_RESOURCE_MASK(PET_RESOURCE_PERIPHERAL) | PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK);
  for (unsigned k=0;k<PET_RESOURCE_KIND_COUNT;k++)
    if ((conflicts & PET_RESOURCE_MASK(k)) && usage.leases[k]) return;
  pet_resource_owner_t owner={0};pet_resource_lease_t lease={0};
  if (pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&owner)!=PET_RESOURCE_OK) return;
  if (pet_resources_device_acquire(owner,PET_RESOURCE_STORAGE_WORK,1,&lease)!=PET_RESOURCE_OK) {
    pet_resources_device_revoke(owner);return;
  }
  nvs_handle_t handle=0;bool opened=false,wrote=false;
  esp_err_t err=ESP_OK;
  unsigned target=dirty_volume;
  pet_audio_volume_status_t state;
  if (!io_current(lease,still_idle) || pet_audio_volume_get(&state)!=ESP_OK
      || !dirty || state.volume!=(int)target || !io_current(lease,still_idle)) goto done;
  attempted=true;last_attempt=now_ms;
  err=nvs_open(MASTER_NAMESPACE,NVS_READWRITE,&handle);
  if (err!=ESP_OK) goto completed;
  opened=true;
  if (!io_current(lease,still_idle)) goto done;
  /* set_u8 may itself write flash; retain exact lease through close. */
  wrote=true;
  err=nvs_set_u8(handle,MASTER_KEY,(uint8_t)target);
  if (err==ESP_OK) err=io_current(lease,still_idle) ? nvs_commit(handle) : ESP_ERR_INVALID_STATE;
completed:
  if (opened) {nvs_close(handle);opened=false;}
  if (err==ESP_OK) {
    durable_volume=target;durable_known=true;save_error=ESP_OK;retry_at=0;
    if (dirty && dirty_volume==target) dirty=false;
  } else {
    if (wrote) durable_known=false; /* Failure is not evidence of rollback. */
    save_error=err;
    retry_at=now_ms>UINT64_MAX-MASTER_RETRY_MS ? UINT64_MAX : now_ms+MASTER_RETRY_MS;
  }
done:
  if (opened) nvs_close(handle);
  pet_resources_device_finish(lease);
  pet_resources_device_revoke(owner);
}

static const char *bounded_string(const cJSON *item, size_t max) {
  return cJSON_IsString(item) && item->valuestring && item->valuestring[0]
    && strlen(item->valuestring)<max ? item->valuestring : NULL;
}
static const char *request(const cJSON *root, const char *board, bool *set, unsigned *volume,
    const char **id) {
  const cJSON *payload=cJSON_GetObjectItemCaseSensitive(root,"payload");
  const cJSON *topic=cJSON_GetObjectItemCaseSensitive(root,"topic");
  unsigned seen=0;bool fields_ok=true;const cJSON *field;
  *id="";
  if (!cJSON_IsObject(root) || cJSON_GetArraySize(root)!=2 || !cJSON_IsObject(payload)
      || !cJSON_IsString(topic) || strcmp(topic->valuestring,"audio/volume"))
    return "invalid_volume_request";
  cJSON_ArrayForEach(field,root)
    if (!field->string || (strcmp(field->string,"topic") && strcmp(field->string,"payload")))
      return "invalid_volume_request";
  const char *names[]={"operation","requestId","expectedBoardDeviceId","volume"};
  cJSON_ArrayForEach(field,payload) {
    unsigned k=0;
    while (k<4 && (!field->string || strcmp(field->string,names[k]))) k++;
    if (k==4 || (seen & (1U<<k))) fields_ok=false;
    if (k<4) seen|=1U<<k;
  }
  const char *rid=bounded_string(cJSON_GetObjectItemCaseSensitive(payload,"requestId"),96);
  unsigned ids=0;
  cJSON_ArrayForEach(field,payload) if (field->string && !strcmp(field->string,"requestId")) ids++;
  if (rid && ids==1) *id=rid;
  const char *op=bounded_string(cJSON_GetObjectItemCaseSensitive(payload,"operation"),6);
  const char *expected=bounded_string(cJSON_GetObjectItemCaseSensitive(payload,"expectedBoardDeviceId"),64);
  if (!fields_ok || !op || !rid || !expected || (strcmp(op,"query") && strcmp(op,"set"))) return "invalid_volume_request";
  *set=!strcmp(op,"set");
  if (seen!=(*set ? 15U : 7U)) return "invalid_volume_request";
  if (strcmp(expected,board)) return "board_mismatch";
  if (*set) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(payload,"volume");
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<0
        || v->valuedouble>100 || v->valuedouble!=(double)(unsigned)v->valuedouble)
      return "invalid_volume_request";
    *volume=(unsigned)v->valuedouble;
  }
  return NULL;
}
char *pet_audio_volume_reply(const cJSON *root, const char *board, bool available, bool *ok) {
  if (!board || !ok) return NULL;
  bool set=false;unsigned volume=0;const char *id="";
  const char *error=request(root,board,&set,&volume,&id);
  *ok=false;
  /* Allocate all required fields before any apply. No retained request state. */
  cJSON *reply=cJSON_CreateObject(),*payload=cJSON_CreateObject();
  if (!reply || !payload) {cJSON_Delete(reply);cJSON_Delete(payload);return NULL;}
  if (!cJSON_AddItemToObject(reply,"payload",payload)) {
    cJSON_Delete(reply);cJSON_Delete(payload);return NULL;
  }
  cJSON *jok,*jvol,*jpersist,*jpending,*jconfirmed;
  if (!cJSON_AddStringToObject(reply,"topic","audio/volume-state")
      || !cJSON_AddStringToObject(payload,"engine",PET_AUDIO_MASTER_VOLUME_ENGINE)
      || !cJSON_AddStringToObject(payload,"boardDeviceId",board)
      || !cJSON_AddStringToObject(payload,"requestId",id)
      || !(jok=cJSON_AddBoolToObject(payload,"ok",false))
      || !(jvol=cJSON_AddNumberToObject(payload,"volume",0))
      || !(jpersist=cJSON_AddBoolToObject(payload,"persisted",false))
      || !(jpending=cJSON_AddBoolToObject(payload,"savePending",false))
      || !(jconfirmed=cJSON_AddBoolToObject(payload,"confirmed",false))) {
    cJSON_Delete(reply);return NULL;
  }
  pet_audio_volume_status_t state={.volume=-1};
  if (!error) {
    if (!available || !pet_audio_volume_ready()) error="audio_unavailable";
    else {
      esp_err_t err=set ? pet_audio_volume_set(volume,(uint64_t)esp_timer_get_time()/1000,&state)
        : pet_audio_volume_get(&state);
      if (err==ESP_OK) *ok=true;
      else error=err==ESP_ERR_TIMEOUT ? "volume_busy" : "volume_codec_failed";
    }
  }
  cJSON_SetBoolValue(jok,*ok);
  if (state.confirmed) cJSON_SetNumberValue(jvol,state.volume);
  else jvol->type=cJSON_NULL;
  cJSON_SetBoolValue(jpersist,state.persisted);
  cJSON_SetBoolValue(jpending,state.save_pending);
  cJSON_SetBoolValue(jconfirmed,state.confirmed);
  if (!error && state.save_error) error="volume_persist_failed";
  if (error && !cJSON_AddStringToObject(payload,"error",error)) {cJSON_Delete(reply);return NULL;}
  char *line=cJSON_PrintUnformatted(reply);
  cJSON_Delete(reply);
  return line;
}
