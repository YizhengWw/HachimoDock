#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_resources_device.h"
#include "pet_p4_render_trace.h"
#include "pet_p4_fs_activity.h"

#define PET_P4_ASSET_CACHE_SLOTS 2U
#define PET_P4_ASSET_CACHE_MAX_FILE_BYTES (2U * 1024U * 1024U)
#define PET_P4_ASSET_PREPARE_CHUNK_BYTES 16384U
#define PET_P4_ASSET_PREPARE_STACK_BYTES 4096U
#define PET_P4_ASSET_PREPARE_PRIORITY 1U
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define portMUX_TYPE pthread_mutex_t
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
static _Thread_local unsigned critical_depth;
#define portENTER_CRITICAL(m) do { assert(!critical_depth); pthread_mutex_lock(m); critical_depth++; } while (0)
#define portEXIT_CRITICAL(m) do { critical_depth--; pthread_mutex_unlock(m); } while (0)
static void discard_log(const char *format, ...) { (void)format; }
#define ESP_LOGI(tag, ...) discard_log(__VA_ARGS__)
#define ESP_LOGD(tag, ...) discard_log(__VA_ARGS__)
typedef void *TaskHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
static pet_resource_broker_t broker;
static pthread_mutex_t broker_lock = PTHREAD_MUTEX_INITIALIZER;
#define BROKER_BEGIN() do { assert(!critical_depth); pthread_mutex_lock(&broker_lock); } while (0)
#define BROKER_END() pthread_mutex_unlock(&broker_lock)
pet_resource_result_t pet_resources_device_owner_open(uint32_t mask, pet_resource_owner_t *out) {
  BROKER_BEGIN(); pet_resource_result_t result = pet_resources_owner_open(&broker, mask, out); BROKER_END(); return result;
}
pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t owner, pet_resource_kind_t kind, uint32_t units, pet_resource_lease_t *out) {
  BROKER_BEGIN(); pet_resource_result_t result = pet_resources_try_acquire(&broker, owner, kind, units, out); BROKER_END(); return result;
}
bool pet_resources_device_current(pet_resource_lease_t lease) {
  BROKER_BEGIN(); bool result = pet_resources_current(&broker, lease); BROKER_END(); return result;
}
void pet_resources_device_revoke(pet_resource_owner_t owner) {
  BROKER_BEGIN(); pet_resources_owner_revoke(&broker, owner); BROKER_END();
}
pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t lease) {
  BROKER_BEGIN(); pet_resource_finish_t result = pet_resources_finish(&broker, lease); BROKER_END(); return result;
}
static pet_resource_usage_t usage(void) {
  pet_resource_usage_t result; BROKER_BEGIN(); pet_resources_usage(&broker, &result); BROKER_END(); return result;
}
static _Atomic unsigned allocations, frees, reads, yields, notifications, audio_at_yield;
static _Atomic bool audio, fail_alloc, fail_read, block_read, use_raw = true;
static _Atomic int active_slot = 1;
static bool fail_task, cancel_in_locate;
static pthread_mutex_t read_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_cond = PTHREAD_COND_INITIALIZER;
static bool read_entered, read_resume;
static uint8_t source_bytes[40000];
static _Atomic int64_t clock_us;
static int64_t esp_timer_get_time(void) { return atomic_fetch_add(&clock_us, 10); }
static unsigned long long renderer_monotonic_ms(void) { return (unsigned long long) esp_timer_get_time() / 1000; }
static uint32_t elapsed_us_clamped(int64_t start, int64_t end) { return end > start ? (uint32_t)(end - start) : 0; }
static pet_p4_render_trace_t g_render_trace;
static bool pet_p4_audio_active(void) { return audio; }
static bool pet_p4_audio_playback_active(void) { return false; }
static bool pet_p4_audio_conversation_active(void) { return false; }
static bool pet_p4_audio_music_active(void) { return false; }
static void *heap_caps_malloc(size_t n, int flags) {
  assert(!critical_depth && flags == 3 && n <= PET_P4_ASSET_CACHE_MAX_FILE_BYTES);
  if (fail_alloc) return NULL;
  allocations++; return malloc(n);
}
static void heap_caps_free(void *p) { assert(!critical_depth && p); frees++; free(p); }
static void vTaskDelay(unsigned ticks) {
  assert(!critical_depth && ticks == 1); unsigned count = ++yields;
  if (audio_at_yield && count >= audio_at_yield) audio = true;
}
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *out) {
  assert(fn && !strcmp(name, "p4-asset-prep") && stack == 4096 && !arg && priority == 1);
  if (fail_task) return 0;
  *out = (void *)(uintptr_t)1; return pdPASS;
}
static void xTaskNotifyGive(TaskHandle_t task) { assert(task && !critical_depth); notifications++; }
static unsigned ulTaskNotifyTake(unsigned clear, unsigned wait) { assert(clear == pdTRUE && wait == portMAX_DELAY); return 1; }
static bool pet_p4_raw_assets_supports_path(const char *path) { assert(path); return use_raw; }
void pet_p4_renderer_cancel_asset_preparation(void);
static bool pet_p4_raw_assets_locate(int slot, const char *path, size_t size, uint32_t *base) {
  assert(!critical_depth && slot == active_slot && path && size == sizeof(source_bytes));
  *base = (uint32_t)slot * 50000 + 4096;
  if (cancel_in_locate) { cancel_in_locate = false; pet_p4_renderer_cancel_asset_preparation(); }
  return true;
}
static bool pet_p4_raw_assets_read_range(int slot, uint32_t base, size_t total, size_t offset, unsigned char *out, size_t len) {
  assert(!critical_depth && base == (uint32_t)slot * 50000 + 4096);
  assert(total == sizeof(source_bytes) && len <= 16384 && offset + len <= total);
  reads++;
  if (block_read) {
    pthread_mutex_lock(&read_lock); read_entered = true; pthread_cond_broadcast(&read_cond);
    while (!read_resume) pthread_cond_wait(&read_cond, &read_lock);
    pthread_mutex_unlock(&read_lock);
  }
  if (fail_read) return false;
  memcpy(out, source_bytes + offset, len); return true;
}
static FILE *source_file;
static unsigned file_opens, file_closes;
static FILE *captured_fopen(const char *path, const char *mode) {
  assert(!critical_depth && !strcmp(path, "/captured-slot-file") && !strcmp(mode, "rb"));
  file_opens++; rewind(source_file); return source_file;
}
static int captured_fclose(FILE *file) { assert(!critical_depth && file == source_file); file_closes++; return 0; }
#define fopen captured_fopen
#define fclose captured_fclose
/* PRODUCTION_DECLARATIONS */
/* PRODUCTION_FUNCTIONS */
#undef fopen
#undef fclose

