"""Actual OTA init/process/status and observation boundaries, no device IO."""
from pathlib import Path
import subprocess
import pytest
from p4_control_integrity_test import _c_function
ROOT=Path(__file__).resolve().parents[1]

@pytest.mark.parametrize('cached',[0,1])
def test_actual_ota_observation_tracks_only_native_success_and_keeps_rollbacks(tmp_path,cached):
    text=(ROOT/'main/pet_p4_ota.c').read_text()
    start=text.index('/* Descriptor pointers are native boot data')
    observation=text[start:text.index('\n#endif',start)]
    functions='\n'.join(_c_function(text,name) for name in (
        'ota_state_name','send_status_locked','pet_p4_ota_send_status','pet_p4_ota_init','pet_p4_ota_process'))
    source=tmp_path/'ota.c';binary=tmp_path/'ota'
    source.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int esp_err_t;
enum {ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_NOT_FOUND=4,ESP_ERR_NOT_SUPPORTED=5};
typedef enum {ESP_OTA_IMG_NEW,ESP_OTA_IMG_PENDING_VERIFY,ESP_OTA_IMG_VALID,ESP_OTA_IMG_INVALID,ESP_OTA_IMG_ABORTED,ESP_OTA_IMG_UNDEFINED=-1} esp_ota_img_states_t;
typedef struct {char label[17];} esp_partition_t;
typedef struct {char version[32],project_name[32];} esp_app_desc_t;
typedef struct {int unused;} pet_p4_runtime_state_t;
typedef struct {int unused;} cJSON;
typedef void (*pet_p4_send_line_fn)(const char*,void*);
typedef int portMUX_TYPE;
#define DRAM_ATTR
#define portMUX_INITIALIZER_UNLOCKED 0
static bool meta_held,ota_held;
#if PET_P4_OTA_METADATA_CACHED
static void meta_enter(const void*p){assert(p&&!meta_held);meta_held=true;}
static void meta_exit(const void*p){assert(p&&meta_held);meta_held=false;}
#endif
#define portENTER_CRITICAL(p) meta_enter(p)
#define portEXIT_CRITICAL(p) meta_exit(p)
#define pdTRUE 1
#define portMAX_DELAY 0
static int g_ota_mutex;
static int xSemaphoreCreateMutex(void){assert(!meta_held&&!ota_held);return 1;}
static int xSemaphoreTake(int m,int wait){(void)wait;assert(m&&!meta_held&&!ota_held);ota_held=true;return 1;}
static void xSemaphoreGive(int m){assert(m&&ota_held&&!meta_held);ota_held=false;}
static const char *TAG="ota";
static void log_stub(const char *tag,const char *fmt,...){assert(tag&&fmt&&!meta_held);}
#define ESP_LOGI log_stub
#define ESP_LOGW log_stub
#define ESP_LOGE log_stub
#define PET_P4_OTA_VALIDATE_MIN_SAMPLES 20U
#define PET_P4_OTA_VALIDATE_STABLE_MS 8000ULL
#define PET_P4_OTA_VALIDATE_SAMPLE_GAP_MS 1000ULL
#define PET_P4_OTA_TRANSFER_IDLE_TIMEOUT_MS 30000ULL
#define PET_P4_OTA_FAILED_BOOT_RESTART_MS 20000ULL
static pet_p4_runtime_state_t *g_state;
static bool g_active,g_pending_verify,g_runtime_ready,g_initialized;
static unsigned g_runtime_healthy_samples,g_next_sequence;
static unsigned long long g_runtime_first_healthy_ms,g_runtime_last_healthy_ms,g_last_activity_ms,g_initialized_ms,g_restart_at_ms;
static unsigned long long g_received_size,g_expected_size;
static char g_transfer_id[64];
static struct {int placeholder;} g_commit_result;
static const esp_partition_t part0={"ota_0"},part1={"ota_1"};
static const esp_app_desc_t description={"test","test"};
static unsigned running_calls,boot_calls,next_calls,state_calls,mark_calls,rollback_calls,restart_calls;
static int state_error,mark_error;
static esp_ota_img_states_t native_state=ESP_OTA_IMG_PENDING_VERIFY;
static const esp_partition_t *esp_ota_get_running_partition(void){assert(!meta_held&&ota_held);running_calls++;return &part0;}
static const esp_partition_t *esp_ota_get_boot_partition(void){assert(!meta_held&&ota_held);boot_calls++;return &part0;}
static const esp_partition_t *esp_ota_get_next_update_partition(const void *p){assert(!p&&!meta_held&&ota_held);next_calls++;return &part1;}
static int esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *out){assert(p==&part0&&!meta_held&&ota_held);state_calls++;*out=native_state;return state_error;}
static const esp_app_desc_t *esp_app_get_description(void){assert(!meta_held);return &description;}
static int esp_ota_mark_app_valid_cancel_rollback(void){assert(ota_held&&!meta_held);mark_calls++;if(!mark_error)native_state=ESP_OTA_IMG_VALID;return mark_error;}
static void esp_ota_mark_app_invalid_rollback_and_reboot(void){assert(!ota_held&&!meta_held);rollback_calls++;}
static void esp_restart(void){assert(!ota_held&&!meta_held);restart_calls++;}
static int64_t esp_timer_get_time(void){assert(!meta_held);return 1000000;}
static const char *esp_err_to_name(int n){(void)n;assert(!meta_held);return "error";}
static void clear_transfer(bool abort){(void)abort;assert(false);}
static cJSON object;
static const char *image_name;
static bool boot_available;
static const esp_partition_t *seen_boot;
static unsigned status_calls;
static cJSON *cJSON_CreateObject(void){assert(!meta_held);return &object;}
static const char *json_string(const cJSON *p,const char *k){(void)p;(void)k;return "";}
static void cJSON_AddStringToObject(cJSON *o,const char *key,const char *value){assert(o&&!meta_held);if(!strcmp(key,"imageState"))image_name=value;}
static void cJSON_AddBoolToObject(cJSON *o,const char *key,bool value){assert(o&&!meta_held);if(!strcmp(key,"bootPartitionAvailable"))boot_available=value;}
static void cJSON_AddNumberToObject(cJSON *o,const char *key,double value){(void)key;(void)value;assert(o&&!meta_held);}
static void add_partition(cJSON *o,const char *key,const esp_partition_t *p){assert(o&&!meta_held);if(!strcmp(key,"bootPartition"))seen_boot=p;}
static void send_topic(pet_p4_send_line_fn send,void *ctx,const char *topic,cJSON *payload){(void)send;(void)ctx;assert(payload&&!strcmp(topic,"firmware/status")&&!meta_held);status_calls++;}
void pet_p4_ota_send_status(pet_p4_send_line_fn,void*);
#if PET_P4_OTA_METADATA_CACHED
'''+observation+r'''
#endif
'''+functions+r'''
int main(void){
#if PET_P4_OTA_METADATA_CACHED
 esp_ota_img_states_t observed=ESP_OTA_IMG_VALID;
 assert(pet_p4_ota_observed_image_state(&observed)==ESP_ERR_INVALID_STATE&&observed==ESP_OTA_IMG_UNDEFINED);
 assert(pet_p4_ota_observed_image_state(NULL)==ESP_ERR_INVALID_ARG);
 assert(!state_calls&&!boot_calls&&!running_calls&&!next_calls);
#endif
 pet_p4_runtime_state_t runtime={0};
 assert(pet_p4_ota_init(&runtime,NULL,NULL)==ESP_OK&&g_initialized&&g_pending_verify&&!ota_held&&!meta_held);
 assert(status_calls==1&&!strcmp(image_name,"pending_verify")&&seen_boot==&part0);
#if PET_P4_OTA_METADATA_CACHED
 assert(state_calls==1&&boot_calls==1&&next_calls==1&&running_calls==1&&boot_available);
 for(unsigned i=0;i<10000;i++){
   assert(pet_p4_ota_observed_image_state(&observed)==ESP_OK&&observed==ESP_OTA_IMG_PENDING_VERIFY);
   pet_p4_ota_send_status(NULL,NULL);
 }
 assert(state_calls==1&&boot_calls==1&&next_calls==1&&running_calls==1);
 observe_boot_result(-7,&part1);pet_p4_ota_send_status(NULL,NULL);assert(!boot_available&&!seen_boot);
 observe_boot_result(ESP_OK,&part1);pet_p4_ota_send_status(NULL,NULL);assert(boot_available&&seen_boot==&part1);
#else
 assert(state_calls==2&&boot_calls==1&&next_calls==2&&running_calls==2);
 pet_p4_ota_send_status(NULL,NULL);assert(state_calls==3&&boot_calls==2&&next_calls==3&&running_calls==3);
#endif
 g_runtime_healthy_samples=20;g_runtime_first_healthy_ms=2000;g_runtime_last_healthy_ms=11000;
 mark_error=-7;pet_p4_ota_process(11000);assert(mark_calls==1&&g_pending_verify&&!rollback_calls&&!restart_calls);
#if PET_P4_OTA_METADATA_CACHED
 assert(pet_p4_ota_observed_image_state(&observed)==-7&&observed==ESP_OTA_IMG_PENDING_VERIFY);
 pet_p4_ota_send_status(NULL,NULL);assert(!strcmp(image_name,"unavailable"));
#endif
 mark_error=0;g_runtime_last_healthy_ms=11001;pet_p4_ota_process(11001);
 assert(mark_calls==2&&!g_pending_verify&&!rollback_calls&&!restart_calls);
#if PET_P4_OTA_METADATA_CACHED
 assert(pet_p4_ota_observed_image_state(&observed)==ESP_OK&&observed==ESP_OTA_IMG_VALID);
#endif
 g_runtime_last_healthy_ms=11002;pet_p4_ota_process(11002);assert(mark_calls==2);
 /* The original failed-boot rollback threshold must still run unchanged. */
 g_pending_verify=true;g_runtime_healthy_samples=0;pet_p4_ota_process(22000);
 assert(rollback_calls==1&&mark_calls==2&&!restart_calls);
 /* Initial SDK read failure never invents a valid/pending boot result. */
 state_error=-9;assert(pet_p4_ota_init(&runtime,NULL,NULL)==ESP_OK&&!g_pending_verify);
 assert(!strcmp(image_name,"unavailable")&&!ota_held&&!meta_held);
#if PET_P4_OTA_METADATA_CACHED
 assert(pet_p4_ota_observed_image_state(&observed)==-9);
#endif
 return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-fsanitize=address,undefined',
        f'-DPET_P4_OTA_METADATA_CACHED={cached}',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_actual_diagnostic_cached_branch_has_no_unresolved_sdk_state_read(tmp_path):
    text=_c_function((ROOT/'main/pet_p4_diagnostics.c').read_text(),'prepare_status')
    start=text.index('  const esp_partition_t *running = esp_ota_get_running_partition();')
    end=text.index('  cJSON *payload = cJSON_CreateObject();',start)
    body=text[start:end]
    source=tmp_path/'query.c';binary=tmp_path/'query'
    source.write_text(r'''
#include <assert.h>
#include <stddef.h>
typedef int esp_err_t;typedef int esp_ota_img_states_t;typedef int esp_partition_t;typedef int esp_app_desc_t;
enum{ESP_OK=0,ESP_OTA_IMG_UNDEFINED=-1,ESP_OTA_IMG_VALID=2};
static int calls;
static const esp_partition_t *esp_ota_get_running_partition(void){return NULL;}
static const esp_app_desc_t *esp_app_get_description(void){return NULL;}
extern int esp_ota_get_state_partition(const esp_partition_t*,esp_ota_img_states_t*);
static int pet_p4_ota_observed_image_state(esp_ota_img_states_t *state){calls++;*state=ESP_OTA_IMG_VALID;return 0;}
int main(void){
'''+body+r'''
 (void)running;(void)app;assert(image_state_err==0&&image_state==ESP_OTA_IMG_VALID&&calls==1);return 0;}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DPET_P4_OTA_METADATA_CACHED=1',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_ota_cache_is_default_off_not_a_validation_or_flash_authority():
    text=(ROOT/'main/pet_p4_ota.c').read_text()
    getter=_c_function(text,'pet_p4_ota_observed_image_state')
    for forbidden in ('esp_ota_get_','esp_partition_','mmap','malloc(','cJSON','xSemaphore','g_pending_verify','g_runtime_ready'):
        assert forbidden not in getter
    commit=_c_function(text,'handle_commit')
    assert commit.index('esp_ota_end(')<commit.index('esp_ota_get_partition_description(')<commit.index('esp_ota_set_boot_partition(')<commit.index('observe_boot_result(')
    assert '#define PET_P4_OTA_METADATA_CACHED 0' in (ROOT/'main/pet_p4_ota.h').read_text()
    assert (ROOT/'platformio.ini').read_text().count('-DPET_P4_OTA_METADATA_CACHED=1')==1
