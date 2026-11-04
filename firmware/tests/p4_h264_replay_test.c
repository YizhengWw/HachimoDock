/* Host regression: exercise the renderer's actual replay/reset/decode functions. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "pet_p4_render_trace.h"
#include "pet_p4_task_policy.h"

#define PET_P4_ASSET_CACHE_MAX_FILE_BYTES (2U * 1024U * 1024U)
#define PET_P4_ASSET_PREPARE_CHUNK_BYTES 16384U
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define PET_P4_H264_PRIMARY_TASK_PRIORITY PET_P4_H264_TASK_PRIORITY
#define ESP_TASK_MAIN_PRIO 1U
#define ESP_H264_RAW_FMT_I420 0
#define ESP_H264_ERR_OK 0
#define ESP_LOGW(...) ((void) 0)
typedef void *esp_h264_dec_handle_t;
typedef int esp_h264_err_t;
typedef unsigned UBaseType_t;
typedef struct { int pic_type; } esp_h264_dec_cfg_sw_t;
typedef struct { struct { uint8_t *buffer; uint32_t len; } raw_data; uint32_t consume; } esp_h264_dec_in_frame_t;
typedef struct { uint8_t *outbuf; uint32_t out_size; } esp_h264_dec_out_frame_t;
static char g_h264_fs_path[80];
static uint32_t g_last_decode_us;
static pet_p4_render_trace_t g_render_trace;
static unsigned allocations;
static bool fail_alloc, fail_new, fail_open;
static unsigned current_priority = ESP_TASK_MAIN_PRIO, priority_changes;
static bool native_worker;
typedef struct {unsigned unused;} pet_p4_asset_cache_entry_t;
static bool asset_prepare_current(const pet_p4_asset_cache_entry_t *job){assert(job);return true;}
static uint8_t output_pixels[6];
static void *heap_caps_realloc(void *ptr, size_t size, int caps) {
  assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  allocations++;
  return fail_alloc ? NULL : realloc(ptr, size);
}
static int esp_h264_dec_close(void *dec) { (void)dec; return 0; }
static int esp_h264_dec_del(void *dec) { (void)dec; return 0; }
static int esp_h264_dec_sw_new(const esp_h264_dec_cfg_sw_t *cfg, void **dec) {
  (void)cfg; *dec = fail_new ? NULL : output_pixels; return fail_new ? -1 : 0;
}
static int esp_h264_dec_open(void *dec) { (void)dec; return fail_open ? -1 : 0; }
static int esp_h264_dec_process(void *dec, esp_h264_dec_in_frame_t *in, esp_h264_dec_out_frame_t *out) {
  (void)dec;
  assert(native_worker?current_priority==ESP_TASK_MAIN_PRIO:current_priority>=PET_P4_H264_TASK_PRIORITY);
  // Model tinyh264's destructive NAL extraction. The input must be a private copy.
  memset(in->raw_data.buffer, 0xA5, in->raw_data.len);
  in->consume = in->raw_data.len;
  out->outbuf = output_pixels;
  out->out_size = sizeof(output_pixels);
  return 0;
}
static UBaseType_t uxTaskPriorityGet(void *task) { (void)task; return current_priority; }
static void vTaskPrioritySet(void *task, UBaseType_t priority) {
  (void)task; current_priority = priority; ++priority_changes;
}
static int64_t esp_timer_get_time(void) { return 1; }
static uint32_t elapsed_us_clamped(int64_t start, int64_t end) { (void)start; (void)end; return 1; }

/* RENDERER_IMPLEMENTATION */

int main(void) {
  static const uint8_t small[] = {0,0,0,1,9,0x10,0,0,1,6,0,0,3,1,0x80};
  static const uint8_t large[128] = {0,0,1,7,0,0,3,0,0,0,3,1};
  for (unsigned i = 0; i < 100; i++) {
    assert(start_h264_decoder("idle", small, sizeof(small)));
    assert(g_h264_work_stream != small);
    assert(memcmp(g_h264_work_stream, small, sizeof(small)) == 0);
    assert(decode_next_h264_frame(2, 2));
    assert(current_priority == ESP_TASK_MAIN_PRIO && priority_changes == (i + 1) * 2);
    assert(g_h264_decoded_frame == 0);
    assert(g_h264_work_stream[0] == 0xA5 && small[0] == 0);
  }
  assert(allocations == 1); // no frame/loop allocation churn
  for (unsigned inherited = 9; inherited <= 18; ++inherited) {
    current_priority = inherited;
    unsigned changes_before = priority_changes;
    assert(start_h264_decoder("inherited", small, sizeof(small)));
    assert(decode_next_h264_frame(2, 2));
    assert(current_priority == inherited && priority_changes == changes_before);
  }
  current_priority = ESP_TASK_MAIN_PRIO;
  assert(start_h264_decoder("working", large, sizeof(large)));
  assert(allocations == 2 && g_h264_work_size == sizeof(large));
  assert(decode_next_h264_frame(2, 2));
  /* Same primary decoder from a native worker must neither impersonate main
   * nor write the main render trace/decode counter. Original renderer loops
   * and inherited-priority assertions above are retained unchanged. */
  pet_p4_render_trace_t original=g_render_trace,worker_trace={0};
  uint32_t main_decode=g_last_decode_us,worker_decode=0;unsigned changes=priority_changes;
  pet_p4_asset_cache_entry_t job={0};native_worker=true;
  assert(start_h264_decoder_role("worker",small,sizeof(small),&worker_trace,&job));
  assert(decode_next_h264_frame_role(2,2,false,&worker_decode,&job));
  assert(priority_changes==changes&&current_priority==ESP_TASK_MAIN_PRIO);
  assert(!memcmp(&original,&g_render_trace,sizeof(original))&&g_last_decode_us==main_decode&&worker_decode);
  native_worker=false;
  assert(start_h264_decoder("idle", small, sizeof(small)));
  assert(g_h264_work_size == sizeof(small) && allocations == 2);
  assert(memcmp(g_h264_work_stream, small, sizeof(small)) == 0);
  reset_h264_decoder();
  assert(g_h264_work_size == 0 && !decode_next_h264_frame(2, 2));
  assert(!start_h264_decoder("bad", NULL, sizeof(small)));
  assert(!start_h264_decoder("bad", small, 0));
  assert(!start_h264_decoder("bad", small, PET_P4_ASSET_CACHE_MAX_FILE_BYTES + 1U));
  fail_new = true;
  assert(!start_h264_decoder("bad", small, sizeof(small)));
  assert(g_h264_work_size == 0);
  fail_new = false; fail_open = true;
  assert(!start_h264_decoder("bad", small, sizeof(small)));
  assert(g_h264_work_size == 0);
  fail_open = false;
  free(g_h264_work_stream); g_h264_work_stream = NULL; g_h264_work_capacity = 0;
  fail_alloc = true;
  assert(!start_h264_decoder("oom", large, sizeof(large)));
  assert(g_h264_work_size == 0 && !g_h264_decoder);
  fail_alloc = false;
  assert(start_h264_decoder("recovery", small, sizeof(small)));
  assert(decode_next_h264_frame(2, 2));
  reset_h264_decoder();
  free(g_h264_work_stream);
  reset_h264_decoder_measured(); /* Empty native cleanup is also trace-safe. */
  puts("PASS: 100 replays, state switches, immutable cache, bounded reuse, and failure recovery");
  return 0;
}