static pet_p4_asset_cache_entry_t *request(const char *name, bool h264) {
  portENTER_CRITICAL(&g_asset_prepare_mux);
  uint64_t request_epoch = g_asset_prepare_cancel_epoch, storage_epoch = g_asset_prepare_storage_epoch;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  return load_asset_stream_cache(name, name, sizeof(source_bytes), h264, active_slot, request_epoch, storage_epoch);
}
static void run_job(void) {
  assert(g_asset_prepare_slot >= 0); prepare_asset_entry(&g_asset_cache[g_asset_prepare_slot]);
}
static void *run_thread(void *arg) { (void)arg; run_job(); return NULL; }
static void assert_empty(void) {
  pet_resource_usage_t u = usage();
  assert(!u.live_owners && !u.revoked_owners && !u.leases[PET_RESOURCE_ASSET_BUFFER] && !u.leases[PET_RESOURCE_ASSET_PREPARE]);
  assert(allocations == frees);
}
static void valid_source(void) {
  memset(source_bytes, 0x80, sizeof(source_bytes));
  const uint8_t head[] = {0,0,0,1,9,0,0,1,5}; memcpy(source_bytes, head, sizeof(head));
}
int main(void) {
  valid_source(); fail_task = true;
  assert(!request("a", true) && g_asset_cache_pending && !allocations && !reads);
  assert_empty(); fail_task = false;
  assert(!request("a", true) && g_asset_cache_pending && !allocations && !reads);
  pet_resource_usage_t u = usage(); assert(u.leases[PET_RESOURCE_ASSET_BUFFER] == 1 && u.leases[PET_RESOURCE_ASSET_PREPARE] == 1);
  run_job(); assert(reads == 3 && yields >= 5);
  pet_p4_asset_cache_entry_t *a = request("a", true);
  assert(a && a->single_slice_valid && !memcmp(a->bytes, source_bytes, sizeof(source_bytes)));
  unsigned before_reads = reads, before_yields = yields;
  pet_p4_renderer_cancel_asset_preparation(); /* Ready data survives hide/reset. */
  assert(request("a", true) == a && reads == before_reads && yields == before_yields);
  assert(!request("b", true)); run_job(); assert(request("b", true));
  assert(request("a", true) == a); /* Two ready entries, no repeated validation. */
  assert(usage().leases[PET_RESOURCE_ASSET_BUFFER] == 2 && usage().units[PET_RESOURCE_ASSET_BUFFER] == 80000);
  uint64_t old_request = g_asset_prepare_cancel_epoch, old_storage = g_asset_prepare_storage_epoch;
  pet_p4_renderer_set_asset_preparation_suspended(true);
  pet_p4_renderer_set_asset_preparation_suspended(false);
  assert(!load_asset_stream_cache("a", "a", sizeof(source_bytes), true, active_slot, old_request, old_storage));
  before_reads = reads;
  assert(!request("a", true)); run_job(); assert(request("a", true));
  assert(reads == before_reads + 3 && usage().leases[PET_RESOURCE_ASSET_BUFFER] == 1);
  active_slot = 0; before_reads = reads;
  assert(!request("a", true)); run_job(); assert(request("a", true));
  assert(reads == before_reads + 3 && usage().leases[PET_RESOURCE_ASSET_BUFFER] == 1);
  clear_asset_stream_cache(); assert_empty();

  /* Real concurrent cancellation during a physical read: no early free/lease
     release, no second worker, no old-generation delivery after completion. */
  block_read = true; read_entered = read_resume = false;
  assert(!request("old", true)); pthread_t thread; assert(!pthread_create(&thread, NULL, run_thread, NULL));
  pthread_mutex_lock(&read_lock); while (!read_entered) pthread_cond_wait(&read_cond, &read_lock); pthread_mutex_unlock(&read_lock);
  unsigned before_free = frees, before_notify = notifications;
  active_slot = 0;
  pet_p4_renderer_set_asset_preparation_suspended(true); clear_asset_stream_cache();
  assert(frees == before_free);
  u = usage(); assert(u.revoked_owners == 1 && u.leases[PET_RESOURCE_ASSET_BUFFER] == 1 && u.leases[PET_RESOURCE_ASSET_PREPARE] == 1);
  assert(!request("new", true) && g_asset_cache_pending && notifications == before_notify);
  pthread_mutex_lock(&read_lock); read_resume = true; pthread_cond_broadcast(&read_cond); pthread_mutex_unlock(&read_lock);
  assert(!pthread_join(thread, NULL)); block_read = false;
  assert(usage().leases[PET_RESOURCE_ASSET_PREPARE] == 0 && usage().leases[PET_RESOURCE_ASSET_BUFFER] == 1);
  assert(!request("new", true) && g_asset_cache_pending && frees == before_free + 1);
  assert(notifications == before_notify); /* Persistent gate blocks fresh requests too. */
  assert_empty();
  pet_p4_renderer_set_asset_preparation_suspended(false);
  assert(!request("new", true) && g_asset_cache_pending);
  run_job(); assert(request("new", true)); clear_asset_stream_cache(); assert_empty();

  /* Cancellation between descriptor capture and publication cannot enqueue. */
  cancel_in_locate = true; before_notify = notifications;
  assert(!request("race", true) && g_asset_cache_pending && notifications == before_notify); assert_empty();
  pet_p4_renderer_set_asset_preparation_suspended(true);
  assert(!request("suspended", true) && g_asset_cache_pending && notifications == before_notify);
  assert_empty(); pet_p4_renderer_set_asset_preparation_suspended(false);

  /* Read/alloc/unsafe-stream failures are terminal for this attempt, unlike
     cancelled/deferred work. No bad cache bytes may reach a decoder. */
  fail_read = true; assert(!request("readfail", true)); run_job();
  assert(!request("readfail", true) && !g_asset_cache_pending); assert_empty(); fail_read = false;
  fail_alloc = true; assert(!request("oom", true)); run_job();
  assert(!request("oom", true) && !g_asset_cache_pending); assert_empty(); fail_alloc = false;
  source_bytes[9] = 0; source_bytes[10] = 0; source_bytes[11] = 1; source_bytes[12] = 5;
  assert(!request("unsafe", true)); run_job();
  assert(!request("unsafe", true) && !g_asset_cache_pending); assert_empty(); valid_source();

  /* Audio activation between read chunks and during scanner yields both
     revoke work; no new work is admitted while audio remains busy. */
  for (unsigned stop = 1; stop <= 4; stop += 3) {
    audio_at_yield = yields + stop;
    assert(!request("audio", true)); run_job(); assert(audio);
    assert(!request("audio", true) && g_asset_cache_pending); assert_empty();
    audio = false; audio_at_yield = 0;
  }

  /* Capture the SPIFFS path once; worker never re-resolves active slot. */
  use_raw = false; source_file = tmpfile(); assert(source_file);
  assert(fwrite(source_bytes, 1, sizeof(source_bytes), source_file) == sizeof(source_bytes));
  int captured_slot = active_slot;
  assert(!request("/captured-slot-file", true)); active_slot = 1; run_job(); active_slot = captured_slot;
  assert(request("/captured-slot-file", true) && file_opens == 1 && file_closes == 1);
  clear_asset_stream_cache(); assert_empty(); fclose(source_file);

  /* Admission denial cannot leak the buffer lease or an empty owner. */
  pet_resource_owner_t other = {0}; pet_resource_lease_t other_prepare = {0};
  assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_ASSET_PREPARE), &other) == PET_RESOURCE_OK);
  assert(pet_resources_device_acquire(other, PET_RESOURCE_ASSET_PREPARE, 1, &other_prepare) == PET_RESOURCE_OK);
  assert(!request("denied", false) && g_asset_cache_pending);
  assert(usage().leases[PET_RESOURCE_ASSET_BUFFER] == 0 && usage().live_owners == 1);
  pet_resources_device_revoke(other); pet_resources_device_finish(other_prepare); assert_empty();
  cancel_pending_asset_prepares();
  puts("asset preparer production concurrency/failure tests passed");
  return 0;
}
