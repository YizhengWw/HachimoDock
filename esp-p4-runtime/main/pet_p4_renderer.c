/*
 * [Input] Pet lifecycle/session state, decoded assets, bounded mini-app views,
 *         semantic shapes, and persisted RGB565-alpha sprite frames.
 * [Output] Logical RGB565 P4 frames with aspect-fit asset scaling and an
 *          optional direct-to-panel H.264 path for full-size idle playback,
 *          plus a two-dot main/components indicator, current SW3-back/SW1-enter
 *          hints in the component catalog, modern clean cards/HUDs, and
 *          a lightweight transfer screen that never reads changing assets.
 * [Pos] ESP32-P4 display renderer.
 * [Sync] If this file changes, update `esp-p4-runtime/.folder.md` and renderer tests.
 */

#include "pet_p4_renderer.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/display.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "libs/tjpgd/tjpgd.h"
#include "lvgl.h"

#include "pet_p4_lcd.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_behavior.h"

#define PET_P4_ASSET_WIDTH 640
#define PET_P4_ASSET_HEIGHT 480
#define PET_P4_MAX_MJPEG_FRAMES PET_P4_ASSET_CATALOG_MAX_FRAMES
#define PET_P4_MAX_JPEG_BYTES (512 * 1024)
#define PET_P4_TJPGD_WORK_BUFFER_BYTES 4096
#define PET_P4_HARDWARE_JPEG_TIMEOUT_MS 40
#define PET_P4_ASSET_DECODE_BACKOFF_MS 500ULL
#define PET_P4_RENDER_INTERVAL_MS 50ULL
#define PET_P4_RENDER_FRAME_BUDGET_US 66667U
#define PET_P4_RENDER_PERF_WINDOW 100U
#define PET_P4_RENDER_PERF_GAP_RESET_US 2000000LL
#define PET_P4_PPA_BUFFER_ALIGNMENT 64U
#define PET_P4_ASSET_CACHE_SLOTS 2U
#define PET_P4_ASSET_CACHE_MAX_FILE_BYTES (2U * 1024U * 1024U)
#define PET_P4_H264_MAX_DECODE_FRAMES_PER_RENDER 1U
#define PET_P4_H264_PRIMARY_TASK_PRIORITY 16U
#define PET_P4_H264_PACK_TASK_STACK_BYTES 3072U
#define PET_P4_H264_PACK_TASK_PRIORITY 16U
#define P4_BOOT_DIAGNOSTIC_MS 3000ULL
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
#define PET_P4_SESSION_CARD_X 42
#define PET_P4_SESSION_CARD_WIDTH 556
#define PET_P4_SESSION_CARD_HEIGHT 120
#define PET_P4_SESSION_CARD_RADIUS 14
#define PET_P4_SESSION_CARD_GAP 8
#define PET_P4_SESSION_CARD_BOTTOM 460
#define PET_P4_COMPACT_TEXT_OPTICAL_Y (-2)
#define PET_P4_PAGE_INDICATOR_X 294
#define PET_P4_PAGE_INDICATOR_Y 16
#define PET_P4_PAGE_INDICATOR_WIDTH 52
#define PET_P4_PAGE_INDICATOR_HEIGHT 20
#define PET_P4_PAGE_INDICATOR_TITLE_GAP 16
#define PET_P4_SESSION_CACHE_SENTINEL 0x0001U
#define PET_P4_SESSION_CACHE_MAX_RUNS 4U

#ifdef CONFIG_ESP_H264_DUAL_TASK
#define PET_P4_H264_DECODER_MODE "software-dual"
#else
#define PET_P4_H264_DECODER_MODE "software-single"
#endif
#define PET_P4_NATIVE_OUTPUT_BUFFER_COUNT 2

static const char *TAG = "pet-p4-renderer";

extern const lv_font_t pet_p4_font_cn_16;

static uint16_t *g_framebuffer;
static uint16_t *g_native_framebuffer;
static uint16_t *g_native_framebuffers[PET_P4_NATIVE_OUTPUT_BUFFER_COUNT];
static unsigned int g_native_framebuffer_index;
static uint8_t *g_jpeg_input;
static uint16_t *g_jpeg_output;
static uint8_t *g_tjpgd_workbuf;
static jpeg_decoder_handle_t g_jpeg_decoder;
static ppa_client_handle_t g_ppa_srm_client;
static size_t g_jpeg_input_capacity;
static size_t g_jpeg_output_capacity;
static bool g_jpeg_input_dma_compatible;
static bool g_jpeg_output_dma_compatible;
static bool g_jpeg_decoder_init_attempted;
static bool g_framebuffer_ppa_compatible;
static bool g_ppa_init_attempted;
static bool g_native_framebuffer_ppa_compatible;
static bool g_logged_hardware_decoder;
static bool g_logged_hardware_fallback;
static bool g_logged_h264_decoder;
static bool g_logged_h264_ppa;
static bool g_logged_h264_ppa_fallback;
static bool g_logged_ppa_scaling;
static bool g_logged_ppa_scale_fallback;
static bool g_logged_ppa_rotation;
static bool g_logged_ppa_fallback;
static char g_decoder_fs_path[80];
static char g_rendered_family[PET_P4_ASSET_FAMILY_MAX];
static pet_p4_behavior_t g_behavior;
static pet_p4_miniapp_sprite_pack_t g_miniapp_sprites;
static bool g_logged_asset;
static bool g_logged_missing_asset;
static bool g_logged_asset_failure;
static bool g_framebuffer_initialized;
static unsigned long long g_asset_decode_blocked_until_ms;
static char g_last_render_page[PET_P4_PAGE_MAX];
static uint32_t g_last_decode_us;
static unsigned int g_asset_cache_revision;
static uint16_t *g_session_overlay_cache;
static uint64_t g_session_overlay_signature;
static bool g_session_overlay_cache_valid;
static bool g_session_overlay_cache_allocation_failed;
static uint8_t g_session_overlay_row_run_count[PET_P4_UI_HEIGHT];
static uint16_t g_session_overlay_row_run_start[PET_P4_UI_HEIGHT][PET_P4_SESSION_CACHE_MAX_RUNS];
static uint16_t g_session_overlay_row_run_end[PET_P4_UI_HEIGHT][PET_P4_SESSION_CACHE_MAX_RUNS];
static esp_h264_dec_handle_t g_h264_decoder;
static bool g_h264_decoder_open;
static size_t g_h264_stream_offset;
static int g_h264_decoded_frame = -1;
static uint8_t *g_h264_output;
static uint32_t g_h264_output_size;
static uint8_t *g_h264_ppa_input;
static size_t g_h264_ppa_input_capacity;
static TaskHandle_t g_h264_pack_task;
static SemaphoreHandle_t g_h264_pack_done;
static bool g_h264_pack_task_init_attempted;
static char g_h264_fs_path[80];
static char g_h264_validated_fs_path[80];
static bool g_h264_single_slice_valid;

typedef struct {
  const uint8_t *plane_y;
  const uint8_t *plane_u;
  const uint8_t *plane_v;
  uint8_t *packed;
  size_t packed_stride;
  int src_w;
  int src_stride;
  int row_begin;
  int row_end;
} pet_p4_h264_pack_job_t;

static pet_p4_h264_pack_job_t g_h264_pack_job;

typedef struct {
  uint8_t *bytes;
  size_t size;
  unsigned long long last_used_ms;
  char fs_path[80];
} pet_p4_asset_cache_entry_t;

static pet_p4_asset_cache_entry_t g_asset_cache[PET_P4_ASSET_CACHE_SLOTS];

typedef struct {
  uint32_t samples;
  int64_t started_us;
  int64_t last_ended_us;
  uint64_t asset_us;
  uint64_t decode_us;
  uint64_t h264_pack_us;
  uint64_t h264_convert_us;
  uint64_t overlay_us;
  uint64_t rotate_us;
  uint64_t lcd_us;
  uint64_t total_us;
  uint32_t over_budget;
  uint32_t total_samples_us[PET_P4_RENDER_PERF_WINDOW];
} pet_p4_render_perf_t;

static pet_p4_render_perf_t g_render_perf;
static int64_t g_last_slow_render_log_us;
static uint32_t g_last_h264_pack_us;
static uint32_t g_last_h264_convert_us;

static unsigned long long renderer_monotonic_ms(void) {
  return (unsigned long long) (esp_timer_get_time() / 1000ULL);
}

static bool asset_decode_is_in_backoff(void) {
  return g_asset_decode_blocked_until_ms > renderer_monotonic_ms();
}

static void note_asset_decode_failure(void) {
  unsigned long long now_ms = renderer_monotonic_ms();
  g_asset_decode_blocked_until_ms = now_ms + PET_P4_ASSET_DECODE_BACKOFF_MS;
}

static void note_asset_decode_success(void) {
  g_asset_decode_blocked_until_ms = 0;
}

static uint32_t elapsed_us_clamped(int64_t started_us, int64_t ended_us) {
  int64_t elapsed = ended_us - started_us;
  if (elapsed <= 0) return 0;
  return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t) elapsed;
}

static uint32_t perf_average_tenths_ms(uint64_t total_us, uint32_t samples) {
  if (samples == 0) return 0;
  return (uint32_t) (total_us / ((uint64_t) samples * 100ULL));
}

static void record_render_performance(
  int64_t started_us,
  int64_t ended_us,
  uint32_t asset_us,
  uint32_t overlay_us,
  uint32_t rotate_us,
  uint32_t lcd_us
) {
  uint32_t total_us = elapsed_us_clamped(started_us, ended_us);
  if (total_us > 100000U
      && (!g_last_slow_render_log_us
          || ended_us - g_last_slow_render_log_us >= 2000000LL)) {
    ESP_LOGW(
      TAG,
      "P4 slow render total=%u.%ums asset=%u.%u decode=%u.%u pack=%u.%u convert=%u.%u "
      "overlay=%u.%u rotate=%u.%u lcd=%u.%u",
      total_us / 1000U,
      (total_us % 1000U) / 100U,
      asset_us / 1000U,
      (asset_us % 1000U) / 100U,
      g_last_decode_us / 1000U,
      (g_last_decode_us % 1000U) / 100U,
      g_last_h264_pack_us / 1000U,
      (g_last_h264_pack_us % 1000U) / 100U,
      g_last_h264_convert_us / 1000U,
      (g_last_h264_convert_us % 1000U) / 100U,
      overlay_us / 1000U,
      (overlay_us % 1000U) / 100U,
      rotate_us / 1000U,
      (rotate_us % 1000U) / 100U,
      lcd_us / 1000U,
      (lcd_us % 1000U) / 100U
    );
    g_last_slow_render_log_us = ended_us;
  }
  if (total_us > PET_P4_RENDER_PERF_GAP_RESET_US) {
    memset(&g_render_perf, 0, sizeof(g_render_perf));
    return;
  }
  if (g_render_perf.samples > 0
      && started_us - g_render_perf.last_ended_us > PET_P4_RENDER_PERF_GAP_RESET_US) {
    memset(&g_render_perf, 0, sizeof(g_render_perf));
  }
  if (g_render_perf.samples == 0) g_render_perf.started_us = started_us;
  uint32_t index = g_render_perf.samples;
  if (index >= PET_P4_RENDER_PERF_WINDOW) index = PET_P4_RENDER_PERF_WINDOW - 1;
  g_render_perf.total_samples_us[index] = total_us;
  g_render_perf.samples += 1;
  g_render_perf.asset_us += asset_us;
  g_render_perf.decode_us += g_last_decode_us;
  g_render_perf.h264_pack_us += g_last_h264_pack_us;
  g_render_perf.h264_convert_us += g_last_h264_convert_us;
  g_render_perf.overlay_us += overlay_us;
  g_render_perf.rotate_us += rotate_us;
  g_render_perf.lcd_us += lcd_us;
  g_render_perf.total_us += total_us;
  g_render_perf.last_ended_us = ended_us;
  if (total_us > PET_P4_RENDER_FRAME_BUDGET_US) g_render_perf.over_budget += 1;
  if (g_render_perf.samples < PET_P4_RENDER_PERF_WINDOW) return;

  uint32_t sorted[PET_P4_RENDER_PERF_WINDOW];
  memcpy(sorted, g_render_perf.total_samples_us, sizeof(sorted));
  for (uint32_t i = 1; i < PET_P4_RENDER_PERF_WINDOW; i += 1) {
    uint32_t value = sorted[i];
    uint32_t j = i;
    while (j > 0 && sorted[j - 1] > value) {
      sorted[j] = sorted[j - 1];
      j -= 1;
    }
    sorted[j] = value;
  }

  int64_t window_us = ended_us - g_render_perf.started_us;
  uint32_t fps_tenths = window_us > 0
    ? (uint32_t) (((uint64_t) PET_P4_RENDER_PERF_WINDOW * 10000000ULL) / (uint64_t) window_us)
    : 0;
  uint32_t total_tenths = perf_average_tenths_ms(g_render_perf.total_us, g_render_perf.samples);
  uint32_t asset_tenths = perf_average_tenths_ms(g_render_perf.asset_us, g_render_perf.samples);
  uint32_t decode_tenths = perf_average_tenths_ms(g_render_perf.decode_us, g_render_perf.samples);
  uint32_t pack_tenths = perf_average_tenths_ms(g_render_perf.h264_pack_us, g_render_perf.samples);
  uint32_t convert_tenths = perf_average_tenths_ms(g_render_perf.h264_convert_us, g_render_perf.samples);
  uint32_t overlay_tenths = perf_average_tenths_ms(g_render_perf.overlay_us, g_render_perf.samples);
  uint32_t rotate_tenths = perf_average_tenths_ms(g_render_perf.rotate_us, g_render_perf.samples);
  uint32_t lcd_tenths = perf_average_tenths_ms(g_render_perf.lcd_us, g_render_perf.samples);
  uint32_t p95_tenths = sorted[94] / 100U;
  uint32_t max_tenths = sorted[PET_P4_RENDER_PERF_WINDOW - 1] / 100U;
  ESP_LOGI(
    TAG,
    "P4 render perf fps=%u.%u total=%u.%ums p95=%u.%ums max=%u.%ums "
    "asset=%u.%u decode=%u.%u pack=%u.%u convert=%u.%u overlay=%u.%u rotate=%u.%u lcd=%u.%u over66=%u/%u",
    fps_tenths / 10U,
    fps_tenths % 10U,
    total_tenths / 10U,
    total_tenths % 10U,
    p95_tenths / 10U,
    p95_tenths % 10U,
    max_tenths / 10U,
    max_tenths % 10U,
    asset_tenths / 10U,
    asset_tenths % 10U,
    decode_tenths / 10U,
    decode_tenths % 10U,
    pack_tenths / 10U,
    pack_tenths % 10U,
    convert_tenths / 10U,
    convert_tenths % 10U,
    overlay_tenths / 10U,
    overlay_tenths % 10U,
    rotate_tenths / 10U,
    rotate_tenths % 10U,
    lcd_tenths / 10U,
    lcd_tenths % 10U,
    g_render_perf.over_budget,
    PET_P4_RENDER_PERF_WINDOW
  );
  memset(&g_render_perf, 0, sizeof(g_render_perf));
}

static void clear_asset_stream_cache(void) {
  for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i += 1) {
    if (g_asset_cache[i].bytes) {
      heap_caps_free(g_asset_cache[i].bytes);
    }
    memset(&g_asset_cache[i], 0, sizeof(g_asset_cache[i]));
  }
}

static pet_p4_asset_cache_entry_t *load_asset_stream_cache(
  const char *logical_path,
  const char *fs_path,
  size_t expected_size
) {
  unsigned long long now_ms = renderer_monotonic_ms();
  for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i += 1) {
    if (g_asset_cache[i].bytes
        && g_asset_cache[i].size == expected_size
        && strcmp(g_asset_cache[i].fs_path, fs_path) == 0) {
      g_asset_cache[i].last_used_ms = now_ms;
      return &g_asset_cache[i];
    }
  }
  if (!fs_path || !fs_path[0] || expected_size == 0
      || expected_size > PET_P4_ASSET_CACHE_MAX_FILE_BYTES) {
    return NULL;
  }

  unsigned int selected = 0;
  for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i += 1) {
    if (!g_asset_cache[i].bytes) {
      selected = i;
      break;
    }
    if (g_asset_cache[i].last_used_ms < g_asset_cache[selected].last_used_ms) {
      selected = i;
    }
  }
  pet_p4_asset_cache_entry_t *entry = &g_asset_cache[selected];
  if (entry->bytes) heap_caps_free(entry->bytes);
  memset(entry, 0, sizeof(*entry));
  entry->bytes = (uint8_t *) heap_caps_malloc(
    expected_size,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
  if (!entry->bytes) return NULL;

  int64_t load_started_us = esp_timer_get_time();
  size_t read = pet_p4_asset_read_all(logical_path, entry->bytes, expected_size)
    ? expected_size
    : 0;
  if (read != expected_size) {
    ESP_LOGW(
      TAG,
      "P4 asset cache load failed path=%s want=%u got=%u",
      fs_path,
      (unsigned int) expected_size,
      (unsigned int) read
    );
    heap_caps_free(entry->bytes);
    memset(entry, 0, sizeof(*entry));
    return NULL;
  }

  entry->size = expected_size;
  entry->last_used_ms = now_ms;
  snprintf(entry->fs_path, sizeof(entry->fs_path), "%s", fs_path);
  uint32_t load_ms = elapsed_us_clamped(load_started_us, esp_timer_get_time()) / 1000U;
  ESP_LOGI(
    TAG,
    "P4 asset cache loaded slot=%u bytes=%u load=%ums path=%s",
    selected,
    (unsigned int) expected_size,
    load_ms,
    fs_path
  );
  return entry;
}

static bool decode_jpeg_frame_hardware(
  const uint8_t *input,
  size_t input_size,
  int *out_width,
  int *out_height,
  int *out_stride,
  esp_err_t *out_error
) {
  jpeg_decode_picture_info_t picture_info = {0};
  jpeg_decode_cfg_t decode_cfg = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
  };
  uint32_t decoded_size = 0;
  esp_err_t err;

  if (out_error) *out_error = ESP_ERR_INVALID_STATE;
  if (!g_jpeg_decoder || !input || input_size == 0 || !g_jpeg_output) return false;
  if (input_size > UINT32_MAX || g_jpeg_output_capacity > UINT32_MAX) {
    if (out_error) *out_error = ESP_ERR_INVALID_SIZE;
    return false;
  }

  err = jpeg_decoder_get_info(input, (uint32_t) input_size, &picture_info);
  if (err != ESP_OK) {
    if (out_error) *out_error = err;
    return false;
  }
  if (picture_info.width == 0 || picture_info.height == 0
      || picture_info.width > PET_P4_ASSET_WIDTH || picture_info.height > PET_P4_ASSET_HEIGHT) {
    if (out_error) *out_error = ESP_ERR_INVALID_SIZE;
    return false;
  }

  err = jpeg_decoder_process(
    g_jpeg_decoder,
    &decode_cfg,
    input,
    (uint32_t) input_size,
    (uint8_t *) g_jpeg_output,
    (uint32_t) g_jpeg_output_capacity,
    &decoded_size
  );
  if (err != ESP_OK) {
    if (out_error) *out_error = err;
    return false;
  }
  uint32_t mcu_width =
    picture_info.sample_method == JPEG_DOWN_SAMPLING_YUV420
      || picture_info.sample_method == JPEG_DOWN_SAMPLING_YUV422
      ? 16U : 8U;
  uint32_t mcu_height =
    picture_info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16U : 8U;
  uint32_t decoded_stride =
    (picture_info.width + mcu_width - 1U) / mcu_width * mcu_width;
  uint32_t decoded_rows =
    (picture_info.height + mcu_height - 1U) / mcu_height * mcu_height;
  if (decoded_size < decoded_stride * decoded_rows * sizeof(uint16_t)) {
    if (out_error) *out_error = ESP_ERR_INVALID_SIZE;
    return false;
  }

  if (out_width) *out_width = (int) picture_info.width;
  if (out_height) *out_height = (int) picture_info.height;
  if (out_stride) *out_stride = (int) decoded_stride;
  if (out_error) *out_error = ESP_OK;
  return true;
}

typedef struct {
  char family[PET_P4_ASSET_FAMILY_MAX];
  char path[128];
  uint32_t frame_sizes[PET_P4_MAX_MJPEG_FRAMES];
  uint32_t stream_bytes;
  int frames;
  int fps;
  uint32_t frame_duration_ms;
  uint32_t duration_ms;
  int width;
  int height;
  pet_p4_asset_codec_t codec;
} pet_p4_asset_selection_t;

typedef struct {
  const uint8_t *input;
  size_t input_size;
  size_t input_offset;
  uint16_t *output;
  size_t output_pixels;
  int width;
  int height;
} pet_p4_tjpgd_session_t;

static void blend_px(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static size_t tjpgd_input_func(JDEC *jd, uint8_t *buff, size_t ndata) {
  pet_p4_tjpgd_session_t *session = jd ? (pet_p4_tjpgd_session_t *) jd->device : NULL;
  if (!session || session->input_offset >= session->input_size) return 0;
  size_t available = session->input_size - session->input_offset;
  size_t count = ndata < available ? ndata : available;
  if (buff && count > 0) {
    memcpy(buff, session->input + session->input_offset, count);
  }
  session->input_offset += count;
  return count;
}

static int tjpgd_output_func(JDEC *jd, void *bitmap, JRECT *rect) {
  pet_p4_tjpgd_session_t *session = jd ? (pet_p4_tjpgd_session_t *) jd->device : NULL;
  const uint8_t *src = (const uint8_t *) bitmap;
  if (!session || !session->output || !src || !rect) return 0;

  int rect_w = (int) rect->right - (int) rect->left + 1;
  int rect_h = (int) rect->bottom - (int) rect->top + 1;
  if (rect_w <= 0 || rect_h <= 0) return 0;
  for (int y = 0; y < rect_h; y += 1) {
    int dst_y = (int) rect->top + y;
    if (dst_y < 0 || dst_y >= session->height) continue;
    for (int x = 0; x < rect_w; x += 1) {
      int dst_x = (int) rect->left + x;
      if (dst_x < 0 || dst_x >= session->width) continue;
      size_t dst_index = (size_t) dst_y * (size_t) session->width + (size_t) dst_x;
      if (dst_index >= session->output_pixels) return 0;
      size_t src_index = ((size_t) y * (size_t) rect_w + (size_t) x) * 3U;
      session->output[dst_index] = rgb565(src[src_index + 2], src[src_index + 1], src[src_index]);
    }
  }
  return 1;
}

static bool decode_jpeg_frame_tjpgd(const uint8_t *input, size_t input_size, int *out_width, int *out_height, JRESULT *out_error) {
  JDEC decoder;
  pet_p4_tjpgd_session_t session = {
    .input = input,
    .input_size = input_size,
    .output = g_jpeg_output,
    .output_pixels = g_jpeg_output_capacity / sizeof(uint16_t),
  };
  if (out_error) *out_error = JDR_OK;
  if (!input || input_size == 0 || !g_tjpgd_workbuf || !g_jpeg_output) {
    if (out_error) *out_error = JDR_PAR;
    return false;
  }

  JRESULT rc = jd_prepare(&decoder, tjpgd_input_func, g_tjpgd_workbuf, PET_P4_TJPGD_WORK_BUFFER_BYTES, &session);
  if (rc != JDR_OK) {
    if (out_error) *out_error = rc;
    return false;
  }
  session.width = decoder.width;
  session.height = decoder.height;
  if (session.width <= 0 || session.height <= 0 ||
      (size_t) session.width * (size_t) session.height > session.output_pixels) {
    if (out_error) *out_error = JDR_MEM1;
    return false;
  }

  rc = jd_decomp(&decoder, tjpgd_output_func, 0);
  if (rc != JDR_OK) {
    if (out_error) *out_error = rc;
    return false;
  }
  if (out_width) *out_width = session.width;
  if (out_height) *out_height = session.height;
  return true;
}

static uint16_t blend_rgb565(uint16_t dst, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
  uint8_t dr = (uint8_t) (((dst >> 11) & 0x1F) * 255 / 31);
  uint8_t dg = (uint8_t) (((dst >> 5) & 0x3F) * 255 / 63);
  uint8_t db = (uint8_t) ((dst & 0x1F) * 255 / 31);
  uint8_t nr = (uint8_t) ((r * alpha + dr * (255 - alpha)) / 255);
  uint8_t ng = (uint8_t) ((g * alpha + dg * (255 - alpha)) / 255);
  uint8_t nb = (uint8_t) ((b * alpha + db * (255 - alpha)) / 255);
  return rgb565(nr, ng, nb);
}

static void put_px(int x, int y, uint16_t color) {
  if (x < 0 || y < 0 || x >= PET_P4_UI_WIDTH || y >= PET_P4_UI_HEIGHT) return;
  if (!g_framebuffer) return;
  g_framebuffer[y * PET_P4_UI_WIDTH + x] = color;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w > PET_P4_UI_WIDTH ? PET_P4_UI_WIDTH : x + w;
  int y1 = y + h > PET_P4_UI_HEIGHT ? PET_P4_UI_HEIGHT : y + h;
  if (x0 >= x1 || y0 >= y1) return;
  if (!g_framebuffer) return;
  for (int yy = y0; yy < y1; yy += 1) {
    uint16_t *row = g_framebuffer + yy * PET_P4_UI_WIDTH;
    for (int xx = x0; xx < x1; xx += 1) row[xx] = color;
  }
}

static void fill_ellipse(int cx, int cy, int rx, int ry, uint16_t color) {
  long long rx2 = (long long) rx * rx;
  long long ry2 = (long long) ry * ry;
  long long limit = rx2 * ry2;
  for (int y = -ry; y <= ry; y += 1) {
    for (int x = -rx; x <= rx; x += 1) {
      if ((long long) x * x * ry2 + (long long) y * y * rx2 <= limit) {
        put_px(cx + x, cy + y, color);
      }
    }
  }
}

static void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
  int min_x = x0 < x1 ? x0 : x1;
  int max_x = x0 > x1 ? x0 : x1;
  int min_y = y0 < y1 ? y0 : y1;
  int max_y = y0 > y1 ? y0 : y1;
  if (x2 < min_x) min_x = x2;
  if (x2 > max_x) max_x = x2;
  if (y2 < min_y) min_y = y2;
  if (y2 > max_y) max_y = y2;

  int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
  if (area == 0) return;
  for (int y = min_y; y <= max_y; y += 1) {
    for (int x = min_x; x <= max_x; x += 1) {
      int w0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
      int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
      int w2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
      if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
          (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
        put_px(x, y, color);
      }
    }
  }
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    put_px(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

static const uint8_t *font5x7(char ch) {
  static const uint8_t glyphs[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5f,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1c,0x22,0x41,0x00},{0x00,0x41,0x22,0x1c,0x00},{0x14,0x08,0x3e,0x08,0x14},{0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3e},{0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},{0x3e,0x41,0x49,0x49,0x7a},
    {0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},
    {0x7f,0x40,0x40,0x40,0x40},{0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7f,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},{0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78},{0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},{0x7f,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7f,0x40,0x00},{0x7c,0x04,0x18,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7c,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20},{0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7f,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08},{0x00,0x00,0x00,0x00,0x00}
  };
  unsigned char c = (unsigned char) ch;
  if (c < 32 || c > 127) c = '?';
  return glyphs[c - 32];
}

static void draw_glyph5x7(int x, int y, char ch, uint16_t color, int scale) {
  const uint8_t *glyph = font5x7(ch);
  for (int col = 0; col < 5; col += 1) {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 7; row += 1) {
      if ((bits >> row) & 1) {
        fill_rect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

static uint32_t utf8_next(const char **cursor) {
  const unsigned char *s = (const unsigned char *) *cursor;
  uint32_t cp = 0;
  if (!s || !s[0]) return 0;
  if (s[0] < 0x80) {
    cp = s[0];
    *cursor += 1;
  } else if ((s[0] & 0xE0) == 0xC0 && s[1]) {
    cp = ((uint32_t) (s[0] & 0x1F) << 6) | (uint32_t) (s[1] & 0x3F);
    *cursor += 2;
  } else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
    cp = ((uint32_t) (s[0] & 0x0F) << 12) | ((uint32_t) (s[1] & 0x3F) << 6) | (uint32_t) (s[2] & 0x3F);
    *cursor += 3;
  } else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
    cp = ((uint32_t) (s[0] & 0x07) << 18) | ((uint32_t) (s[1] & 0x3F) << 12) |
         ((uint32_t) (s[2] & 0x3F) << 6) | (uint32_t) (s[3] & 0x3F);
    *cursor += 4;
  } else {
    cp = '?';
    *cursor += 1;
  }
  return cp;
}

static uint32_t utf8_peek(const char *cursor) {
  return utf8_next(&cursor);
}

static uint8_t glyph_alpha_at(const uint8_t *bitmap, const lv_font_glyph_dsc_t *glyph, int x, int y) {
  if (!bitmap || !glyph || x < 0 || y < 0 || x >= glyph->box_w || y >= glyph->box_h) return 0;
  if (glyph->format == LV_FONT_GLYPH_FORMAT_A8) {
    int stride = glyph->stride ? glyph->stride : glyph->box_w;
    return bitmap[y * stride + x];
  }
  if (glyph->format == LV_FONT_GLYPH_FORMAT_A4) {
    int pixel_index = y * glyph->box_w + x;
    int byte_index = glyph->stride ? y * glyph->stride + x / 2 : pixel_index / 2;
    uint8_t byte = bitmap[byte_index];
    uint8_t nibble = (pixel_index & 1) ? (byte & 0x0F) : (byte >> 4);
    return (uint8_t) (nibble * 17);
  }
  if (glyph->format == LV_FONT_GLYPH_FORMAT_A2) {
    int pixel_index = y * glyph->box_w + x;
    int byte_index = glyph->stride ? y * glyph->stride + x / 4 : pixel_index / 4;
    uint8_t byte = bitmap[byte_index];
    uint8_t shift = (uint8_t) ((3 - (pixel_index & 3)) * 2);
    return (uint8_t) (((byte >> shift) & 0x03) * 85);
  }
  if (glyph->format == LV_FONT_GLYPH_FORMAT_A1) {
    int pixel_index = y * glyph->box_w + x;
    int byte_index = glyph->stride ? y * glyph->stride + x / 8 : pixel_index / 8;
    uint8_t byte = bitmap[byte_index];
    return (byte & (0x80 >> (pixel_index & 7))) ? 255 : 0;
  }
  return 0;
}

static int glyph_width(uint32_t codepoint, uint32_t next_codepoint) {
  lv_font_glyph_dsc_t glyph = {0};
  if (codepoint == 0xFE0F || codepoint == 0x200D) return 0;
  if (!lv_font_get_glyph_dsc(&pet_p4_font_cn_16, &glyph, codepoint, next_codepoint)) {
    return codepoint >= 0x2300 ? 16 : 10;
  }
  return glyph.adv_w > 0 ? glyph.adv_w : glyph.box_w;
}

static bool glyph_visual_bounds(
  uint32_t codepoint,
  uint32_t next_codepoint,
  int scale,
  int *top,
  int *bottom
) {
  lv_font_glyph_dsc_t glyph = {0};
  if (codepoint == 0xFE0F || codepoint == 0x200D) return false;
  if (scale < 1) scale = 1;
  if (!lv_font_get_glyph_dsc(&pet_p4_font_cn_16, &glyph, codepoint, next_codepoint)) {
    if (codepoint == 0x00B7) {
      *top = 6 * scale;
      *bottom = 8 * scale;
    } else if (codepoint >= 0x2300) {
      *top = scale;
      *bottom = 13 * scale;
    } else {
      *top = 2 * scale;
      *bottom = 9 * scale - 1;
    }
    return true;
  }
  if (glyph.box_h <= 0 || glyph.box_w <= 0) return false;
  glyph.req_raw_bitmap = 1;
  const uint8_t *bitmap = (const uint8_t *) glyph.resolved_font->get_glyph_bitmap(&glyph, NULL);
  int first_ink_row = glyph.box_h;
  int last_ink_row = -1;
  if (bitmap) {
    for (int y = 0; y < glyph.box_h; y += 1) {
      for (int x = 0; x < glyph.box_w; x += 1) {
        if (!glyph_alpha_at(bitmap, &glyph, x, y)) continue;
        if (y < first_ink_row) first_ink_row = y;
        if (y > last_ink_row) last_ink_row = y;
      }
    }
  }
  lv_font_glyph_release_draw_data(&glyph);
  if (last_ink_row < first_ink_row) return false;
  int baseline = (pet_p4_font_cn_16.line_height - pet_p4_font_cn_16.base_line) * scale;
  int glyph_top = baseline - glyph.box_h * scale - glyph.ofs_y * scale;
  *top = glyph_top + first_ink_row * scale;
  *bottom = glyph_top + (last_ink_row + 1) * scale - 1;
  return true;
}

static void text_visual_bounds(const char *text, int scale, int *top, int *bottom) {
  const char *cursor = text ? text : "";
  bool found = false;
  while (*cursor) {
    const char *next_cursor = cursor;
    uint32_t codepoint = utf8_next(&next_cursor);
    int glyph_top = 0;
    int glyph_bottom = 0;
    if (glyph_visual_bounds(
          codepoint,
          utf8_peek(next_cursor),
          scale,
          &glyph_top,
          &glyph_bottom
        )) {
      if (!found || glyph_top < *top) *top = glyph_top;
      if (!found || glyph_bottom > *bottom) *bottom = glyph_bottom;
      found = true;
    }
    cursor = next_cursor;
  }
  if (!found) {
    *top = 0;
    *bottom = pet_p4_font_cn_16.line_height * (scale > 0 ? scale : 1) - 1;
  }
}

static int utf8_text_width(const char *text, int byte_limit, int scale) {
  const char *cursor = text ? text : "";
  const char *end = byte_limit >= 0 ? cursor + byte_limit : NULL;
  int width = 0;
  if (scale < 1) scale = 1;
  while (*cursor && (!end || cursor < end)) {
    const char *next_cursor = cursor;
    uint32_t cp = utf8_next(&next_cursor);
    if (!cp || (end && next_cursor > end)) break;
    int glyph_advance = glyph_width(cp, utf8_peek(next_cursor));
    if (glyph_advance > 0) width += glyph_advance * scale + scale;
    cursor = next_cursor;
  }
  return width;
}

static void draw_lvgl_glyph(uint32_t codepoint, uint32_t next_codepoint, int x, int y, uint16_t color, int scale) {
  lv_font_glyph_dsc_t glyph = {0};
  if (codepoint == 0xFE0F || codepoint == 0x200D) return;
  if (!lv_font_get_glyph_dsc(&pet_p4_font_cn_16, &glyph, codepoint, next_codepoint)) {
    if (scale < 1) scale = 1;
    if (codepoint == 0x00B7) {
      fill_ellipse(x + 4 * scale, y + 7 * scale, scale, scale, color);
    } else if (codepoint >= 0x2300) {
      int cx = x + 7 * scale;
      int cy = y + 7 * scale;
      draw_line(cx, y + scale, cx, y + 13 * scale, color);
      draw_line(x + scale, cy, x + 13 * scale, cy, color);
      fill_ellipse(cx, cy, 3 * scale, 3 * scale, color);
    } else {
      draw_glyph5x7(x, y + 2 * scale, '?', color, scale);
    }
    return;
  }
  if (scale < 1) scale = 1;
  glyph.req_raw_bitmap = 1;
  const uint8_t *bitmap = (const uint8_t *) glyph.resolved_font->get_glyph_bitmap(&glyph, NULL);
  if (!bitmap) {
    lv_font_glyph_release_draw_data(&glyph);
    return;
  }
  int baseline = y + (pet_p4_font_cn_16.line_height - pet_p4_font_cn_16.base_line) * scale;
  int gx0 = x + glyph.ofs_x * scale;
  int gy0 = baseline - glyph.box_h * scale - glyph.ofs_y * scale;
  uint8_t r = (uint8_t) (((color >> 11) & 0x1F) * 255 / 31);
  uint8_t g = (uint8_t) (((color >> 5) & 0x3F) * 255 / 63);
  uint8_t b = (uint8_t) ((color & 0x1F) * 255 / 31);
  for (int gy = 0; gy < glyph.box_h; gy += 1) {
    for (int gx = 0; gx < glyph.box_w; gx += 1) {
      uint8_t alpha = glyph_alpha_at(bitmap, &glyph, gx, gy);
      if (alpha) {
        for (int sy = 0; sy < scale; sy += 1) {
          for (int sx = 0; sx < scale; sx += 1) {
            blend_px(gx0 + gx * scale + sx, gy0 + gy * scale + sy, r, g, b, alpha);
          }
        }
      }
    }
  }
  lv_font_glyph_release_draw_data(&glyph);
}

static int medium_glyph_width(uint32_t codepoint, uint32_t next_codepoint) {
  int width = glyph_width(codepoint, next_codepoint);
  return width > 0 ? (width * 3 + 1) / 2 : 0;
}

static bool medium_glyph_visual_bounds(
  uint32_t codepoint,
  uint32_t next_codepoint,
  int *top,
  int *bottom
) {
  lv_font_glyph_dsc_t glyph = {0};
  if (codepoint == 0xFE0F || codepoint == 0x200D) return false;
  if (!lv_font_get_glyph_dsc(&pet_p4_font_cn_16, &glyph, codepoint, next_codepoint)) {
    if (codepoint == 0x00B7) {
      *top = 8;
      *bottom = 12;
    } else if (codepoint >= 0x2300) {
      *top = 2;
      *bottom = 18;
    } else {
      *top = 3;
      *bottom = 16;
    }
    return true;
  }
  if (glyph.box_h <= 0 || glyph.box_w <= 0) return false;
  glyph.req_raw_bitmap = 1;
  const uint8_t *bitmap = (const uint8_t *) glyph.resolved_font->get_glyph_bitmap(&glyph, NULL);
  int scaled_height = (glyph.box_h * 3 + 1) / 2;
  int first_ink_row = scaled_height;
  int last_ink_row = -1;
  if (bitmap) {
    for (int dy = 0; dy < scaled_height; dy += 1) {
      int source_y = dy * 2 / 3;
      if (source_y >= glyph.box_h) source_y = glyph.box_h - 1;
      for (int x = 0; x < glyph.box_w; x += 1) {
        if (!glyph_alpha_at(bitmap, &glyph, x, source_y)) continue;
        if (dy < first_ink_row) first_ink_row = dy;
        if (dy > last_ink_row) last_ink_row = dy;
      }
    }
  }
  lv_font_glyph_release_draw_data(&glyph);
  if (last_ink_row < first_ink_row) return false;
  int baseline = ((pet_p4_font_cn_16.line_height - pet_p4_font_cn_16.base_line) * 3 + 1) / 2;
  int glyph_top = baseline - scaled_height - glyph.ofs_y * 3 / 2;
  *top = glyph_top + first_ink_row;
  *bottom = glyph_top + last_ink_row;
  return true;
}

static void text_visual_bounds_medium(const char *text, int *top, int *bottom) {
  const char *cursor = text ? text : "";
  bool found = false;
  while (*cursor) {
    const char *next_cursor = cursor;
    uint32_t codepoint = utf8_next(&next_cursor);
    int glyph_top = 0;
    int glyph_bottom = 0;
    if (medium_glyph_visual_bounds(
          codepoint,
          utf8_peek(next_cursor),
          &glyph_top,
          &glyph_bottom
        )) {
      if (!found || glyph_top < *top) *top = glyph_top;
      if (!found || glyph_bottom > *bottom) *bottom = glyph_bottom;
      found = true;
    }
    cursor = next_cursor;
  }
  if (!found) {
    *top = 0;
    *bottom = (pet_p4_font_cn_16.line_height * 3 + 1) / 2 - 1;
  }
}

static int utf8_text_width_medium(const char *text) {
  const char *cursor = text ? text : "";
  int width = 0;
  while (*cursor) {
    const char *next_cursor = cursor;
    uint32_t codepoint = utf8_next(&next_cursor);
    int glyph_advance = medium_glyph_width(codepoint, utf8_peek(next_cursor));
    if (glyph_advance > 0) width += glyph_advance + 1;
    cursor = next_cursor;
  }
  return width;
}

static void draw_lvgl_glyph_medium(
  uint32_t codepoint,
  uint32_t next_codepoint,
  int x,
  int y,
  uint16_t color
) {
  lv_font_glyph_dsc_t glyph = {0};
  if (codepoint == 0xFE0F || codepoint == 0x200D) return;
  if (!lv_font_get_glyph_dsc(&pet_p4_font_cn_16, &glyph, codepoint, next_codepoint)) {
    if (codepoint == 0x00B7) {
      fill_ellipse(x + 6, y + 10, 2, 2, color);
    } else if (codepoint >= 0x2300) {
      draw_line(x + 10, y + 2, x + 10, y + 18, color);
      draw_line(x + 2, y + 10, x + 18, y + 10, color);
      fill_ellipse(x + 10, y + 10, 4, 4, color);
    } else {
      draw_glyph5x7(x, y + 3, '?', color, 2);
    }
    return;
  }
  glyph.req_raw_bitmap = 1;
  const uint8_t *bitmap = (const uint8_t *) glyph.resolved_font->get_glyph_bitmap(&glyph, NULL);
  if (!bitmap) {
    lv_font_glyph_release_draw_data(&glyph);
    return;
  }
  int baseline = y + ((pet_p4_font_cn_16.line_height - pet_p4_font_cn_16.base_line) * 3 + 1) / 2;
  int gx0 = x + glyph.ofs_x * 3 / 2;
  int gy0 = baseline - (glyph.box_h * 3 + 1) / 2 - glyph.ofs_y * 3 / 2;
  int scaled_w = (glyph.box_w * 3 + 1) / 2;
  int scaled_h = (glyph.box_h * 3 + 1) / 2;
  uint8_t r = (uint8_t) (((color >> 11) & 0x1F) * 255 / 31);
  uint8_t g = (uint8_t) (((color >> 5) & 0x3F) * 255 / 63);
  uint8_t b = (uint8_t) ((color & 0x1F) * 255 / 31);
  for (int dy = 0; dy < scaled_h; dy += 1) {
    int source_y = dy * 2 / 3;
    if (source_y >= glyph.box_h) source_y = glyph.box_h - 1;
    for (int dx = 0; dx < scaled_w; dx += 1) {
      int source_x = dx * 2 / 3;
      if (source_x >= glyph.box_w) source_x = glyph.box_w - 1;
      uint8_t alpha = glyph_alpha_at(bitmap, &glyph, source_x, source_y);
      if (alpha) blend_px(gx0 + dx, gy0 + dy, r, g, b, alpha);
    }
  }
  lv_font_glyph_release_draw_data(&glyph);
}

static const char *draw_text_line_medium(
  const char *text,
  int x,
  int y,
  int max_px,
  uint16_t color,
  bool ellipsis
) {
  const char *cursor = text ? text : "";
  const char *last_fit = cursor;
  int pen_x = x;
  int used_px = 0;
  bool needs_ellipsis = ellipsis && utf8_text_width_medium(cursor) > max_px;
  int reserve_px = needs_ellipsis ? utf8_text_width_medium("...") : 0;
  while (*cursor) {
    const char *next_cursor = cursor;
    uint32_t codepoint = utf8_next(&next_cursor);
    uint32_t next_codepoint = utf8_peek(next_cursor);
    int glyph_advance = medium_glyph_width(codepoint, next_codepoint);
    int advance = glyph_advance > 0 ? glyph_advance + 1 : 0;
    if (used_px + advance + reserve_px > max_px) break;
    draw_lvgl_glyph_medium(codepoint, next_codepoint, pen_x, y, color);
    pen_x += advance;
    used_px += advance;
    cursor = next_cursor;
    last_fit = cursor;
  }
  if (needs_ellipsis && *cursor) {
    const char *dots = "...";
    while (*dots) {
      const char *next_cursor = dots;
      uint32_t codepoint = utf8_next(&next_cursor);
      uint32_t next_codepoint = utf8_peek(next_cursor);
      draw_lvgl_glyph_medium(codepoint, next_codepoint, pen_x, y, color);
      pen_x += medium_glyph_width(codepoint, next_codepoint) + 1;
      dots = next_cursor;
    }
  }
  return last_fit;
}

static int append_display_char(char *out, int used, int max_chars, const char **cursor) {
  const unsigned char *s = (const unsigned char *) *cursor;
  if (!s || !s[0] || used >= max_chars) return used;
  if (s[0] < 0x80) {
    out[used++] = (char) s[0];
    *cursor += 1;
  } else {
    out[used++] = '?';
    int skip = 1;
    if ((s[0] & 0xE0) == 0xC0) skip = 2;
    else if ((s[0] & 0xF0) == 0xE0) skip = 3;
    else if ((s[0] & 0xF8) == 0xF0) skip = 4;
    *cursor += skip;
  }
  return used;
}

static const char *draw_text_line(
  const char *text,
  int x,
  int y,
  int max_px,
  uint16_t color,
  int scale,
  bool ellipsis
) {
  if (scale < 1) scale = 1;
  const char *cursor = text ? text : "";
  const char *last_fit = cursor;
  int pen_x = x;
  int used_px = 0;
  int ellipsis_px = utf8_text_width("...", -1, scale);
  bool needs_ellipsis = ellipsis && utf8_text_width(cursor, -1, scale) > max_px;
  int reserve_px = needs_ellipsis ? ellipsis_px : 0;
  while (*cursor) {
    const char *next_cursor = cursor;
    uint32_t cp = utf8_next(&next_cursor);
    uint32_t next_cp = utf8_peek(next_cursor);
    int glyph_advance = glyph_width(cp, next_cp);
    int adv = glyph_advance > 0 ? glyph_advance * scale + scale : 0;
    if (used_px + adv + reserve_px > max_px) break;
    draw_lvgl_glyph(cp, next_cp, pen_x, y, color, scale);
    pen_x += adv;
    used_px += adv;
    cursor = next_cursor;
    last_fit = cursor;
  }
  if (needs_ellipsis && *cursor) {
    const char *dots = "...";
    while (*dots) {
      const char *next_cursor = dots;
      uint32_t cp = utf8_next(&next_cursor);
      uint32_t next_cp = utf8_peek(next_cursor);
      draw_lvgl_glyph(cp, next_cp, pen_x, y, color, scale);
      pen_x += glyph_width(cp, next_cp) * scale + scale;
      dots = next_cursor;
    }
  }
  cursor = last_fit;
  return cursor;
}

static int text_y_in_box(const char *text, int scale, int box_y, int box_h) {
  int top = 0;
  int bottom = 0;
  text_visual_bounds(text, scale, &top, &bottom);
  return (box_y * 2 + box_h - 1 - top - bottom) / 2;
}

static int text_y_in_box_medium(const char *text, int box_y, int box_h) {
  int top = 0;
  int bottom = 0;
  text_visual_bounds_medium(text, &top, &bottom);
  return (box_y * 2 + box_h - 1 - top - bottom) / 2;
}

static const char *draw_text_line_vcenter(
  const char *text,
  int x,
  int box_y,
  int box_h,
  int max_px,
  uint16_t color,
  int scale,
  bool ellipsis
) {
  return draw_text_line(
    text,
    x,
    text_y_in_box(text, scale, box_y, box_h),
    max_px,
    color,
    scale,
    ellipsis
  );
}

static const char *draw_text_line_medium_vcenter(
  const char *text,
  int x,
  int box_y,
  int box_h,
  int max_px,
  uint16_t color,
  bool ellipsis
) {
  return draw_text_line_medium(
    text,
    x,
    text_y_in_box_medium(text, box_y, box_h),
    max_px,
    color,
    ellipsis
  );
}

static int display_char_count(const char *text, int limit) {
  int count = 0;
  const unsigned char *s = (const unsigned char *) (text ? text : "");
  while (*s && count < limit) {
    if (*s < 0x80) s += 1;
    else if ((*s & 0xE0) == 0xC0) s += 2;
    else if ((*s & 0xF0) == 0xE0) s += 3;
    else if ((*s & 0xF8) == 0xF0) s += 4;
    else s += 1;
    count += 1;
  }
  return count;
}

static void fill_round_rect(int x, int y, int w, int h, int radius, uint16_t color) {
  fill_rect(x + radius, y, w - radius * 2, h, color);
  fill_rect(x, y + radius, w, h - radius * 2, color);
  fill_ellipse(x + radius, y + radius, radius, radius, color);
  fill_ellipse(x + w - radius - 1, y + radius, radius, radius, color);
  fill_ellipse(x + radius, y + h - radius - 1, radius, radius, color);
  fill_ellipse(x + w - radius - 1, y + h - radius - 1, radius, radius, color);
}

static void blend_px(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
  if (x < 0 || y < 0 || x >= PET_P4_UI_WIDTH || y >= PET_P4_UI_HEIGHT) return;
  if (!g_framebuffer) return;
  uint16_t *px = &g_framebuffer[y * PET_P4_UI_WIDTH + x];
  *px = blend_rgb565(*px, r, g, b, alpha);
}

static void blend_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w > PET_P4_UI_WIDTH ? PET_P4_UI_WIDTH : x + w;
  int y1 = y + h > PET_P4_UI_HEIGHT ? PET_P4_UI_HEIGHT : y + h;
  if (x0 >= x1 || y0 >= y1) return;
  for (int yy = y0; yy < y1; yy += 1) {
    for (int xx = x0; xx < x1; xx += 1) blend_px(xx, yy, r, g, b, alpha);
  }
}

static void blend_ellipse(int cx, int cy, int rx, int ry, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
  long long rx2 = (long long) rx * rx;
  long long ry2 = (long long) ry * ry;
  long long limit = rx2 * ry2;
  for (int y = -ry; y <= ry; y += 1) {
    for (int x = -rx; x <= rx; x += 1) {
      if ((long long) x * x * ry2 + (long long) y * y * rx2 <= limit) {
        blend_px(cx + x, cy + y, r, g, b, alpha);
      }
    }
  }
}

static void blend_round_rect(int x, int y, int w, int h, int radius, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
  blend_rect(x + radius, y, w - radius * 2, h, r, g, b, alpha);
  blend_rect(x, y + radius, w, h - radius * 2, r, g, b, alpha);
  blend_ellipse(x + radius, y + radius, radius, radius, r, g, b, alpha);
  blend_ellipse(x + w - radius - 1, y + radius, radius, radius, r, g, b, alpha);
  blend_ellipse(x + radius, y + h - radius - 1, radius, radius, r, g, b, alpha);
  blend_ellipse(x + w - radius - 1, y + h - radius - 1, radius, radius, r, g, b, alpha);
}

static void draw_home_white_panel(int x, int y, int w, int h, int radius, uint16_t outline) {
  // The panel is opaque, so a preblended solid shadow gives the same visible
  // edge without read/modify/write alpha blending tens of thousands of PSRAM
  // pixels every frame.
  fill_round_rect(x + 2, y + 4, w, h, radius, rgb565(55, 58, 57));
  fill_round_rect(x, y, w, h, radius, outline);
  fill_round_rect(x + 2, y + 2, w - 4, h - 4, radius - 2, rgb565(255, 255, 255));
}

static void draw_session_card_panel(
  int x,
  int y,
  int w,
  int h,
  int radius,
  bool selected,
  uint16_t outline
) {
  if (!selected) {
    draw_home_white_panel(x, y, w, h, radius, outline);
    return;
  }

  uint16_t selection = rgb565(25, 151, 99);
  uint16_t selected_surface = rgb565(237, 249, 242);
  fill_round_rect(x - 2, y - 2, w + 4, h + 4, radius + 2, rgb565(173, 220, 193));
  fill_round_rect(x + 2, y + 5, w, h, radius, rgb565(46, 50, 48));
  fill_round_rect(x, y, w, h, radius, selection);
  fill_round_rect(x + 4, y + 4, w - 8, h - 8, radius - 4, selected_surface);
}

static void draw_card_body_lines(
  const char *body,
  int x,
  int first_line_y,
  int max_px,
  uint16_t color
) {
  const char *second_line = draw_text_line_medium_vcenter(
    body,
    x,
    first_line_y,
    27,
    max_px,
    color,
    false
  );
  while (second_line && (*second_line == ' ' || *second_line == '\t')) second_line += 1;
  if (second_line && second_line[0]) {
    draw_text_line_medium_vcenter(second_line, x, first_line_y + 27, 27, max_px, color, true);
  }
}

static void fill_round_rect_outline(
  int x,
  int y,
  int w,
  int h,
  int radius,
  uint16_t fill,
  uint16_t outline
) {
  fill_round_rect(x, y, w, h, radius, outline);
  fill_round_rect(x + 2, y + 2, w - 4, h - 4, radius - 2, fill);
}

static void draw_status_marker(pet_p4_view_status_t status, unsigned long long now_ms, int cx, int cy) {
  uint16_t white = rgb565(245, 245, 245);
  uint16_t green = rgb565(0, 190, 95);
  uint16_t red = rgb565(225, 64, 64);
  uint16_t gray = rgb565(82, 90, 105);
  uint16_t amber = rgb565(232, 157, 38);
  uint16_t teal = rgb565(0, 176, 164);

  if (status == PET_P4_VIEW_STATUS_IDLE) return;
  if (status == PET_P4_VIEW_STATUS_DONE) {
    fill_ellipse(cx, cy, 15, 15, green);
    draw_line(cx - 7, cy, cx - 2, cy + 6, white);
    draw_line(cx - 2, cy + 6, cx + 8, cy - 7, white);
    return;
  }
  if (status == PET_P4_VIEW_STATUS_ERROR) {
    fill_ellipse(cx, cy, 15, 15, red);
    draw_line(cx - 6, cy - 6, cx + 6, cy + 6, white);
    draw_line(cx + 6, cy - 6, cx - 6, cy + 6, white);
    return;
  }
  if (status == PET_P4_VIEW_STATUS_WAITING) {
    fill_ellipse(cx, cy, 15, 15, amber);
    fill_ellipse(cx - 6, cy, 2, 2, white);
    fill_ellipse(cx, cy, 2, 2, white);
    fill_ellipse(cx + 6, cy, 2, 2, white);
    return;
  }
  int phase = (int) ((now_ms / 150ULL) % 8ULL);
  for (int i = 0; i < 8; i += 1) {
    int dx[] = {0, 9, 13, 9, 0, -9, -13, -9};
    int dy[] = {-13, -9, 0, 9, 13, 9, 0, -9};
    uint16_t c = i == phase ? teal : gray;
    fill_ellipse(cx + dx[i], cy + dy[i], 3, 3, c);
  }
}

static uint16_t view_status_color(pet_p4_view_status_t status) {
  if (status == PET_P4_VIEW_STATUS_DONE) return rgb565(85, 193, 133);
  if (status == PET_P4_VIEW_STATUS_ERROR) return rgb565(235, 89, 82);
  if (status == PET_P4_VIEW_STATUS_WAITING) return rgb565(255, 163, 31);
  if (status == PET_P4_VIEW_STATUS_WORKING) return rgb565(96, 196, 255);
  return rgb565(170, 176, 173);
}

static bool select_asset(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms,
  pet_p4_asset_selection_t *out
) {
  const pet_p4_asset_entry_t *entry;
  int index;
  if (!state || !out) return false;
  index = pet_p4_behavior_select(
    &g_behavior,
    &state->asset_catalog,
    pet_p4_state_effective_lifecycle(state, now_ms),
    state->asset_revision,
    now_ms
  );
  if (index < 0 || index >= state->asset_catalog.count) return false;
  entry = &state->asset_catalog.entries[index];
  memset(out, 0, sizeof(*out));
  snprintf(out->family, sizeof(out->family), "%s", entry->family);
  snprintf(out->path, sizeof(out->path), "%s", entry->path);
  out->codec = entry->codec;
  out->stream_bytes = entry->stream_bytes;
  out->frames = entry->frames;
  out->fps = entry->fps;
  out->frame_duration_ms = entry->frame_duration_ms;
  out->duration_ms = entry->duration_ms;
  out->width = entry->width;
  out->height = entry->height;
  for (int i = 0; i < entry->frames && i < PET_P4_MAX_MJPEG_FRAMES; i += 1) {
    out->frame_sizes[i] = entry->frame_sizes[i];
  }
  return out->path[0] && out->frames > 0;
}

static bool asset_fit_layout(
  int src_w,
  int src_h,
  int *dst_x,
  int *dst_y,
  int *dst_w,
  int *dst_h,
  float *out_scale
) {
  if (src_w <= 0 || src_h <= 0 || !dst_x || !dst_y || !dst_w || !dst_h) {
    return false;
  }
  uint32_t scale_x_sixteenths =
    (((uint32_t) PET_P4_UI_WIDTH + 1U) * 16U - 1U) / (uint32_t) src_w;
  uint32_t scale_y_sixteenths =
    (((uint32_t) PET_P4_UI_HEIGHT + 1U) * 16U - 1U) / (uint32_t) src_h;
  uint32_t scale_sixteenths =
    scale_x_sixteenths < scale_y_sixteenths ? scale_x_sixteenths : scale_y_sixteenths;
  if (scale_sixteenths == 0) return false;
  if (scale_sixteenths > 255U) scale_sixteenths = 255U;

  *dst_w = (int) (((uint32_t) src_w * scale_sixteenths) / 16U);
  *dst_h = (int) (((uint32_t) src_h * scale_sixteenths) / 16U);
  if (*dst_w <= 0 || *dst_h <= 0
      || *dst_w > PET_P4_UI_WIDTH || *dst_h > PET_P4_UI_HEIGHT) {
    return false;
  }
  *dst_x = (PET_P4_UI_WIDTH - *dst_w) / 2;
  *dst_y = (PET_P4_UI_HEIGHT - *dst_h) / 2;
  if (out_scale) *out_scale = (float) scale_sixteenths / 16.0f;
  return true;
}

static bool scale_asset_frame_ppa(int src_w, int src_h, int src_stride) {
  int dst_x;
  int dst_y;
  int dst_w;
  int dst_h;
  float scale;
  if (!g_ppa_srm_client || !g_framebuffer_ppa_compatible || src_stride < src_w
      || !asset_fit_layout(src_w, src_h, &dst_x, &dst_y, &dst_w, &dst_h, &scale)) {
    return false;
  }

  ppa_srm_oper_config_t scaling = {
    .in = {
      .buffer = g_jpeg_output,
      .pic_w = (uint32_t) src_stride,
      .pic_h = (uint32_t) src_h,
      .block_w = (uint32_t) src_w,
      .block_h = (uint32_t) src_h,
      .block_offset_x = 0,
      .block_offset_y = 0,
      .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
    },
    .out = {
      .buffer = g_framebuffer,
      .buffer_size = PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t),
      .pic_w = PET_P4_UI_WIDTH,
      .pic_h = PET_P4_UI_HEIGHT,
      .block_offset_x = (uint32_t) dst_x,
      .block_offset_y = (uint32_t) dst_y,
      .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
    },
    .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
    .scale_x = scale,
    .scale_y = scale,
    .rgb_swap = false,
    .byte_swap = false,
    .mode = PPA_TRANS_MODE_BLOCKING,
  };
  esp_err_t err = ppa_do_scale_rotate_mirror(g_ppa_srm_client, &scaling);
  if (err == ESP_OK) {
    if (!g_logged_ppa_scaling) {
      ESP_LOGI(
        TAG,
        "P4 asset scale=PPA source=%dx%d stride=%d output=%dx%d offset=%d,%d factor=%.3f",
        src_w,
        src_h,
        src_stride,
        dst_w,
        dst_h,
        dst_x,
        dst_y,
        (double) scale
      );
      g_logged_ppa_scaling = true;
    }
    return true;
  }
  if (!g_logged_ppa_scale_fallback) {
    ESP_LOGW(TAG, "P4 PPA asset scaling failed (%s); using CPU fallback", esp_err_to_name(err));
    g_logged_ppa_scale_fallback = true;
  }
  return false;
}

static void scale_asset_frame_cpu(int src_w, int src_h, int src_stride) {
  int dst_x;
  int dst_y;
  int dst_w;
  int dst_h;
  if (src_stride < src_w
      || !asset_fit_layout(src_w, src_h, &dst_x, &dst_y, &dst_w, &dst_h, NULL)) {
    return;
  }
  for (int y = 0; y < dst_h; y += 1) {
    int sy = (y * src_h) / dst_h;
    const uint16_t *source = g_jpeg_output + sy * src_stride;
    uint16_t *target = g_framebuffer + (dst_y + y) * PET_P4_UI_WIDTH + dst_x;
    for (int x = 0; x < dst_w; x += 1) {
      target[x] = source[(x * src_w) / dst_w];
    }
  }
}

static void scale_asset_frame_to_display(int src_w, int src_h, int src_stride) {
  if (src_w == PET_P4_UI_WIDTH && src_h == PET_P4_UI_HEIGHT && src_stride == src_w) {
    memcpy(
      g_framebuffer,
      g_jpeg_output,
      PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t)
    );
    return;
  }
  memset(g_framebuffer, 0, PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t));
  if (!scale_asset_frame_ppa(src_w, src_h, src_stride)) {
    scale_asset_frame_cpu(src_w, src_h, src_stride);
  }
}

static void reset_h264_decoder(void) {
  if (g_h264_decoder) {
    if (g_h264_decoder_open) {
      (void) esp_h264_dec_close(g_h264_decoder);
    }
    (void) esp_h264_dec_del(g_h264_decoder);
  }
  g_h264_decoder = NULL;
  g_h264_decoder_open = false;
  g_h264_stream_offset = 0;
  g_h264_decoded_frame = -1;
  g_h264_output = NULL;
  g_h264_output_size = 0;
  g_h264_fs_path[0] = '\0';
}

void pet_p4_renderer_reset_playback(void) {
  pet_p4_behavior_init(&g_behavior);
  reset_h264_decoder();
  g_decoder_fs_path[0] = '\0';
  g_h264_validated_fs_path[0] = '\0';
  g_h264_single_slice_valid = false;
}

static bool h264_stream_has_single_slice_access_units(
  const uint8_t *stream,
  size_t stream_size
) {
  size_t cursor = 0;
  unsigned int access_units = 0;
  unsigned int slices_in_access_unit = 0;
  if (!stream || stream_size < 5) return false;
  while (cursor + 3 < stream_size) {
    size_t prefix_size = 0;
    if (cursor + 4 <= stream_size
        && stream[cursor] == 0 && stream[cursor + 1] == 0
        && stream[cursor + 2] == 0 && stream[cursor + 3] == 1) {
      prefix_size = 4;
    } else if (stream[cursor] == 0 && stream[cursor + 1] == 0
               && stream[cursor + 2] == 1) {
      prefix_size = 3;
    }
    if (prefix_size == 0) {
      cursor += 1;
      continue;
    }
    size_t payload_start = cursor + prefix_size;
    if (payload_start >= stream_size) break;
    uint8_t nal_type = stream[payload_start] & 0x1fU;
    if (nal_type == 9U) {
      if (access_units > 0 && slices_in_access_unit != 1U) return false;
      access_units += 1U;
      slices_in_access_unit = 0;
    } else if (nal_type == 1U || nal_type == 5U) {
      if (access_units == 0) return false;
      slices_in_access_unit += 1U;
      if (slices_in_access_unit > 1U) return false;
    }
    cursor = payload_start + 1U;
  }
  return access_units > 0 && slices_in_access_unit == 1U;
}

static bool start_h264_decoder(const char *fs_path) {
  esp_h264_dec_cfg_sw_t config = {
    .pic_type = ESP_H264_RAW_FMT_I420,
  };
  reset_h264_decoder();
  esp_h264_err_t err = esp_h264_dec_sw_new(&config, &g_h264_decoder);
  if (err != ESP_H264_ERR_OK || !g_h264_decoder) {
    ESP_LOGW(TAG, "P4 H264 decoder create failed error=%d", (int) err);
    reset_h264_decoder();
    return false;
  }
  err = esp_h264_dec_open(g_h264_decoder);
  if (err != ESP_H264_ERR_OK) {
    ESP_LOGW(TAG, "P4 H264 decoder open failed error=%d", (int) err);
    reset_h264_decoder();
    return false;
  }
  g_h264_decoder_open = true;
  snprintf(g_h264_fs_path, sizeof(g_h264_fs_path), "%s", fs_path ? fs_path : "");
  return true;
}

static bool decode_next_h264_frame(
  const uint8_t *stream,
  size_t stream_size,
  int width,
  int height
) {
  unsigned int steps = 0;
  size_t expected_output = (size_t) width * (size_t) height * 3U / 2U;
  if (!g_h264_decoder || !g_h264_decoder_open || !stream
      || stream_size == 0 || width <= 0 || height <= 0) {
    return false;
  }
  while (g_h264_stream_offset < stream_size && steps++ < 64U) {
    size_t remaining = stream_size - g_h264_stream_offset;
    esp_h264_dec_in_frame_t input = {
      .raw_data = {
        .buffer = (uint8_t *) (stream + g_h264_stream_offset),
        .len = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t) remaining,
      },
    };
    esp_h264_dec_out_frame_t output = {0};
    UBaseType_t original_priority = uxTaskPriorityGet(NULL);
    if (original_priority < PET_P4_H264_PRIMARY_TASK_PRIORITY) {
      vTaskPrioritySet(NULL, PET_P4_H264_PRIMARY_TASK_PRIORITY);
    }
    int64_t decode_started_us = esp_timer_get_time();
    esp_h264_err_t err = esp_h264_dec_process(g_h264_decoder, &input, &output);
    g_last_decode_us += elapsed_us_clamped(decode_started_us, esp_timer_get_time());
    if (original_priority < PET_P4_H264_PRIMARY_TASK_PRIORITY) {
      vTaskPrioritySet(NULL, original_priority);
    }
    if (err != ESP_H264_ERR_OK || input.consume == 0
        || input.consume > input.raw_data.len) {
      ESP_LOGW(
        TAG,
        "P4 H264 decode failed error=%d offset=%u remaining=%u consume=%u",
        (int) err,
        (unsigned int) g_h264_stream_offset,
        (unsigned int) remaining,
        (unsigned int) input.consume
      );
      return false;
    }
    g_h264_stream_offset += input.consume;
    if (output.out_size == 0) continue;
    if (!output.outbuf || output.out_size < expected_output) {
      ESP_LOGW(
        TAG,
        "P4 H264 output invalid size=%u expected=%u",
        (unsigned int) output.out_size,
        (unsigned int) expected_output
      );
      return false;
    }
    g_h264_output = output.outbuf;
    g_h264_output_size = output.out_size;
    g_h264_decoded_frame += 1;
    return true;
  }
  return false;
}

static bool resolve_h264_i420_layout(
  int visible_w,
  int visible_h,
  uint32_t output_size,
  int *stride,
  int *storage_h
) {
  size_t luma_bytes;
  int coded_w;
  size_t coded_h;
  if (visible_w <= 0 || visible_h <= 0 || !stride || !storage_h
      || (visible_w & 1) != 0 || (visible_h & 1) != 0
      || output_size == 0 || (output_size % 3U) != 0) {
    return false;
  }
  luma_bytes = (size_t) output_size * 2U / 3U;
  coded_w = (visible_w + 15) & ~15;
  if (coded_w < visible_w || luma_bytes % (size_t) coded_w != 0) {
    return false;
  }
  coded_h = luma_bytes / (size_t) coded_w;
  if (coded_h < (size_t) visible_h || coded_h > INT_MAX
      || (coded_h & 1U) != 0) {
    return false;
  }
  *stride = coded_w;
  *storage_h = (int) coded_h;
  return true;
}

static void pack_h264_i420_rows(const pet_p4_h264_pack_job_t *job) {
  for (int y = job->row_begin; y < job->row_end; y += 1) {
    const int src_w = job->src_w;
    const uint8_t *source_y = job->plane_y
      + (size_t) y * (size_t) job->src_stride;
    const uint8_t *source_chroma = ((y & 1) == 0 ? job->plane_u : job->plane_v)
      + (size_t) (y / 2) * (size_t) (job->src_stride / 2);
    uint8_t *target = job->packed + (size_t) y * job->packed_stride;
    int x = 0;
    for (; x + 7 < src_w; x += 8) {
      uint32_t y_lo;
      uint32_t y_hi;
      uint32_t chroma;
      uint32_t packed0;
      uint32_t packed1;
      uint32_t packed2;
      memcpy(&y_lo, source_y + x, sizeof(y_lo));
      memcpy(&y_hi, source_y + x + 4, sizeof(y_hi));
      memcpy(&chroma, source_chroma + x / 2, sizeof(chroma));
      packed0 = (chroma & 0x000000ffU)
              | ((y_lo & 0x0000ffffU) << 8)
              | ((chroma & 0x0000ff00U) << 16);
      packed1 = (y_lo >> 16)
              | (chroma & 0x00ff0000U)
              | ((y_hi & 0x000000ffU) << 24);
      packed2 = ((y_hi >> 8) & 0x000000ffU)
              | ((chroma >> 16) & 0x0000ff00U)
              | (y_hi & 0xffff0000U);
      memcpy(target, &packed0, sizeof(packed0));
      memcpy(target + 4, &packed1, sizeof(packed1));
      memcpy(target + 8, &packed2, sizeof(packed2));
      target += 12;
    }
    for (; x < src_w; x += 2) {
      *target++ = source_chroma[x / 2];
      *target++ = source_y[x];
      *target++ = source_y[x + 1];
    }
  }
}

static void h264_pack_worker(void *unused) {
  (void) unused;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    pack_h264_i420_rows(&g_h264_pack_job);
    xSemaphoreGive(g_h264_pack_done);
  }
}

static bool pack_h264_i420_for_ppa(
  const uint8_t *i420,
  int src_w,
  int src_h,
  int src_stride,
  int storage_h,
  const uint8_t **packed
) {
  if (!i420 || !packed || src_w <= 0 || src_h <= 0
      || src_stride < src_w || storage_h < src_h
      || (src_w & 1) != 0 || (src_h & 1) != 0
      || (src_stride & 1) != 0 || (storage_h & 1) != 0) {
    return false;
  }
  size_t storage_luma_bytes = (size_t) src_stride * (size_t) storage_h;
  size_t packed_luma_bytes = (size_t) src_w * (size_t) storage_h;
  size_t required = packed_luma_bytes * 3U / 2U;
  if (!g_h264_ppa_input || g_h264_ppa_input_capacity < required) {
    if (g_h264_ppa_input) heap_caps_free(g_h264_ppa_input);
    g_h264_ppa_input = (uint8_t *) heap_caps_aligned_calloc(
      PET_P4_PPA_BUFFER_ALIGNMENT,
      required,
      sizeof(uint8_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA
    );
    g_h264_ppa_input_capacity = g_h264_ppa_input ? required : 0;
  }
  if (!g_h264_ppa_input) return false;

  pet_p4_h264_pack_job_t primary_job = {
    .plane_y = i420,
    .plane_u = i420 + storage_luma_bytes,
    .plane_v = i420 + storage_luma_bytes + storage_luma_bytes / 4U,
    .packed = g_h264_ppa_input,
    .packed_stride = (size_t) src_w * 3U / 2U,
    .src_w = src_w,
    .src_stride = src_stride,
    .row_begin = 0,
    .row_end = storage_h,
  };
  if (g_h264_pack_task && g_h264_pack_done && storage_h >= 2) {
    int split_row = storage_h / 2;
    g_h264_pack_job = primary_job;
    g_h264_pack_job.row_begin = split_row;
    primary_job.row_end = split_row;
    while (xSemaphoreTake(g_h264_pack_done, 0) == pdTRUE) {}
    UBaseType_t original_priority = uxTaskPriorityGet(NULL);
    if (original_priority < PET_P4_H264_PACK_TASK_PRIORITY) {
      vTaskPrioritySet(NULL, PET_P4_H264_PACK_TASK_PRIORITY);
    }
    xTaskNotifyGive(g_h264_pack_task);
    pack_h264_i420_rows(&primary_job);
    xSemaphoreTake(g_h264_pack_done, portMAX_DELAY);
    if (original_priority < PET_P4_H264_PACK_TASK_PRIORITY) {
      vTaskPrioritySet(NULL, original_priority);
    }
  } else {
    pack_h264_i420_rows(&primary_job);
  }
  *packed = g_h264_ppa_input;
  return true;
}

static void clear_framebuffer_outside_rect(int x, int y, int width, int height) {
  const size_t row_bytes = PET_P4_UI_WIDTH * sizeof(uint16_t);
  if (!g_framebuffer) return;
  if (x < 0 || y < 0 || width <= 0 || height <= 0
      || x + width > PET_P4_UI_WIDTH || y + height > PET_P4_UI_HEIGHT) {
    memset(g_framebuffer, 0, PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t));
    return;
  }
  if (y > 0) memset(g_framebuffer, 0, (size_t) y * row_bytes);
  if (y + height < PET_P4_UI_HEIGHT) {
    memset(
      g_framebuffer + (size_t) (y + height) * PET_P4_UI_WIDTH,
      0,
      (size_t) (PET_P4_UI_HEIGHT - y - height) * row_bytes
    );
  }
  if (x > 0 || x + width < PET_P4_UI_WIDTH) {
    for (int row = y; row < y + height; row += 1) {
      uint16_t *line = g_framebuffer + (size_t) row * PET_P4_UI_WIDTH;
      if (x > 0) memset(line, 0, (size_t) x * sizeof(uint16_t));
      if (x + width < PET_P4_UI_WIDTH) {
        memset(
          line + x + width,
          0,
          (size_t) (PET_P4_UI_WIDTH - x - width) * sizeof(uint16_t)
        );
      }
    }
  }
}

static bool scale_h264_frame_ppa(
  const uint8_t *packed_yuv420,
  int src_w,
  int src_h,
  int storage_h
) {
  int dst_x;
  int dst_y;
  int dst_w;
  int dst_h;
  float scale;
  if (!packed_yuv420 || !g_ppa_srm_client) {
    return false;
  }
  if (!g_framebuffer_ppa_compatible
      || !asset_fit_layout(src_w, src_h, &dst_x, &dst_y, &dst_w, &dst_h, &scale)) {
    return false;
  }
  if (dst_x != 0 || dst_y != 0
      || dst_w != PET_P4_UI_WIDTH || dst_h != PET_P4_UI_HEIGHT) {
    clear_framebuffer_outside_rect(dst_x, dst_y, dst_w, dst_h);
  }
  ppa_srm_oper_config_t scaling = {
    .in = {
      .buffer = (void *) packed_yuv420,
      .pic_w = (uint32_t) src_w,
      .pic_h = (uint32_t) storage_h,
      .block_w = (uint32_t) src_w,
      .block_h = (uint32_t) src_h,
      .block_offset_x = 0,
      .block_offset_y = 0,
      .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
      .yuv_range = PPA_COLOR_RANGE_LIMIT,
      .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT709,
    },
    .out = {
      .buffer = g_framebuffer,
      .buffer_size = PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t),
      .pic_w = PET_P4_UI_WIDTH,
      .pic_h = PET_P4_UI_HEIGHT,
      .block_offset_x = (uint32_t) dst_x,
      .block_offset_y = (uint32_t) dst_y,
      .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
    },
    .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
    .scale_x = scale,
    .scale_y = scale,
    .rgb_swap = false,
    .byte_swap = false,
    .mode = PPA_TRANS_MODE_BLOCKING,
  };
  int64_t convert_started_us = esp_timer_get_time();
  esp_err_t err = ppa_do_scale_rotate_mirror(g_ppa_srm_client, &scaling);
  g_last_h264_convert_us += elapsed_us_clamped(convert_started_us, esp_timer_get_time());
  if (err == ESP_OK) {
    if (!g_logged_h264_ppa) {
      ESP_LOGI(
        TAG,
        "P4 H264 convert=PPA source=%dx%d output=%dx%d offset=%d,%d factor=%.3f",
        src_w,
        src_h,
        dst_w,
        dst_h,
        dst_x,
        dst_y,
        (double) scale
      );
      g_logged_h264_ppa = true;
    }
    return true;
  }
  if (!g_logged_h264_ppa_fallback) {
    ESP_LOGW(TAG, "P4 H264 PPA conversion failed (%s); using CPU fallback", esp_err_to_name(err));
    g_logged_h264_ppa_fallback = true;
  }
  return false;
}

static bool h264_frame_supports_ppa_yuv420(int width, int storage_h) {
  return width > 0 && storage_h > 0
    && (width % 16) == 0
    && (storage_h % 16) == 0;
}

static uint8_t clamp_u8(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return (uint8_t) value;
}

static void scale_h264_frame_cpu(
  const uint8_t *i420,
  int src_w,
  int src_h,
  int src_stride,
  int storage_h
) {
  int dst_x;
  int dst_y;
  int dst_w;
  int dst_h;
  if (!i420 || src_stride < src_w || storage_h < src_h
      || !asset_fit_layout(src_w, src_h, &dst_x, &dst_y, &dst_w, &dst_h, NULL)) {
    return;
  }
  const uint8_t *plane_y = i420;
  size_t storage_luma_bytes = (size_t) src_stride * (size_t) storage_h;
  const uint8_t *plane_u = plane_y + storage_luma_bytes;
  const uint8_t *plane_v = plane_u + storage_luma_bytes / 4U;
  memset(g_framebuffer, 0, PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t));
  for (int y = 0; y < dst_h; y += 1) {
    int sy = y * src_h / dst_h;
    uint16_t *target = g_framebuffer + (dst_y + y) * PET_P4_UI_WIDTH + dst_x;
    for (int x = 0; x < dst_w; x += 1) {
      int sx = x * src_w / dst_w;
      int luma = (int) plane_y[sy * src_stride + sx] - 16;
      int u = (int) plane_u[(sy / 2) * (src_stride / 2) + sx / 2] - 128;
      int v = (int) plane_v[(sy / 2) * (src_stride / 2) + sx / 2] - 128;
      if (luma < 0) luma = 0;
      int red = (298 * luma + 459 * v + 128) >> 8;
      int green = (298 * luma - 55 * u - 136 * v + 128) >> 8;
      int blue = (298 * luma + 541 * u + 128) >> 8;
      target[x] = rgb565(clamp_u8(red), clamp_u8(green), clamp_u8(blue));
    }
  }
}

static bool render_h264_asset_frame(
  const pet_p4_asset_selection_t *asset,
  const char *fs_path,
  const pet_p4_asset_cache_entry_t *stream,
  unsigned long long cycle_elapsed_ms
) {
  int target_frame;
  int i420_stride;
  int i420_storage_h;
  const uint8_t *packed_yuv420 = NULL;
  if (!asset || !fs_path || !stream || asset->frames <= 0
      || asset->width <= 0 || asset->height <= 0) {
    return false;
  }
  if (strcmp(g_h264_validated_fs_path, fs_path) != 0) {
    g_h264_single_slice_valid =
      h264_stream_has_single_slice_access_units(stream->bytes, stream->size);
    snprintf(g_h264_validated_fs_path, sizeof(g_h264_validated_fs_path), "%s", fs_path);
    if (!g_h264_single_slice_valid) {
      ESP_LOGW(
        TAG,
        "P4 H264 rejected unsafe stream: expected one slice per access unit path=%s",
        fs_path
      );
    }
  }
  if (!g_h264_single_slice_valid) return false;
  target_frame = (int) (
    (cycle_elapsed_ms * (unsigned long long) asset->frames)
    / (unsigned long long) asset->duration_ms
  );
  if (target_frame >= asset->frames) target_frame = asset->frames - 1;
  if (!g_h264_decoder || strcmp(g_h264_fs_path, fs_path) != 0
      || target_frame < g_h264_decoded_frame) {
    if (!start_h264_decoder(fs_path)) return false;
  }
  unsigned int decoded_in_pass = 0;
  while (g_h264_decoded_frame < target_frame
         && decoded_in_pass < PET_P4_H264_MAX_DECODE_FRAMES_PER_RENDER) {
    if (!decode_next_h264_frame(stream->bytes, stream->size, asset->width, asset->height)) {
      return false;
    }
    decoded_in_pass += 1U;
    if ((decoded_in_pass % 8U) == 0U) vTaskDelay(1);
  }
  if (!g_h264_output
      || g_h264_output_size < (uint32_t) (asset->width * asset->height * 3 / 2)) {
    return false;
  }
  if (!resolve_h264_i420_layout(
        asset->width,
        asset->height,
        g_h264_output_size,
        &i420_stride,
        &i420_storage_h
      )) {
    ESP_LOGW(
      TAG,
      "P4 H264 output layout invalid visible=%dx%d bytes=%u",
      asset->width,
      asset->height,
      (unsigned int) g_h264_output_size
    );
    return false;
  }
  bool converted = false;
  if (h264_frame_supports_ppa_yuv420(asset->width, i420_storage_h)) {
    int64_t pack_started_us = esp_timer_get_time();
    bool packed_ok = pack_h264_i420_for_ppa(
      g_h264_output,
      asset->width,
      asset->height,
      i420_stride,
      i420_storage_h,
      &packed_yuv420
    );
    g_last_h264_pack_us += elapsed_us_clamped(pack_started_us, esp_timer_get_time());
    if (packed_ok) {
      converted = scale_h264_frame_ppa(
        packed_yuv420,
        asset->width,
        asset->height,
        i420_storage_h
      );
    }
  }
  if (!converted) {
    scale_h264_frame_cpu(
      g_h264_output,
      asset->width,
      asset->height,
      i420_stride,
      i420_storage_h
    );
  }
  if (!g_logged_h264_decoder) {
    ESP_LOGI(
      TAG,
      "P4 H264 decoder=%s output=I420 visible=%dx%d storage=%dx%d bytes=%u",
      PET_P4_H264_DECODER_MODE,
      asset->width,
      asset->height,
      i420_stride,
      i420_storage_h,
      (unsigned int) g_h264_output_size
    );
    g_logged_h264_decoder = true;
  }
  return true;
}

static bool render_asset_pet_frame(const pet_p4_runtime_state_t *state, const pet_p4_view_model_t *view, unsigned long long now_ms) {
  char fs_path[80];
  pet_p4_asset_selection_t asset;
  if (!select_asset(state, now_ms, &asset)) {
    if (!g_logged_asset_failure) {
      ESP_LOGW(TAG, "P4 asset select failed manifest_len=%u family_count=%u status=%d",
               state && state->asset_manifest_json[0] ? (unsigned int) strlen(state->asset_manifest_json) : 0,
               state ? state->asset_family_count : 0,
               view ? (int) view->status : -1);
      g_logged_asset_failure = true;
    }
    return false;
  }
  if (!pet_p4_asset_fs_path(asset.path, fs_path, sizeof(fs_path))) {
    if (!g_logged_asset_failure) {
      ESP_LOGW(TAG, "P4 asset path rejected: %s", asset.path);
      g_logged_asset_failure = true;
    }
    return false;
  }
  if (state && g_asset_cache_revision != state->asset_revision) {
    reset_h264_decoder();
    clear_asset_stream_cache();
    g_asset_cache_revision = state->asset_revision;
    g_decoder_fs_path[0] = '\0';
    g_h264_validated_fs_path[0] = '\0';
    g_h264_single_slice_valid = false;
  }
  if (strcmp(g_decoder_fs_path, fs_path) != 0) {
    snprintf(g_decoder_fs_path, sizeof(g_decoder_fs_path), "%s", fs_path);
    note_asset_decode_success();
  }
  if (asset_decode_is_in_backoff()) {
    return false;
  }

  size_t stream_size = asset.stream_bytes;
  if (stream_size == 0 || stream_size > PET_P4_ASSET_CACHE_MAX_FILE_BYTES) return false;
  pet_p4_asset_cache_entry_t *stream = load_asset_stream_cache(asset.path, fs_path, stream_size);
  if (!stream) {
    if (!g_logged_asset_failure) {
      ESP_LOGW(
        TAG,
        "P4 asset PSRAM cache unavailable logical=%s fs=%s bytes=%u",
        asset.path,
        fs_path,
        (unsigned int) stream_size
      );
      g_logged_asset_failure = true;
    }
    note_asset_decode_failure();
    return false;
  }

  unsigned long long playback_duration_ms = asset.duration_ms;
  if (playback_duration_ms == 0 && asset.frame_duration_ms > 0) {
    playback_duration_ms = (unsigned long long) asset.frame_duration_ms * (unsigned long long) asset.frames;
  } else if (playback_duration_ms == 0 && asset.fps > 0) {
    playback_duration_ms = ((unsigned long long) asset.frames * 1000ULL) / (unsigned long long) asset.fps;
  }
  if (playback_duration_ms == 0) playback_duration_ms = PET_P4_RENDER_INTERVAL_MS * (unsigned long long) asset.frames;
  unsigned long long playback_elapsed_ms = now_ms >= g_behavior.selected_since_ms
    ? now_ms - g_behavior.selected_since_ms : 0;
  unsigned long long cycle_elapsed_ms = playback_elapsed_ms % playback_duration_ms;
  g_last_decode_us = 0;
  if (asset.codec == PET_P4_ASSET_CODEC_H264) {
    bool rendered = render_h264_asset_frame(&asset, fs_path, stream, cycle_elapsed_ms);
    if (!rendered) {
      if (!g_logged_asset_failure) {
        ESP_LOGW(
          TAG,
          "P4 H264 render failed logical=%s frame=%d offset=%u bytes=%u",
          asset.path,
          g_h264_decoded_frame,
          (unsigned int) g_h264_stream_offset,
          (unsigned int) stream->size
        );
        g_logged_asset_failure = true;
      }
      note_asset_decode_failure();
      return false;
    }
    if (!g_logged_asset || strcmp(g_rendered_family, asset.family) != 0) {
      ESP_LOGI(TAG, "rendering P4 family=%s asset=%s", asset.family, asset.path);
      snprintf(g_rendered_family, sizeof(g_rendered_family), "%s", asset.family);
      g_logged_asset = true;
    }
    note_asset_decode_success();
    return true;
  }
  if (g_h264_decoder) reset_h264_decoder();
  int frame_index = (int) (
    (cycle_elapsed_ms * (unsigned long long) asset.frames) / playback_duration_ms
  );
  if (frame_index >= asset.frames) frame_index = asset.frames - 1;
  size_t offset = 0;
  for (int i = 0; i < frame_index; i += 1) offset += asset.frame_sizes[i];
  uint32_t frame_size = asset.frame_sizes[frame_index];
  if (frame_size == 0 || frame_size > PET_P4_MAX_JPEG_BYTES
      || offset > stream->size || frame_size > stream->size - offset) {
    return false;
  }
  memcpy(g_jpeg_input, stream->bytes + offset, frame_size);

  int decoded_width = 0;
  int decoded_height = 0;
  int decoded_stride = 0;
  esp_err_t hardware_decode_err = ESP_ERR_INVALID_STATE;
  JRESULT decode_err = JDR_OK;
  int64_t decode_started_us = esp_timer_get_time();
  bool decoded_with_hardware = decode_jpeg_frame_hardware(
    g_jpeg_input,
    frame_size,
    &decoded_width,
    &decoded_height,
    &decoded_stride,
    &hardware_decode_err
  );
  bool decoded = decoded_with_hardware;
  if (!decoded) {
    decoded = decode_jpeg_frame_tjpgd(
      g_jpeg_input,
      frame_size,
      &decoded_width,
      &decoded_height,
      &decode_err
    );
    if (decoded) decoded_stride = decoded_width;
    if (decoded && g_jpeg_decoder && !g_logged_hardware_fallback) {
      ESP_LOGW(
        TAG,
        "P4 hardware JPEG decode failed (%s); using TJPGD fallback",
        esp_err_to_name(hardware_decode_err)
      );
      g_logged_hardware_fallback = true;
    }
  }
  g_last_decode_us = elapsed_us_clamped(decode_started_us, esp_timer_get_time());
  if (!decoded) {
    if (!g_logged_asset_failure) {
      ESP_LOGW(
        TAG,
        "P4 JPEG decode failed logical=%s frame=%d size=%u out_cap=%u hardware=%s tjpgd=%d",
        asset.path,
        frame_index,
        frame_size,
        (unsigned int) g_jpeg_output_capacity,
        esp_err_to_name(hardware_decode_err),
        (int) decode_err
      );
      g_logged_asset_failure = true;
    }
    note_asset_decode_failure();
    return false;
  }
  if (decoded_with_hardware && !g_logged_hardware_decoder) {
    ESP_LOGI(
      TAG,
      "P4 JPEG decoder=hardware output=RGB565 size=%dx%d",
      decoded_width,
      decoded_height
    );
    g_logged_hardware_decoder = true;
  }

  int src_w = decoded_width;
  int src_h = decoded_height;
  if (src_w <= 0 || src_h <= 0) return false;
  if (src_w > PET_P4_ASSET_WIDTH) src_w = PET_P4_ASSET_WIDTH;
  if (src_h > PET_P4_ASSET_HEIGHT) src_h = PET_P4_ASSET_HEIGHT;
  scale_asset_frame_to_display(src_w, src_h, decoded_stride);
  if (!g_logged_asset || strcmp(g_rendered_family, asset.family) != 0) {
    ESP_LOGI(TAG, "rendering P4 family=%s asset=%s", asset.family, asset.path);
    snprintf(g_rendered_family, sizeof(g_rendered_family), "%s", asset.family);
    g_logged_asset = true;
  }
  note_asset_decode_success();
  return true;
}

static void draw_bubble(const pet_p4_view_model_t *view, unsigned long long now_ms) {
  if (view && !view->show_bubble) return;
  bool compact = view ? view->compact_bubble : true;
  if (!compact) {
    int x = PET_P4_SESSION_CARD_X;
    int y = PET_P4_SESSION_CARD_BOTTOM - PET_P4_SESSION_CARD_HEIGHT;
    int w = PET_P4_SESSION_CARD_WIDTH;
    int h = PET_P4_SESSION_CARD_HEIGHT;
    const char *title = view && view->title ? view->title : "";
    const char *body = view && view->body ? view->body : "";
    uint16_t ink = rgb565(28, 31, 31);
    uint16_t muted = rgb565(92, 98, 96);
    uint16_t selection = rgb565(25, 151, 99);
    pet_p4_view_status_t status = view ? view->status : PET_P4_VIEW_STATUS_IDLE;
    draw_session_card_panel(x, y, w, h, PET_P4_SESSION_CARD_RADIUS, true, selection);
    fill_round_rect(x + 10, y + 14, 7, h - 28, 2, selection);
    draw_text_line_vcenter(title, x + 30, y + 10, 38, w - 82, ink, 2, true);
    if (body[0]) draw_card_body_lines(body, x + 30, y + 50, w - 60, muted);
    draw_status_marker(status, now_ms, x + w - 26, y + 26);
    return;
  }

  int h = 56;
  int x = 20;
  int y = PET_P4_UI_HEIGHT - h - 20;
  int w = 220;
  int text_x = x + 24;
  uint16_t ink = rgb565(28, 31, 31);
  uint16_t outline = rgb565(216, 221, 218);
  uint16_t accent = view_status_color(view ? view->status : PET_P4_VIEW_STATUS_IDLE);
  const char *body = view && view->body && view->body[0] ? view->body : (view ? view->title : "");
  w = 72 + utf8_text_width(body, -1, 2);
  if (w < 160) w = 160;
  if (w > 420) w = 420;
  if (w & 1) w += 1;
  draw_home_white_panel(x, y, w, h, 16, outline);
  fill_round_rect(x + 10, y + 14, 4, h - 28, 2, accent);
  draw_text_line(
    body,
    text_x,
    text_y_in_box(body, 2, y, h) + PET_P4_COMPACT_TEXT_OPTICAL_Y,
    w - 72,
    ink,
    2,
    true
  );
  draw_status_marker(
    view ? view->status : PET_P4_VIEW_STATUS_IDLE,
    now_ms,
    x + w - 28,
    y + 28
  );
}

static void draw_touch_feedback(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!pet_p4_state_touch_feedback_active(state, now_ms)) return;
  int x = state->local_touch_x;
  int y = state->local_touch_y;
  uint16_t white = rgb565(255, 255, 255);
  uint16_t teal = rgb565(0, 170, 162);
  fill_ellipse(x, y, 13, 13, white);
  fill_ellipse(x, y, 9, 9, teal);
  fill_ellipse(x, y, 3, 3, white);
}

static void draw_page_indicator(const char *page) {
  int count = 2;
  int current = strcmp(page, "components") == 0 || strcmp(page, "app") == 0 ? 1 : 0;
  int x = PET_P4_PAGE_INDICATOR_X;
  int y = PET_P4_PAGE_INDICATOR_Y;
  uint16_t background = rgb565(11, 14, 13);
  uint16_t active = rgb565(255, 163, 31);
  uint16_t inactive = rgb565(111, 119, 112);
  blend_round_rect(
    x,
    y,
    PET_P4_PAGE_INDICATOR_WIDTH,
    PET_P4_PAGE_INDICATOR_HEIGHT,
    10,
    5,
    7,
    6,
    220
  );
  for (int i = 0; i < count; i += 1) {
    int radius = i == current ? 4 : 2;
    fill_ellipse(
      x + 18 + i * 16,
      y + PET_P4_PAGE_INDICATOR_HEIGHT / 2,
      radius,
      radius,
      i == current ? active : inactive
    );
  }
}

static void draw_text_right(
  const char *text,
  int right_x,
  int y,
  int max_px,
  uint16_t color,
  int scale
);
static void draw_text_center(
  const char *text,
  int center_x,
  int y,
  int max_px,
  uint16_t color,
  int scale
);

static void render_component_center_page(void) {
  const uint16_t background = rgb565(5, 7, 6);
  const uint16_t panel = rgb565(14, 18, 16);
  const uint16_t panel_outline = rgb565(52, 58, 53);
  const uint16_t selected_panel = rgb565(74, 44, 16);
  const uint16_t selected_outline = rgb565(255, 163, 31);
  const uint16_t ivory = rgb565(250, 241, 204);
  const uint16_t muted = rgb565(126, 133, 126);
  const uint16_t orange = rgb565(255, 163, 31);
  const size_t count = pet_p4_miniapp_catalog_count();
  const size_t selected = pet_p4_miniapp_catalog_selected();
  const size_t visible_rows = 5;
  size_t start = 0;

  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, background);
  draw_text_line("组件中心", 28, 16, 248, orange, 2, true);
  char count_text[24];
  snprintf(count_text, sizeof(count_text), "已安装 %u 个", (unsigned int) count);
  draw_text_right(count_text, 612, 22, 160, muted, 1);
  fill_rect(28, 58, 584, 1, panel_outline);

  if (count == 0) {
    draw_text_center("暂无已安装组件", 320, 170, 560, ivory, 2);
    draw_text_center("请在客户端安装组件", 320, 218, 560, muted, 1);
  } else {
    if (selected >= visible_rows) start = selected - visible_rows + 1;
    if (start + visible_rows > count && count > visible_rows) start = count - visible_rows;
    size_t end = count < start + visible_rows ? count : start + visible_rows;
    for (size_t index = start; index < end; index += 1) {
      pet_p4_miniapp_catalog_entry_t item = {0};
      if (!pet_p4_miniapp_catalog_get(index, &item)) continue;
      const bool focused = index == selected;
      const int row = (int) (index - start);
      const int y = 76 + row * 62;
      fill_round_rect_outline(
        28,
        y,
        584,
        52,
        9,
        focused ? selected_panel : panel,
        focused ? selected_outline : panel_outline
      );
      if (focused) fill_ellipse(48, y + 26, 5, 5, orange);
      draw_text_line(
        item.title[0] ? item.title : item.widget_id,
        66,
        y + 8,
        390,
        focused ? ivory : orange,
        2,
        true
      );
      if (item.active) {
        fill_round_rect(500, y + 8, 86, 22, 11, orange);
        draw_text_center(
          "已打开",
          543,
          text_y_in_box("已打开", 1, y + 8, 22),
          72,
          background,
          1
        );
      }
    }
  }

  fill_round_rect_outline(28, 408, 584, 44, 9, panel, panel_outline);
  const char *hint = "SW3返回，SW1进入";
  draw_text_center(
    hint,
    320,
    text_y_in_box(hint, 1, 408, 44),
    548,
    muted,
    1
  );
}

static void draw_connection_banner(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!state) return;
  bool connected = pet_p4_state_host_connected(state, now_ms);
  const char *label = NULL;
  uint16_t dot = rgb565(0, 170, 162);
  if (connected && state->asset_family_count > 0) return;
  if (connected) {
    label = "正在同步宠物形象";
  } else if (state->desktop_device_id[0]) {
    label = "客户端未连接";
    dot = rgb565(224, 79, 72);
  } else if (state->native_usb_mounted || state->host_last_seen_ms > 0) {
    label = "正在连接 Pet Manager";
    dot = rgb565(242, 168, 45);
  } else {
    label = "连接 Pet Manager 以开始";
    dot = rgb565(242, 168, 45);
  }

  uint16_t background = rgb565(11, 14, 13);
  uint16_t text = rgb565(250, 241, 204);
  int banner_h = 32;
  int banner_w = utf8_text_width(label, -1, 1) + 48;
  if (banner_w < 200) banner_w = 200;
  if (banner_w > 272) banner_w = 272;
  if (banner_w & 1) banner_w += 1;
  int banner_x = PET_P4_UI_WIDTH - banner_w - 20;
  int banner_y = 18;
  fill_round_rect(banner_x, banner_y, banner_w, banner_h, 12, background);
  fill_ellipse(banner_x + 16, banner_y + banner_h / 2, 4, 4, dot);
  draw_text_line_vcenter(
    label,
    banner_x + 30,
    banner_y,
    banner_h,
    banner_w - 44,
    text,
    1,
    true
  );
}

static void draw_text_right(
  const char *text,
  int right_x,
  int y,
  int max_px,
  uint16_t color,
  int scale
) {
  int width = utf8_text_width(text ? text : "", -1, scale);
  if (width > max_px) {
    draw_text_line(text, right_x - max_px, y, max_px, color, scale, true);
    return;
  }
  draw_text_line(text, right_x - width, y, max_px, color, scale, false);
}

static void draw_text_center(
  const char *text,
  int center_x,
  int y,
  int max_px,
  uint16_t color,
  int scale
) {
  int width = utf8_text_width(text ? text : "", -1, scale);
  if (width > max_px) {
    draw_text_line(text, center_x - max_px / 2, y, max_px, color, scale, true);
    return;
  }
  draw_text_line(text, center_x - width / 2, y, max_px, color, scale, false);
}

static int fitted_text_scale(const char *text, int max_px, int preferred, int minimum) {
  for (int scale = preferred; scale > minimum; scale -= 1) {
    if (utf8_text_width(text ? text : "", -1, scale) <= max_px) return scale;
  }
  return minimum;
}

static uint16_t miniapp_state_color(const char *state) {
  if (state && (strstr(state, "error") || strstr(state, "failed"))) return rgb565(235, 89, 82);
  if (state && (strstr(state, "pause") || strstr(state, "wait"))) return rgb565(255, 163, 31);
  return rgb565(85, 193, 133);
}

static uint16_t session_status_color(const char *status) {
  if (status && strcmp(status, "done") == 0) return view_status_color(PET_P4_VIEW_STATUS_DONE);
  if (status && strcmp(status, "error") == 0) return view_status_color(PET_P4_VIEW_STATUS_ERROR);
  if (status && strcmp(status, "waiting_user") == 0) return view_status_color(PET_P4_VIEW_STATUS_WAITING);
  if (status && strcmp(status, "speaking") == 0) return view_status_color(PET_P4_VIEW_STATUS_WORKING);
  if (status && strcmp(status, "working") == 0) return view_status_color(PET_P4_VIEW_STATUS_WORKING);
  return view_status_color(PET_P4_VIEW_STATUS_IDLE);
}

static pet_p4_view_status_t session_view_status(const char *status) {
  if (status && strcmp(status, "done") == 0) return PET_P4_VIEW_STATUS_DONE;
  if (status && strcmp(status, "error") == 0) return PET_P4_VIEW_STATUS_ERROR;
  if (status && strcmp(status, "waiting_user") == 0) return PET_P4_VIEW_STATUS_WAITING;
  if (status && strcmp(status, "speaking") == 0) return PET_P4_VIEW_STATUS_WORKING;
  if (status && strcmp(status, "working") == 0) return PET_P4_VIEW_STATUS_WORKING;
  return PET_P4_VIEW_STATUS_IDLE;
}

static void draw_voice_waveform(int x, int y, int width, uint16_t color) {
  static const int heights[] = {4, 7, 11, 16, 9, 5, 8, 13, 6, 4};
  int cursor = x;
  int segments = (int) (sizeof(heights) / sizeof(heights[0]));
  int step = width / segments;
  for (int i = 0; i < segments; i += 1) {
    int next = cursor + step;
    int y1 = y + (i > 0 ? heights[i - 1] : 0);
    int y2 = y + heights[i];
    draw_line(cursor, y1, next, y2, color);
    cursor = next;
  }
}

static void draw_session_queue_uncached(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms,
  bool draw_working_markers
) {
  if (!state || state->session_queue_count == 0) return;

  uint16_t ink = rgb565(28, 31, 31);
  uint16_t muted = rgb565(92, 98, 96);
  uint16_t orange = rgb565(255, 163, 31);
  uint16_t card_outline = rgb565(216, 221, 218);
  uint16_t selection = rgb565(25, 151, 99);
  unsigned int selected = state->current_session_index > 0
    ? state->current_session_index - 1
    : 0;
  if (selected >= state->session_queue_count) selected = 0;
  unsigned int visible = state->session_queue_count < 3 ? state->session_queue_count : 3;
  unsigned int start = 0;
  if (state->session_queue_count > visible) {
    if (selected >= state->session_queue_count - 1) start = state->session_queue_count - visible;
    else if (selected > 0) start = selected - 1;
  }
  int block_height = (int) visible * PET_P4_SESSION_CARD_HEIGHT
    + ((int) visible - 1) * PET_P4_SESSION_CARD_GAP;
  int first_y = PET_P4_SESSION_CARD_BOTTOM - block_height;
  for (unsigned int row = 0; row < visible; row += 1) {
    unsigned int item_index = start + row;
    const pet_p4_session_queue_item_t *item = &state->session_queue[item_index];
    bool is_selected = item_index == selected;
    int x = PET_P4_SESSION_CARD_X;
    int y = first_y + (int) row * (PET_P4_SESSION_CARD_HEIGHT + PET_P4_SESSION_CARD_GAP);
    int w = PET_P4_SESSION_CARD_WIDTH;
    int h = PET_P4_SESSION_CARD_HEIGHT;
    uint16_t status = session_status_color(item->state);
    draw_session_card_panel(
      x,
      y,
      w,
      h,
      PET_P4_SESSION_CARD_RADIUS,
      is_selected,
      card_outline
    );
    fill_round_rect(x + 10, y + 14, is_selected ? 7 : 4, h - 28, 2, is_selected ? selection : status);
    pet_p4_view_status_t marker_status = session_view_status(item->state);
    if (draw_working_markers || marker_status != PET_P4_VIEW_STATUS_WORKING) {
      draw_status_marker(marker_status, now_ms, x + w - 26, y + 26);
    }
    draw_text_line_vcenter(item->title, x + 30, y + 10, 38, w - 82, ink, 2, true);

    if (is_selected && state->session_voice_active) {
      draw_voice_waveform(x + 30, y + 62, 230, orange);
      fill_ellipse(x + 288, y + 75, 7, 11, ink);
      fill_rect(x + 286, y + 84, 5, 7, ink);
      draw_line(x + 279, y + 80, x + 279, y + 87, ink);
      draw_line(x + 279, y + 87, x + 297, y + 87, ink);
      draw_line(x + 297, y + 87, x + 297, y + 80, ink);
      draw_text_line_vcenter("LISTENING", x + 324, y + 73, 30, 180, muted, 1, true);
    } else {
      const char *body = item->content[0] ? item->content : item->state;
      draw_card_body_lines(body, x + 30, y + 50, w - 60, muted);
    }
  }
}

static uint64_t session_cache_hash_bytes(uint64_t hash, const void *bytes, size_t size) {
  const uint8_t *cursor = (const uint8_t *) bytes;
  for (size_t i = 0; i < size; i += 1) {
    hash ^= cursor[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static uint64_t session_cache_hash_text(uint64_t hash, const char *text) {
  const char *value = text ? text : "";
  return session_cache_hash_bytes(hash, value, strlen(value) + 1);
}

static uint64_t session_overlay_signature(const pet_p4_runtime_state_t *state) {
  uint64_t hash = 1469598103934665603ULL;
  hash = session_cache_hash_bytes(hash, &state->session_queue_count, sizeof(state->session_queue_count));
  hash = session_cache_hash_bytes(hash, &state->current_session_index, sizeof(state->current_session_index));
  hash = session_cache_hash_bytes(hash, &state->session_voice_active, sizeof(state->session_voice_active));
  for (unsigned int i = 0; i < state->session_queue_count && i < PET_P4_SESSION_QUEUE_MAX; i += 1) {
    const pet_p4_session_queue_item_t *item = &state->session_queue[i];
    hash = session_cache_hash_text(hash, item->id);
    hash = session_cache_hash_text(hash, item->title);
    hash = session_cache_hash_text(hash, item->content);
    hash = session_cache_hash_text(hash, item->state);
  }
  return hash;
}

static void scan_session_overlay_cache_runs(void) {
  memset(g_session_overlay_row_run_count, 0, sizeof(g_session_overlay_row_run_count));
  for (int y = 0; y < PET_P4_UI_HEIGHT; y += 1) {
    const uint16_t *row = g_session_overlay_cache + y * PET_P4_UI_WIDTH;
    int x = 0;
    while (x < PET_P4_UI_WIDTH) {
      while (x < PET_P4_UI_WIDTH && row[x] == PET_P4_SESSION_CACHE_SENTINEL) x += 1;
      if (x >= PET_P4_UI_WIDTH) break;
      int start = x;
      while (x < PET_P4_UI_WIDTH && row[x] != PET_P4_SESSION_CACHE_SENTINEL) x += 1;
      uint8_t run = g_session_overlay_row_run_count[y];
      if (run >= PET_P4_SESSION_CACHE_MAX_RUNS) {
        uint8_t last = PET_P4_SESSION_CACHE_MAX_RUNS - 1;
        g_session_overlay_row_run_end[y][last] = (uint16_t) x;
        continue;
      }
      g_session_overlay_row_run_start[y][run] = (uint16_t) start;
      g_session_overlay_row_run_end[y][run] = (uint16_t) x;
      g_session_overlay_row_run_count[y] = run + 1;
    }
  }
}

static bool rebuild_session_overlay_cache(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms,
  uint64_t signature
) {
  if (!g_session_overlay_cache && !g_session_overlay_cache_allocation_failed) {
    g_session_overlay_cache = (uint16_t *) heap_caps_malloc(
      PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!g_session_overlay_cache) {
      g_session_overlay_cache_allocation_failed = true;
      ESP_LOGW(TAG, "P4 session overlay cache unavailable; using direct drawing");
    }
  }
  if (!g_session_overlay_cache) return false;

  uint16_t *output = g_framebuffer;
  g_framebuffer = g_session_overlay_cache;
  for (size_t i = 0; i < (size_t) PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT; i += 1) {
    g_session_overlay_cache[i] = PET_P4_SESSION_CACHE_SENTINEL;
  }
  draw_session_queue_uncached(state, now_ms, false);
  g_framebuffer = output;
  scan_session_overlay_cache_runs();
  g_session_overlay_signature = signature;
  g_session_overlay_cache_valid = true;
  memset(&g_render_perf, 0, sizeof(g_render_perf));
  ESP_LOGI(
    TAG,
    "P4 session overlay cache rebuilt sessions=%u selected=%u",
    state->session_queue_count,
    state->current_session_index
  );
  return true;
}

static void copy_session_overlay_cache(void) {
  for (int y = 0; y < PET_P4_UI_HEIGHT; y += 1) {
    const uint16_t *source = g_session_overlay_cache + y * PET_P4_UI_WIDTH;
    uint16_t *target = g_framebuffer + y * PET_P4_UI_WIDTH;
    for (uint8_t run = 0; run < g_session_overlay_row_run_count[y]; run += 1) {
      uint16_t start = g_session_overlay_row_run_start[y][run];
      uint16_t end = g_session_overlay_row_run_end[y][run];
      memcpy(target + start, source + start, (size_t) (end - start) * sizeof(uint16_t));
    }
  }
}

static void draw_session_queue_working_markers(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  unsigned int selected = state->current_session_index > 0
    ? state->current_session_index - 1
    : 0;
  if (selected >= state->session_queue_count) selected = 0;
  unsigned int visible = state->session_queue_count < 3 ? state->session_queue_count : 3;
  unsigned int start = 0;
  if (state->session_queue_count > visible) {
    if (selected >= state->session_queue_count - 1) start = state->session_queue_count - visible;
    else if (selected > 0) start = selected - 1;
  }
  int block_height = (int) visible * PET_P4_SESSION_CARD_HEIGHT
    + ((int) visible - 1) * PET_P4_SESSION_CARD_GAP;
  int first_y = PET_P4_SESSION_CARD_BOTTOM - block_height;
  for (unsigned int row = 0; row < visible; row += 1) {
    const pet_p4_session_queue_item_t *item = &state->session_queue[start + row];
    if (session_view_status(item->state) != PET_P4_VIEW_STATUS_WORKING) continue;
    int y = first_y + (int) row * (PET_P4_SESSION_CARD_HEIGHT + PET_P4_SESSION_CARD_GAP);
    draw_status_marker(
      PET_P4_VIEW_STATUS_WORKING,
      now_ms,
      PET_P4_SESSION_CARD_X + PET_P4_SESSION_CARD_WIDTH - 26,
      y + 26
    );
  }
}

static void draw_session_queue(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!state || state->session_queue_count == 0) {
    g_session_overlay_cache_valid = false;
    return;
  }
  uint64_t signature = session_overlay_signature(state);
  if ((!g_session_overlay_cache_valid || signature != g_session_overlay_signature)
      && !rebuild_session_overlay_cache(state, now_ms, signature)) {
    draw_session_queue_uncached(state, now_ms, true);
    return;
  }
  copy_session_overlay_cache();
  draw_session_queue_working_markers(state, now_ms);
}

typedef struct {
  uint16_t background;
  uint16_t background_alt;
  uint16_t surface;
  uint16_t ink;
  uint16_t primary;
  uint16_t secondary;
  uint16_t light;
  uint16_t shadow;
} miniapp_pixel_palette_t;

typedef struct {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint16_t color;
} miniapp_tool_accent_t;

static miniapp_pixel_palette_t miniapp_pixel_palette(const char *name) {
  miniapp_pixel_palette_t palette = {
    .background = rgb565(90, 215, 255),
    .background_alt = rgb565(112, 225, 255),
    .surface = rgb565(255, 243, 184),
    .ink = rgb565(43, 35, 103),
    .primary = rgb565(255, 79, 139),
    .secondary = rgb565(255, 216, 61),
    .light = rgb565(255, 253, 240),
    .shadow = rgb565(103, 80, 164),
  };
  if (name && strcmp(name, "sunset") == 0) {
    palette.background = rgb565(255, 155, 115);
    palette.background_alt = rgb565(255, 176, 132);
    palette.surface = rgb565(255, 234, 182);
    palette.ink = rgb565(85, 39, 95);
    palette.primary = rgb565(233, 63, 134);
    palette.secondary = rgb565(255, 210, 63);
    palette.shadow = rgb565(132, 63, 114);
  } else if (name && strcmp(name, "mint") == 0) {
    palette.background = rgb565(120, 230, 189);
    palette.background_alt = rgb565(151, 239, 204);
    palette.surface = rgb565(244, 255, 208);
    palette.ink = rgb565(23, 63, 95);
    palette.primary = rgb565(255, 95, 119);
    palette.secondary = rgb565(255, 209, 102);
    palette.shadow = rgb565(44, 122, 120);
  } else if (name && strcmp(name, "arcade") == 0) {
    palette.background = rgb565(70, 59, 143);
    palette.background_alt = rgb565(83, 70, 163);
    palette.surface = rgb565(255, 240, 168);
    palette.ink = rgb565(32, 25, 79);
    palette.primary = rgb565(255, 79, 184);
    palette.secondary = rgb565(60, 242, 210);
    palette.shadow = rgb565(33, 25, 73);
  } else if (name && strcmp(name, "ocean") == 0) {
    palette.background = rgb565(24, 70, 106);
    palette.background_alt = rgb565(39, 101, 139);
    palette.surface = rgb565(224, 246, 249);
    palette.ink = rgb565(9, 35, 52);
    palette.primary = rgb565(36, 196, 212);
    palette.secondary = rgb565(255, 196, 71);
    palette.light = rgb565(248, 254, 255);
    palette.shadow = rgb565(18, 67, 84);
  } else if (name && strcmp(name, "forest") == 0) {
    palette.background = rgb565(74, 132, 92);
    palette.background_alt = rgb565(106, 161, 111);
    palette.surface = rgb565(246, 244, 220);
    palette.ink = rgb565(29, 55, 35);
    palette.primary = rgb565(255, 111, 70);
    palette.secondary = rgb565(248, 207, 89);
    palette.light = rgb565(255, 253, 240);
    palette.shadow = rgb565(43, 87, 61);
  } else if (name && strcmp(name, "ember") == 0) {
    palette.background = rgb565(58, 25, 31);
    palette.background_alt = rgb565(91, 33, 36);
    palette.surface = rgb565(255, 232, 193);
    palette.ink = rgb565(35, 17, 21);
    palette.primary = rgb565(255, 91, 54);
    palette.secondary = rgb565(255, 190, 69);
    palette.light = rgb565(255, 249, 235);
    palette.shadow = rgb565(103, 38, 42);
  } else if (name && strcmp(name, "mono") == 0) {
    palette.background = rgb565(214, 219, 218);
    palette.background_alt = rgb565(232, 235, 233);
    palette.surface = rgb565(255, 255, 255);
    palette.ink = rgb565(27, 31, 30);
    palette.primary = rgb565(12, 126, 117);
    palette.secondary = rgb565(238, 95, 76);
    palette.light = rgb565(250, 252, 251);
    palette.shadow = rgb565(112, 122, 119);
  }
  return palette;
}

static miniapp_tool_accent_t miniapp_tool_accent(const char *palette_name) {
  miniapp_tool_accent_t accent = {
    .red = 246,
    .green = 183,
    .blue = 60,
    .color = rgb565(246, 183, 60),
  };
  if (palette_name && strcmp(palette_name, "sunset") == 0) {
    accent.red = 255;
    accent.green = 102;
    accent.blue = 92;
  } else if (palette_name && strcmp(palette_name, "mint") == 0) {
    accent.red = 87;
    accent.green = 217;
    accent.blue = 255;
  } else if (palette_name && strcmp(palette_name, "arcade") == 0) {
    accent.red = 170;
    accent.green = 140;
    accent.blue = 255;
  } else if (palette_name && strcmp(palette_name, "ocean") == 0) {
    accent.red = 36;
    accent.green = 196;
    accent.blue = 212;
  } else if (palette_name && strcmp(palette_name, "forest") == 0) {
    accent.red = 255;
    accent.green = 111;
    accent.blue = 70;
  } else if (palette_name && strcmp(palette_name, "ember") == 0) {
    accent.red = 255;
    accent.green = 91;
    accent.blue = 54;
  } else if (palette_name && strcmp(palette_name, "mono") == 0) {
    accent.red = 12;
    accent.green = 126;
    accent.blue = 117;
  }
  accent.color = rgb565(accent.red, accent.green, accent.blue);
  return accent;
}

static void draw_miniapp_pixel_panel(
  int x,
  int y,
  int width,
  int height,
  const miniapp_pixel_palette_t *palette
) {
  fill_rect(x + 6, y + 6, width, height, palette->shadow);
  fill_rect(x, y, width, height, palette->ink);
  fill_rect(x + 4, y + 4, width - 8, height - 8, palette->surface);
}

static void draw_miniapp_pixel_game_panel(
  int x,
  int y,
  int width,
  int height,
  const miniapp_pixel_palette_t *palette
) {
  fill_rect(x, y, width, height, palette->ink);
  fill_rect(x + 2, y + 2, width - 4, height - 4, palette->background_alt);
}

static void draw_miniapp_clean_card(
  int x,
  int y,
  int width,
  int height,
  uint16_t surface,
  const miniapp_pixel_palette_t *palette
) {
  fill_round_rect(x + 3, y + 5, width, height, 16, palette->shadow);
  fill_round_rect(x, y, width, height, 16, surface);
  if (width > 36 && height > 12) {
    fill_round_rect(x + 14, y + 3, width - 28, 3, 2, palette->light);
  }
}

static void draw_miniapp_pixel_sprite(
  const char *name,
  int x,
  int y,
  int cell,
  const miniapp_pixel_palette_t *palette
) {
  static const char *const target[] = {
    "000222000", "002111200", "021222120", "212111212", "212131212",
    "212111212", "021222120", "002111200", "000222000",
  };
  static const char *const trophy[] = {
    "001111100", "211111112", "221111122", "022111220", "002111200",
    "000111000", "000111000", "001111100", "011111110",
  };
  static const char *const star[] = {
    "000100000", "000100000", "010111010", "001111100", "111141111",
    "001111100", "010111010", "000101000", "000100000",
  };
  static const char *const bolt[] = {
    "000111000", "001111000", "011110000", "111111000", "001111100",
    "000111110", "000011100", "000011000", "000010000",
  };
  static const char *const coffee[] = {
    "001010000", "000101000", "011111020", "011111022", "011111020",
    "011111000", "001111000", "000000000", "011111110",
  };
  static const char *const timer[] = {
    "000111000", "001111100", "000111000", "011111110", "110000011",
    "110030011", "110000011", "011111110", "001111100",
  };
  static const char *const droplet[] = {
    "000010000", "000111000", "001111100", "011111110", "111111111",
    "111111111", "011111110", "001111100", "000111000",
  };
  static const char *const gauge[] = {
    "001111100", "011111110", "110000011", "100000001", "100030001",
    "100010001", "110111011", "011111110", "001111100",
  };
  static const char *const blocks[] = {
    "110022200", "110002000", "000002000", "033333000", "000330000",
    "000330044", "000000044", "111122220", "111122220",
  };
  static const char *const snake[] = {
    "000000000", "011100000", "010100000", "010111000", "000001000",
    "000001330", "000000303", "000222300", "000222000",
  };
  static const char *const flappy[] = {
    "000000000", "000000000", "000011100", "000111110", "001114440",
    "011112220", "001111100", "000011000", "000000000",
  };
  static const char *const mole_ready[] = {
    "000000000000000", "000000000000000", "000000000000000",
    "000000000000000", "000000000000000", "000000000000000",
    "033000330003300", "333303333033330", "033000330003300",
  };
  static const char *const mole_left[] = {
    "100100000000000", "111100000000000", "141400000000000",
    "122100000000000", "011000000000000", "011000000000000",
    "033000330003300", "333303333033330", "033000330003300",
  };
  static const char *const mole_center[] = {
    "000001001000000", "000001111000000", "000001414000000",
    "000001221000000", "000000110000000", "000000110000000",
    "033000330003300", "333303333033330", "033000330003300",
  };
  static const char *const mole_right[] = {
    "000000000010010", "000000000011110", "000000000014140",
    "000000000012210", "000000000001100", "000000000001100",
    "033000330003300", "333303333033330", "033000330003300",
  };
  const char *const *rows = target;
  int columns = 9;
  if (name && strcmp(name, "trophy") == 0) rows = trophy;
  else if (name && strcmp(name, "star") == 0) rows = star;
  else if (name && strcmp(name, "bolt") == 0) rows = bolt;
  else if (name && strcmp(name, "coffee") == 0) rows = coffee;
  else if (name && strcmp(name, "timer") == 0) rows = timer;
  else if (name && strcmp(name, "droplet") == 0) rows = droplet;
  else if (name && strcmp(name, "gauge") == 0) rows = gauge;
  else if (name && strcmp(name, "blocks") == 0) rows = blocks;
  else if (name && strcmp(name, "snake") == 0) rows = snake;
  else if (name && strcmp(name, "flappy") == 0) rows = flappy;
  else if (name && strcmp(name, "mole-ready") == 0) {
    rows = mole_ready;
    columns = 15;
  } else if (name && strcmp(name, "mole-left") == 0) {
    rows = mole_left;
    columns = 15;
  } else if (name && strcmp(name, "mole-center") == 0) {
    rows = mole_center;
    columns = 15;
  } else if (name && strcmp(name, "mole-right") == 0) {
    rows = mole_right;
    columns = 15;
  }
  for (int row = 0; row < 9; row += 1) {
    for (int column = 0; column < columns; column += 1) {
      uint16_t color;
      switch (rows[row][column]) {
        case '1': color = palette->primary; break;
        case '2': color = palette->secondary; break;
        case '3': color = palette->ink; break;
        case '4': color = palette->light; break;
        default: continue;
      }
      fill_rect(x + column * cell, y + row * cell, cell, cell, color);
    }
  }
}

static uint16_t miniapp_game_tone_color(
  uint8_t tone,
  const miniapp_pixel_palette_t *palette
) {
  if (tone == 1) return palette->primary;
  if (tone == 2) return palette->secondary;
  if (tone == 3) return palette->ink;
  if (tone == 4) return palette->light;
  return palette->background_alt;
}

static bool draw_miniapp_entity_sprite(
  const pet_p4_game_entity_t *entity,
  const pet_p4_miniapp_sprite_pack_t *sprites,
  int x,
  int y,
  int width,
  int height
) {
  if (!entity || !sprites || entity->sprite_index < 0
      || entity->sprite_index >= sprites->count) return false;
  const pet_p4_miniapp_sprite_t *sprite = &sprites->items[entity->sprite_index];
  const size_t frame_bytes = (size_t) sprite->frame_width * sprite->frame_height * 3U;
  const uint64_t now_ms = (uint64_t) esp_timer_get_time() / 1000ULL;
  const uint8_t frame = sprite->frames > 1
    ? (uint8_t) (((now_ms * sprite->fps) / 1000ULL) % sprite->frames)
    : 0;
  const size_t frame_offset = (size_t) sprite->data_offset + (size_t) frame * frame_bytes;
  if (sprite->frame_width == 0 || sprite->frame_height == 0
      || frame_offset + frame_bytes > sprites->data_length) return false;
  for (int target_y = 0; target_y < height; target_y += 1) {
    const int source_y = target_y * sprite->frame_height / height;
    for (int target_x = 0; target_x < width; target_x += 1) {
      const int source_x = target_x * sprite->frame_width / width;
      const size_t offset = frame_offset
        + ((size_t) source_y * sprite->frame_width + (size_t) source_x) * 3U;
      const uint16_t color = (uint16_t) sprites->pixels[offset]
        | ((uint16_t) sprites->pixels[offset + 1] << 8U);
      const uint8_t alpha = sprites->pixels[offset + 2];
      if (alpha == 0) continue;
      if (alpha == 255) {
        put_px(x + target_x, y + target_y, color);
      } else {
        const uint8_t red = (uint8_t) (((color >> 11) & 0x1fU) * 255U / 31U);
        const uint8_t green = (uint8_t) (((color >> 5) & 0x3fU) * 255U / 63U);
        const uint8_t blue = (uint8_t) ((color & 0x1fU) * 255U / 31U);
        blend_px(x + target_x, y + target_y, red, green, blue, alpha);
      }
    }
  }
  return true;
}

static void draw_miniapp_game_entity(
  const pet_p4_game_entity_t *entity,
  const pet_p4_miniapp_sprite_pack_t *sprites,
  int grid_x,
  int grid_y,
  int cell,
  bool clean_style,
  const miniapp_pixel_palette_t *palette
) {
  if (!entity || !entity->active) return;
  int inset = clean_style ? 2 : 1;
  int x = grid_x + entity->x * cell + inset;
  int y = grid_y + entity->y * cell + inset;
  int width = entity->width * cell - inset * 2;
  int height = entity->height * cell - inset * 2;
  if (width < 3 || height < 3) return;
  int cx = x + width / 2;
  int cy = y + height / 2;
  uint16_t color = miniapp_game_tone_color(entity->tone, palette);
  uint16_t detail = entity->tone == 4 ? palette->ink : palette->light;

  if (draw_miniapp_entity_sprite(entity, sprites, x, y, width, height)) return;

  switch (entity->shape) {
    case PET_P4_GAME_SHAPE_PLAYER_SHIP: {
      int shoulder_y = y + height * 2 / 3;
      fill_triangle(cx, y, x + width - 1, y + height - 1, x, y + height - 1, color);
      fill_rect(
        cx - (width > 12 ? 2 : 1),
        y + height / 4,
        width > 12 ? 5 : 3,
        height * 3 / 4,
        color
      );
      fill_ellipse(
        cx,
        shoulder_y - height / 4,
        width > 12 ? 2 : 1,
        height > 12 ? 3 : 1,
        detail
      );
      fill_rect(cx - 1, y + height - 2, 3, 2, palette->secondary);
      break;
    }
    case PET_P4_GAME_SHAPE_ENEMY_SHIP:
      fill_triangle(x, y, x + width - 1, y, cx, y + height - 1, color);
      fill_rect(
        cx - (width > 12 ? 2 : 1),
        y,
        width > 12 ? 5 : 3,
        height * 3 / 4,
        color
      );
      fill_ellipse(
        cx,
        y + height / 3,
        width > 12 ? 2 : 1,
        height > 12 ? 3 : 1,
        detail
      );
      break;
    case PET_P4_GAME_SHAPE_BULLET: {
      int bullet_width = width / 3;
      if (bullet_width < 3) bullet_width = 3;
      if (bullet_width > 9) bullet_width = 9;
      fill_round_rect(
        cx - bullet_width / 2,
        y,
        bullet_width,
        height,
        bullet_width / 2,
        color
      );
      if (bullet_width >= 5 && height >= 7) fill_rect(cx, y + 2, 1, height - 4, detail);
      break;
    }
    case PET_P4_GAME_SHAPE_STAR: {
      int rx = width / 2;
      int ry = height / 2;
      fill_triangle(cx, y, cx + rx / 3, cy, cx - rx / 3, cy, color);
      fill_triangle(cx, y + height - 1, cx - rx / 3, cy, cx + rx / 3, cy, color);
      fill_triangle(x, cy, cx, cy - ry / 3, cx, cy + ry / 3, color);
      fill_triangle(x + width - 1, cy, cx, cy + ry / 3, cx, cy - ry / 3, color);
      fill_ellipse(cx, cy, rx / 3 + 1, ry / 3 + 1, detail);
      break;
    }
    case PET_P4_GAME_SHAPE_PADDLE: {
      int radius = (width < height ? width : height) / 3;
      if (radius < 2) radius = 2;
      fill_round_rect(x, y, width, height, radius, color);
      if (width > height && height >= 6) fill_rect(x + 3, y + 2, width - 6, 2, detail);
      break;
    }
    case PET_P4_GAME_SHAPE_BALL:
    case PET_P4_GAME_SHAPE_CIRCLE:
      fill_ellipse(cx, cy, width / 2, height / 2, color);
      if (width >= 8 && height >= 8) {
        fill_ellipse(cx - width / 5, cy - height / 5, 2, 2, detail);
      }
      break;
    case PET_P4_GAME_SHAPE_CAPSULE: {
      int radius = (width < height ? width : height) / 2;
      fill_round_rect(x, y, width, height, radius, color);
      if (width >= 8 && height >= 8) {
        fill_round_rect(x + width / 5, y + height / 5, width / 3, height / 5, 2, detail);
      }
      break;
    }
    case PET_P4_GAME_SHAPE_TRIANGLE:
      fill_triangle(cx, y, x + width - 1, y + height - 1, x, y + height - 1, color);
      break;
    case PET_P4_GAME_SHAPE_DIAMOND:
      fill_triangle(cx, y, x + width - 1, cy, cx, cy, color);
      fill_triangle(x, cy, cx, cy, cx, y + height - 1, color);
      fill_triangle(cx, cy, x + width - 1, cy, cx, y + height - 1, color);
      break;
    case PET_P4_GAME_SHAPE_HEART:
      fill_ellipse(x + width / 3, y + height / 3, width / 4, height / 4, color);
      fill_ellipse(x + width * 2 / 3, y + height / 3, width / 4, height / 4, color);
      fill_triangle(x, y + height / 3, x + width - 1, y + height / 3, cx, y + height - 1, color);
      break;
    case PET_P4_GAME_SHAPE_CLOUD:
      fill_round_rect(x, y + height / 3, width, height * 2 / 3, height / 4, color);
      fill_ellipse(x + width / 3, y + height / 3, width / 4, height / 3, color);
      fill_ellipse(x + width * 2 / 3, y + height / 3, width / 3, height / 3, color);
      break;
    case PET_P4_GAME_SHAPE_COIN:
      fill_ellipse(cx, cy, width / 2, height / 2, color);
      fill_ellipse(cx, cy, width / 3, height / 3, detail);
      fill_ellipse(cx, cy, width / 5, height / 5, color);
      break;
    case PET_P4_GAME_SHAPE_CHARACTER:
      fill_ellipse(cx, y + height / 4, width / 4, height / 4, color);
      fill_round_rect(x + width / 4, y + height / 2, width / 2, height / 2, width / 6, color);
      if (width >= 8 && height >= 8) {
        fill_ellipse(cx - width / 10, y + height / 5, 1, 1, detail);
        fill_ellipse(cx + width / 10, y + height / 5, 1, 1, detail);
      }
      break;
    case PET_P4_GAME_SHAPE_RECT:
    default:
      if (clean_style) {
        int radius = (width < height ? width : height) / 5;
        if (radius < 2) radius = 2;
        if (radius > 6) radius = 6;
        fill_round_rect(x, y, width, height, radius, color);
      } else {
        fill_rect(x, y, width, height, color);
      }
      break;
  }
}

static void draw_miniapp_game_grid(
  const pet_p4_game_frame_t *game,
  const pet_p4_miniapp_sprite_pack_t *sprites,
  int panel_x,
  int panel_y,
  int panel_width,
  int panel_height,
  const miniapp_pixel_palette_t *palette,
  bool clean_style
) {
  if (!game || game->kind == PET_P4_GAME_NONE || game->width == 0 || game->height == 0) return;
  bool flappy_game = game->kind == PET_P4_GAME_FLAPPY;
  uint16_t grid_outline = flappy_game ? rgb565(22, 53, 79) : palette->ink;
  uint16_t empty = flappy_game ? rgb565(143, 216, 247) : palette->background_alt;
  uint16_t primary = flappy_game ? rgb565(55, 168, 73) : palette->primary;
  uint16_t secondary = flappy_game ? rgb565(155, 228, 91) : palette->secondary;
  uint16_t ink = flappy_game ? rgb565(243, 165, 31) : palette->ink;
  uint16_t light = flappy_game ? rgb565(255, 241, 166) : palette->light;
  int cell = (panel_width - 4) / game->width;
  int height_cell = (panel_height - 4) / game->height;
  if (height_cell < cell) cell = height_cell;
  if (cell < 6) cell = 6;
  int max_cell = (
    game->kind == PET_P4_GAME_SNAKE || game->kind == PET_P4_GAME_FLAPPY
  ) ? 28 : 21;
  if (cell > max_cell) cell = max_cell;
  int grid_width = cell * game->width;
  int grid_height = cell * game->height;
  int grid_x = panel_x + (panel_width - grid_width) / 2;
  int grid_y = panel_y + (panel_height - grid_height) / 2;
  if (clean_style) {
    fill_round_rect(grid_x - 2, grid_y - 2, grid_width + 4, grid_height + 4, 8, empty);
  } else {
    fill_rect(grid_x - 2, grid_y - 2, grid_width + 4, grid_height + 4, grid_outline);
  }
  for (int y = 0; y < game->height; y += 1) {
    for (int x = 0; x < game->width; x += 1) {
      int index = y * game->width + x;
      uint8_t tone = game->cells[index];
      uint16_t color = empty;
      if (tone == 1) color = primary;
      else if (tone == 2) color = secondary;
      else if (tone == 3) color = ink;
      else if (tone == 4) color = light;
      if (!clean_style || tone != 0) {
        int gap = clean_style ? 0 : 1;
        fill_rect(
          grid_x + x * cell + gap,
          grid_y + y * cell + gap,
          cell - gap * 2,
          cell - gap * 2,
          color
        );
      }
    }
  }
  if (game->kind == PET_P4_GAME_BOUNDED) {
    for (int index = 0; index < game->entity_count; index += 1) {
      draw_miniapp_game_entity(
        &game->entities[index], sprites, grid_x, grid_y, cell, clean_style, palette
      );
    }
  }
}

static bool miniapp_tool_badge_visible(const char *badge) {
  return badge && badge[0]
    && strcmp(badge, "0") != 0
    && strcmp(badge, "—") != 0
    && strcmp(badge, "-") != 0;
}

static void draw_miniapp_tool_keyboard_icon(
  int x,
  int y,
  uint16_t accent,
  uint16_t background
) {
  fill_rect(x, y, 22, 14, accent);
  fill_rect(x + 2, y + 2, 18, 10, background);
  for (int key_y = 0; key_y < 2; key_y += 1) {
    for (int key_x = 0; key_x < 4; key_x += 1) {
      fill_rect(x + 4 + key_x * 4, y + 4 + key_y * 4, 2, 2, accent);
    }
  }
}

static void render_pixel_tool_miniapp_page(const pet_p4_miniapp_view_t *app) {
  const uint16_t background = rgb565(5, 7, 6);
  const uint16_t panel = rgb565(11, 15, 14);
  const uint16_t line = rgb565(34, 41, 37);
  const uint16_t ink = rgb565(243, 241, 233);
  const uint16_t muted = rgb565(143, 152, 147);
  const uint16_t progress_background = rgb565(32, 37, 34);
  const uint16_t shadow = rgb565(0, 0, 0);
  const miniapp_tool_accent_t accent = miniapp_tool_accent(app->visual_palette);
  const char *title = app->title[0] ? app->title : app->widget_id;
  const bool show_badge = miniapp_tool_badge_visible(app->badge);
  const bool show_progress = app->progress_percent >= 0;
  const bool show_footer = app->footer[0] != '\0';

  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, background);
  for (int y = 0; y < PET_P4_UI_HEIGHT; y += 10) {
    fill_rect(0, y, PET_P4_UI_WIDTH, 1, rgb565(8, 10, 9));
  }
  fill_rect(26, 0, 96, 4, accent.color);

  fill_rect(30, 28, 50, 50, shadow);
  fill_rect(26, 24, 50, 50, accent.color);
  fill_rect(28, 26, 46, 46, background);
  blend_rect(28, 26, 46, 46, accent.red, accent.green, accent.blue, 33);
  miniapp_pixel_palette_t icon_palette = {
    .background = background,
    .background_alt = background,
    .surface = background,
    .ink = accent.color,
    .primary = accent.color,
    .secondary = accent.color,
    .light = accent.color,
    .shadow = shadow,
  };
  draw_miniapp_pixel_sprite(app->visual_sprite, 38, 36, 3, &icon_palette);
  draw_text_line(title, 90, 30, 188, ink, 2, true);

  int badge_width = 0;
  int badge_x = 612;
  if (show_badge) {
    badge_width = utf8_text_width(app->badge, -1, 1) + 22;
    if (badge_width < 44) badge_width = 44;
    if (badge_width > 82) badge_width = 82;
    badge_x = 612 - badge_width;
    fill_round_rect(badge_x, 28, badge_width, 42, 21, accent.color);
    draw_text_center(
      app->badge,
      badge_x + badge_width / 2,
      text_y_in_box(app->badge, 1, 28, 42),
      badge_width - 12,
      background,
      1
    );
  }
  if (app->headline[0]) {
    int status_right = show_badge ? badge_x - 14 : 612;
    int status_width = status_right - 370;
    if (status_width > 36) {
      draw_text_right(app->headline, status_right, 39, status_width, accent.color, 1);
    }
  }

  int content_bottom = 460;
  int footer_y = -1;
  int progress_y = -1;
  if (show_footer) {
    footer_y = content_bottom - 26;
    content_bottom = footer_y - 10;
  }
  if (show_progress) {
    progress_y = content_bottom - 36;
    content_bottom = progress_y - 10;
  }

  bool show_metric = app->metric_label[0] || app->metric_value[0];
  const int metric_x = 26;
  const int metric_y = 94;
  const int metric_width = 588;
  const int metric_height = content_bottom - metric_y;
  if (show_metric && metric_height > 80) {
    fill_rect(metric_x, metric_y, metric_width, metric_height, line);
    fill_rect(metric_x + 2, metric_y + 2, metric_width - 4, metric_height - 4, panel);
    fill_rect(metric_x + 2, metric_y + 2, 6, metric_height - 4, accent.color);

    if (app->metric_label[0]) {
      draw_miniapp_pixel_sprite(app->visual_sprite, 52, metric_y + 24, 1, &icon_palette);
      draw_text_line(app->metric_label, 70, metric_y + 20, 500, muted, 1, true);
    }

    const char *metric_value = app->metric_value[0] ? app->metric_value : "—";
    int value_max_width = app->metric_unit[0] ? 430 : 520;
    int value_scale = fitted_text_scale(metric_value, value_max_width, 5, 2);
    int value_width = utf8_text_width(metric_value, -1, value_scale);
    int value_box_y = metric_y + (app->metric_label[0] ? 58 : 28);
    int value_box_height = metric_height - (app->metric_label[0] ? 74 : 44);
    if (value_box_height < 42) value_box_height = 42;
    int value_y = text_y_in_box(metric_value, value_scale, value_box_y, value_box_height);
    draw_text_line(metric_value, 52, value_y, value_max_width, ink, value_scale, true);
    if (app->metric_unit[0]) {
      int unit_x = 52 + value_width + 14;
      if (unit_x > 520) unit_x = 520;
      draw_text_line(
        app->metric_unit,
        unit_x,
        value_y + (value_scale - 1) * 12,
        82,
        muted,
        1,
        true
      );
    }
  }

  if (show_progress) {
    char percent[12];
    snprintf(percent, sizeof(percent), "%d%%", app->progress_percent);
    draw_text_line(
      app->progress_label[0] ? app->progress_label : "进度",
      26,
      progress_y,
      420,
      muted,
      1,
      true
    );
    draw_text_right(percent, 614, progress_y, 90, ink, 1);
    fill_rect(26, progress_y + 26, 588, 8, progress_background);
    int progress_width = (int) ((uint32_t) 588U * (uint32_t) app->progress_percent / 100U);
    if (progress_width > 0) fill_rect(26, progress_y + 26, progress_width, 8, accent.color);
  }

  if (show_footer) {
    draw_miniapp_tool_keyboard_icon(26, footer_y + 4, accent.color, background);
    draw_text_line(app->footer, 58, footer_y + 2, 556, muted, 1, true);
  }
}

static void render_pixel_miniapp_page(const pet_p4_miniapp_view_t *app) {
  miniapp_pixel_palette_t palette = miniapp_pixel_palette(app->visual_palette);
  memset(&g_miniapp_sprites, 0, sizeof(g_miniapp_sprites));
  (void) pet_p4_miniapp_get_sprites(&g_miniapp_sprites);
  bool clean_style = strcmp(app->visual_style, "clean") == 0;
  bool native_game = app->game.kind != PET_P4_GAME_NONE;
  bool blocks_game = app->game.kind == PET_P4_GAME_BLOCKS;
  bool native_progress = native_game && app->progress_percent >= 0;
  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, palette.background);
  if (clean_style) {
    blend_rect(0, 0, PET_P4_UI_WIDTH, 168, 255, 255, 255, 18);
    blend_rect(0, 360, PET_P4_UI_WIDTH, 120, 0, 0, 0, 10);
    fill_round_rect(28, 0, 108, 7, 4, palette.primary);
  } else {
    for (int y = 0; y < PET_P4_UI_HEIGHT; y += 32) {
      for (int x = 0; x < PET_P4_UI_WIDTH; x += 32) {
        uint16_t color = (((x / 32) + (y / 32)) & 1)
          ? palette.background_alt : palette.background;
        fill_rect(x, y, 32, 32, color);
      }
    }
  }

  if (clean_style) {
    draw_miniapp_clean_card(28, 18, 584, 62, palette.surface, &palette);
  } else {
    draw_miniapp_pixel_panel(28, 18, 584, 62, &palette);
  }
  draw_text_line(app->title[0] ? app->title : app->widget_id,
                 44, 33, 360, palette.ink, 2, true);
  const char *time_text = native_game && app->headline[0]
    ? app->headline
    : (app->note[0] ? app->note : app->eyebrow);
  if (time_text[0]) {
    if (clean_style) fill_round_rect(452, 32, 140, 32, 16, palette.secondary);
    else fill_rect(452, 32, 140, 32, palette.secondary);
    draw_text_center(time_text, 522, text_y_in_box(time_text, 1, 32, 32),
                     128, palette.ink, 1);
  }

  bool scoreboard_first = !native_game && strcmp(app->visual_layout, "scoreboard") == 0;
  int play_x = scoreboard_first ? 248 : 28;
  int play_w = native_game ? 462 : (scoreboard_first ? 364 : 430);
  int score_x = native_game ? 504 : (scoreboard_first ? 28 : 470);
  int score_w = native_game ? 108 : (scoreboard_first ? 202 : 142);
  int stage_y = native_game ? (blocks_game && !native_progress ? 84 : 88) : 104;
  int stage_h = native_game
    ? (native_progress ? 298 : (blocks_game ? 350 : 318))
    : 248;
  int score_h = native_game ? 132 : (scoreboard_first ? stage_h : 160);
  if (native_game) {
    if (clean_style) {
      draw_miniapp_clean_card(
        play_x, stage_y, play_w, stage_h, palette.background_alt, &palette
      );
    } else {
      draw_miniapp_pixel_game_panel(play_x, stage_y, play_w, stage_h, &palette);
    }
  } else {
    if (clean_style) {
      draw_miniapp_clean_card(play_x, stage_y, play_w, stage_h, palette.surface, &palette);
    } else {
      draw_miniapp_pixel_panel(play_x, stage_y, play_w, stage_h, &palette);
    }
  }
  if (clean_style) {
    draw_miniapp_clean_card(score_x, stage_y, score_w, score_h, palette.surface, &palette);
  } else {
    draw_miniapp_pixel_panel(score_x, stage_y, score_w, score_h, &palette);
  }

  const char *headline_text = app->headline[0] ? app->headline : "READY";
  if (native_game) {
    draw_miniapp_game_grid(
      &app->game,
      &g_miniapp_sprites,
      play_x + 2,
      stage_y + 2,
      play_w - 4,
      stage_h - 4,
      &palette,
      clean_style
    );
  } else {
    if (app->eyebrow[0]) {
      draw_text_center(app->eyebrow, play_x + play_w / 2, 120, play_w - 24,
                       palette.ink, 1);
    }
    bool wide_sprite = strncmp(app->visual_sprite, "mole-", 5) == 0;
    int sprite_cell = wide_sprite ? 14 : 16;
    int sprite_size = sprite_cell * (wide_sprite ? 15 : 9);
    draw_miniapp_pixel_sprite(
      app->visual_sprite,
      play_x + (play_w - sprite_size) / 2,
      wide_sprite ? 144 : 140,
      sprite_cell,
      &palette
    );
    draw_text_center(headline_text,
                     play_x + play_w / 2,
                     text_y_in_box(headline_text, 2, 302, 38),
                     play_w - 32,
                     palette.primary,
                     2);
  }

  const char *metric_label = app->metric_label[0] ? app->metric_label : "SCORE";
  bool has_badge = app->badge[0] != '\0';
  int badge_size = native_game ? 26 : 30;
  int score_label_x = score_x + 12;
  int score_label_y = stage_y + 14;
  int score_label_w = score_w - 24 - (has_badge ? badge_size + 8 : 0);
  if (clean_style) {
    fill_round_rect(score_label_x, score_label_y, score_label_w, 26, 8, palette.ink);
  } else {
    fill_rect(score_label_x, score_label_y, score_label_w, 26, palette.ink);
  }
  draw_text_center(metric_label,
                   score_label_x + score_label_w / 2,
                   text_y_in_box(metric_label, 1, score_label_y, 26),
                   score_label_w - 8,
                   palette.light,
                   1);
  const char *metric_value = app->metric_value[0] ? app->metric_value : "0";
  int value_y = stage_y + (native_game ? 48 : (scoreboard_first ? 62 : 52));
  int value_h = native_game ? 58 : (scoreboard_first ? 102 : 62);
  int value_max_scale = native_game ? 3 : (scoreboard_first ? 5 : 4);
  int value_scale = fitted_text_scale(metric_value, score_w - 24, value_max_scale, 2);
  draw_text_center(metric_value,
                   score_x + score_w / 2,
                   text_y_in_box(metric_value, value_scale, value_y, value_h),
                   score_w - 24,
                   palette.primary,
                   value_scale);
  if (app->metric_unit[0]) {
    draw_text_center(app->metric_unit, score_x + score_w / 2,
                     text_y_in_box(app->metric_unit, 1, stage_y + score_h - 30, 24),
                     score_w - 24, palette.ink, 1);
  }
  if (has_badge) {
    int badge_x = score_x + score_w - badge_size - 10;
    int badge_y = stage_y + 12;
    if (clean_style) {
      fill_ellipse(
        badge_x + badge_size / 2,
        badge_y + badge_size / 2,
        badge_size / 2,
        badge_size / 2,
        palette.secondary
      );
    } else {
      fill_rect(badge_x, badge_y, badge_size, badge_size, palette.secondary);
    }
    draw_text_center(app->badge, badge_x + badge_size / 2,
                     text_y_in_box(app->badge, 1, badge_y, badge_size),
                     badge_size - 4, palette.ink, 1);
  }

  int progress_y = native_game ? 394 : 374;
  if (app->progress_percent >= 0) {
    int progress_width = (int) ((uint32_t) 584U * (uint32_t) app->progress_percent / 100U);
    if (clean_style) fill_round_rect(28, progress_y, 584, 14, 7, palette.shadow);
    else fill_rect(28, progress_y, 584, 14, palette.ink);
    if (progress_width > 0) {
      if (clean_style) {
        fill_round_rect(
          32,
          progress_y + 4,
          progress_width > 576 ? 576 : progress_width,
          6,
          3,
          palette.primary
        );
      } else {
        fill_rect(
          32,
          progress_y + 4,
          progress_width > 576 ? 576 : progress_width,
          6,
          palette.primary
        );
      }
    }
  }

  bool maximized_blocks = blocks_game && !native_progress;
  int footer_y = maximized_blocks ? 442 : (native_game ? 414 : 406);
  int footer_h = maximized_blocks ? 26 : (native_game ? 46 : 54);
  if (clean_style) {
    fill_round_rect_outline(
      28, footer_y, 584, footer_h, 8, palette.surface, palette.shadow
    );
  } else {
    draw_miniapp_pixel_panel(28, footer_y, 584, footer_h, &palette);
  }
  if (app->footer[0]) {
    draw_text_center(app->footer, 320,
                     text_y_in_box(app->footer, native_game ? 1 : 2,
                                   footer_y + 4, footer_h - 8),
                     548, palette.ink, native_game ? 1 : 2);
  }
}


static void render_miniapp_page(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  pet_p4_miniapp_view_t snapshot = {0};
  bool available = pet_p4_miniapp_get_view(&snapshot);
  const pet_p4_miniapp_view_t *app = available ? &snapshot : NULL;
  uint16_t background = rgb565(5, 7, 6);
  uint16_t panel = rgb565(11, 14, 13);
  uint16_t panel_outline = rgb565(52, 58, 53);
  uint16_t ivory = rgb565(250, 241, 204);
  uint16_t soft_ivory = rgb565(225, 241, 231);
  uint16_t muted = rgb565(126, 133, 126);
  uint16_t divider = rgb565(63, 69, 64);
  uint16_t orange = rgb565(255, 163, 31);
  uint16_t green = rgb565(85, 193, 133);
  uint16_t progress_bg = rgb565(38, 43, 39);
  uint16_t badge_ink = background;
  uint16_t footer_color = green;
  uint16_t page_muted = muted;
  bool pixel_tool = app && app->active
    && strcmp(app->visual_style, "pixel") == 0
    && strcmp(app->visual_layout, "tool") == 0;
  bool clean_game = app && app->active
    && strcmp(app->visual_style, "clean") == 0
    && app->game.kind != PET_P4_GAME_NONE;
  if (app && app->active && app->visual_palette[0]
      && strcmp(app->visual_style, "pixel") != 0) {
    miniapp_pixel_palette_t palette = miniapp_pixel_palette(app->visual_palette);
    bool arcade_page = strcmp(app->visual_palette, "arcade") == 0;
    background = palette.background;
    panel = palette.surface;
    panel_outline = palette.ink;
    ivory = palette.ink;
    soft_ivory = arcade_page ? palette.light : palette.ink;
    muted = palette.shadow;
    page_muted = arcade_page ? palette.secondary : palette.shadow;
    divider = page_muted;
    orange = palette.primary;
    green = palette.secondary;
    progress_bg = palette.background_alt;
    badge_ink = palette.ink;
    footer_color = soft_ivory;
  }

  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, background);
  if (!app || !app->active) {
    draw_text_line("MINI APP", 28, 24, 240, green, 1, false);
    draw_text_line("NO APP INSTALLED", 28, 90, 584, ivory, 3, true);
    draw_text_line("INSTALL FROM PET MANAGER", 28, 156, 584, muted, 1, true);
    fill_rect(28, 210, 584, 1, divider);
    return;
  }
  if ((strcmp(app->visual_style, "pixel") == 0 || clean_game) && !pixel_tool) {
    render_pixel_miniapp_page(app);
    return;
  }
  if (pixel_tool) {
    render_pixel_tool_miniapp_page(app);
    return;
  }

  bool scoreboard = strcmp(app->visual_layout, "scoreboard") == 0;
  const char *title = app->title[0] ? app->title : app->widget_id;
  uint16_t state_color = miniapp_state_color(app->state);
  int state_width = utf8_text_width(app->state, -1, 1) + 34;
  if (state_width < 76) state_width = 76;
  if (state_width > 154) state_width = 154;
  if (state_width & 1) state_width += 1;
  int state_x = 612 - state_width;
  int title_x = 28;
  int title_max_px = PET_P4_PAGE_INDICATOR_X - title_x - PET_P4_PAGE_INDICATOR_TITLE_GAP;

  draw_text_line(title, title_x, 16, title_max_px, soft_ivory, 2, true);
  bool show_app_state = state
    && pet_p4_state_host_connected(state, now_ms)
    && state->asset_family_count > 0;
  if (app->state[0] && show_app_state) {
    fill_round_rect_outline(state_x, 18, state_width, 32, 16, panel, panel_outline);
    fill_ellipse(state_x + 14, 34, 4, 4, state_color);
    draw_text_center(
      app->state,
      state_x + state_width / 2 + 6,
      text_y_in_box(app->state, 1, 18, 32),
      state_width - 30,
      muted,
      1
    );
  }
  if (app->eyebrow[0]) {
    draw_text_line(app->eyebrow, 28, 60, 584, page_muted, 1, true);
  }
  if (app->headline[0]) {
    draw_text_line(app->headline, 28, 86, 584, orange, 2, true);
  }

  fill_round_rect_outline(28, 138, 584, 236, 8, panel, panel_outline);
  if (app->metric_label[0]) {
    if (scoreboard) {
      draw_text_center(app->metric_label, 320,
                       text_y_in_box(app->metric_label, 1, 154, 30),
                       520, muted, 1);
    } else {
      draw_text_line(app->metric_label, 50, 160, 370, muted, 1, true);
    }
  }
  const char *metric_value = app->metric_value[0] ? app->metric_value : "-";
  int value_max_width = scoreboard ? 520 : 390;
  int value_scale = fitted_text_scale(metric_value, value_max_width, scoreboard ? 5 : 4, 2);
  int value_width = utf8_text_width(metric_value, -1, value_scale);
  if (value_width > value_max_width) value_width = value_max_width;
  if (scoreboard) {
    draw_text_center(metric_value, 320,
                     text_y_in_box(metric_value, value_scale, 180, 86),
                     value_max_width, ivory, value_scale);
    if (app->metric_unit[0]) {
      draw_text_center(app->metric_unit, 320,
                       text_y_in_box(app->metric_unit, 1, 266, 26),
                       520, muted, 1);
    }
  } else {
    draw_text_line(metric_value, 50, 176, 390, ivory, value_scale, true);
    if (app->metric_unit[0]) {
      int unit_x = 50 + value_width + 14;
      if (unit_x > 470) unit_x = 470;
      draw_text_line(app->metric_unit, unit_x, 176 + (value_scale - 1) * 12, 104, muted, 1, true);
    }
  }
  if (app->badge[0]) {
    fill_ellipse(562, 190, 24, 24, green);
    draw_text_center(
      app->badge,
      562,
      text_y_in_box(app->badge, 1, 166, 48),
      36,
      badge_ink,
      1
    );
  }

  if (app->note[0]) {
    if (scoreboard) {
      draw_text_center(app->note, 320,
                       text_y_in_box(app->note, 1, 296, 28),
                       520, muted, 1);
    } else {
      draw_text_line(app->note, 50, 280, 540, muted, 1, true);
    }
  }
  if (app->progress_percent >= 0) {
    int progress_width = (int) ((uint32_t) 540U * (uint32_t) app->progress_percent / 100U);
    char percent[12];
    snprintf(percent, sizeof(percent), "%d%%", app->progress_percent);
    if (app->progress_label[0]) draw_text_line(app->progress_label, 50, 320, 380, muted, 1, true);
    draw_text_right(percent, 590, 320, 92, ivory, 1);
    fill_round_rect(50, 348, 540, 8, 4, progress_bg);
    if (progress_width > 0 && progress_width < 8) progress_width = 8;
    if (progress_width > 0) fill_round_rect(50, 348, progress_width, 8, 4, orange);
  }

  for (int x = 28; x < 612; x += 12) fill_rect(x, 420, 6, 1, divider);
  if (app->footer[0]) {
    draw_text_center(
      app->footer,
      320,
      text_y_in_box(app->footer, 2, 424, 44),
      560,
      footer_color,
      2
    );
  }
}

static void draw_boot_diagnostic(unsigned long long now_ms) {
  if (now_ms > P4_BOOT_DIAGNOSTIC_MS) return;

  uint16_t white = rgb565(255, 255, 255);
  uint16_t orange = rgb565(255, 120, 0);
  int border = 6;
  int marker_w = 96;
  int marker_h = 18;
  fill_rect(0, 0, PET_P4_UI_WIDTH, border, white);
  fill_rect(0, PET_P4_UI_HEIGHT - border, PET_P4_UI_WIDTH, border, white);
  fill_rect(0, 0, border, PET_P4_UI_HEIGHT, white);
  fill_rect(PET_P4_UI_WIDTH - border, 0, border, PET_P4_UI_HEIGHT, white);
  fill_rect(16, 16, marker_w, marker_h, orange);
  fill_rect(PET_P4_UI_WIDTH - marker_w - 16, 16, marker_w, marker_h, orange);
  fill_rect(16, PET_P4_UI_HEIGHT - marker_h - 16, marker_w, marker_h, orange);
  fill_rect(PET_P4_UI_WIDTH - marker_w - 16, PET_P4_UI_HEIGHT - marker_h - 16, marker_w, marker_h, orange);
}

static esp_err_t rotate_landscape_to_panel(void) {
  if (!g_framebuffer || !g_native_framebuffer) return ESP_ERR_INVALID_STATE;
  /* Every producer and the ST7701S panel use the same RGB565 element order. */
  if (g_ppa_srm_client
      && g_framebuffer_ppa_compatible
      && g_native_framebuffer_ppa_compatible) {
    ppa_srm_oper_config_t rotation = {
      .in = {
        .buffer = g_framebuffer,
        .pic_w = PET_P4_UI_WIDTH,
        .pic_h = PET_P4_UI_HEIGHT,
        .block_w = PET_P4_UI_WIDTH,
        .block_h = PET_P4_UI_HEIGHT,
        .block_offset_x = 0,
        .block_offset_y = 0,
        .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
      },
      .out = {
        .buffer = g_native_framebuffer,
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
        .pic_w = BSP_LCD_H_RES,
        .pic_h = BSP_LCD_V_RES,
        .block_offset_x = 0,
        .block_offset_y = 0,
        .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
      },
      .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
      .scale_x = 1.0f,
      .scale_y = 1.0f,
      .rgb_swap = false,
      .byte_swap = false,
      .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(g_ppa_srm_client, &rotation);
    if (err == ESP_OK) {
      if (!g_logged_ppa_rotation) {
        ESP_LOGI(TAG, "P4 rotation=PPA angle=270 output=RGB565 panel-order=RGB size=%dx%d",
          BSP_LCD_H_RES, BSP_LCD_V_RES);
        g_logged_ppa_rotation = true;
      }
      return ESP_OK;
    }
    if (!g_logged_ppa_fallback) {
      ESP_LOGW(TAG, "P4 PPA rotation failed (%s); using CPU fallback", esp_err_to_name(err));
      g_logged_ppa_fallback = true;
    }
  }
  for (int y = 0; y < PET_P4_UI_HEIGHT; y += 1) {
    for (int x = 0; x < PET_P4_UI_WIDTH; x += 1) {
      int panel_x = PET_P4_UI_HEIGHT - 1 - y;
      int panel_y = x;
      g_native_framebuffer[panel_y * BSP_LCD_H_RES + panel_x] =
        g_framebuffer[y * PET_P4_UI_WIDTH + x];
    }
  }
  return ESP_OK;
}

esp_err_t pet_p4_renderer_init(void) {
  if (!g_framebuffer) {
    g_framebuffer = (uint16_t *) heap_caps_aligned_calloc(
      PET_P4_PPA_BUFFER_ALIGNMENT,
      PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT,
      sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA
    );
    g_framebuffer_ppa_compatible = g_framebuffer != NULL;
    if (!g_framebuffer) {
      g_framebuffer = (uint16_t *) heap_caps_malloc(
        PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
      );
    }
    if (!g_framebuffer) g_framebuffer = (uint16_t *) heap_caps_malloc(PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t), MALLOC_CAP_8BIT);
  }
  if (!g_native_framebuffers[0]) {
    g_native_framebuffer_ppa_compatible = true;
    for (unsigned int index = 0; index < PET_P4_NATIVE_OUTPUT_BUFFER_COUNT; index += 1) {
      g_native_framebuffers[index] = (uint16_t *) heap_caps_aligned_calloc(
        PET_P4_PPA_BUFFER_ALIGNMENT,
        BSP_LCD_H_RES * BSP_LCD_V_RES,
        sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA
      );
      if (!g_native_framebuffers[index]) {
        g_native_framebuffer_ppa_compatible = false;
        g_native_framebuffers[index] = (uint16_t *) heap_caps_malloc(
          BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
      }
      if (!g_native_framebuffers[index]) {
        g_native_framebuffers[index] = (uint16_t *) heap_caps_malloc(
          BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
          MALLOC_CAP_8BIT
        );
      }
    }
    g_native_framebuffer = g_native_framebuffers[0];
  }
  if (!g_jpeg_input) {
    jpeg_decode_memory_alloc_cfg_t input_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    g_jpeg_input = (uint8_t *) jpeg_alloc_decoder_mem(
      PET_P4_MAX_JPEG_BYTES,
      &input_mem_cfg,
      &g_jpeg_input_capacity
    );
    g_jpeg_input_dma_compatible = g_jpeg_input != NULL;
    if (!g_jpeg_input) {
      g_jpeg_input = (uint8_t *) heap_caps_malloc(PET_P4_MAX_JPEG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!g_jpeg_input) g_jpeg_input = (uint8_t *) heap_caps_malloc(PET_P4_MAX_JPEG_BYTES, MALLOC_CAP_8BIT);
      if (g_jpeg_input) g_jpeg_input_capacity = PET_P4_MAX_JPEG_BYTES;
    }
  }
  if (!g_jpeg_output) {
    size_t requested_output_capacity = PET_P4_ASSET_WIDTH * PET_P4_ASSET_HEIGHT * sizeof(uint16_t);
    jpeg_decode_memory_alloc_cfg_t output_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    g_jpeg_output = (uint16_t *) jpeg_alloc_decoder_mem(
      requested_output_capacity,
      &output_mem_cfg,
      &g_jpeg_output_capacity
    );
    g_jpeg_output_dma_compatible = g_jpeg_output != NULL;
    if (!g_jpeg_output) {
      g_jpeg_output_capacity = requested_output_capacity;
      g_jpeg_output = (uint16_t *) heap_caps_malloc(g_jpeg_output_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!g_jpeg_output) g_jpeg_output = (uint16_t *) heap_caps_malloc(g_jpeg_output_capacity, MALLOC_CAP_8BIT);
    }
  }
  if (!g_tjpgd_workbuf) {
    g_tjpgd_workbuf = (uint8_t *) heap_caps_malloc(PET_P4_TJPGD_WORK_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_tjpgd_workbuf) g_tjpgd_workbuf = (uint8_t *) heap_caps_malloc(PET_P4_TJPGD_WORK_BUFFER_BYTES, MALLOC_CAP_8BIT);
  }
  if (!g_h264_pack_task_init_attempted) {
    g_h264_pack_task_init_attempted = true;
    g_h264_pack_done = xSemaphoreCreateBinary();
    BaseType_t task_result = g_h264_pack_done
      ? xTaskCreatePinnedToCore(
          h264_pack_worker,
          "h264-pack",
          PET_P4_H264_PACK_TASK_STACK_BYTES,
          NULL,
          PET_P4_H264_PACK_TASK_PRIORITY,
          &g_h264_pack_task,
          CONFIG_ESP_H264_DUAL_TASK_CORE
        )
      : pdFAIL;
    if (task_result != pdPASS) {
      if (g_h264_pack_done) vSemaphoreDelete(g_h264_pack_done);
      g_h264_pack_done = NULL;
      g_h264_pack_task = NULL;
      ESP_LOGW(TAG, "P4 H264 parallel pack task unavailable; using one core");
    }
  }
  if (!g_framebuffer || !g_native_framebuffers[0] || !g_native_framebuffers[1]
      || !g_jpeg_input || !g_jpeg_output || !g_tjpgd_workbuf) {
    return ESP_ERR_NO_MEM;
  }
  if (!g_ppa_init_attempted) {
    g_ppa_init_attempted = true;
    if (g_native_framebuffer_ppa_compatible) {
      ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
      };
      esp_err_t ppa_err = ppa_register_client(&ppa_config, &g_ppa_srm_client);
      if (ppa_err != ESP_OK) {
        g_ppa_srm_client = NULL;
        ESP_LOGW(
          TAG,
          "P4 PPA rotation init failed (%s); using CPU fallback",
          esp_err_to_name(ppa_err)
        );
      }
    } else {
      ESP_LOGW(TAG, "P4 PPA output buffer unavailable; using CPU rotation");
    }
  }
  if (!g_jpeg_decoder_init_attempted) {
    g_jpeg_decoder_init_attempted = true;
    if (g_jpeg_input_dma_compatible && g_jpeg_output_dma_compatible) {
      jpeg_decode_engine_cfg_t decoder_cfg = {
        .intr_priority = 0,
        .timeout_ms = PET_P4_HARDWARE_JPEG_TIMEOUT_MS,
      };
      esp_err_t decoder_err = jpeg_new_decoder_engine(&decoder_cfg, &g_jpeg_decoder);
      if (decoder_err != ESP_OK) {
        g_jpeg_decoder = NULL;
        ESP_LOGW(
          TAG,
          "P4 hardware JPEG decoder init failed (%s); using TJPGD fallback",
          esp_err_to_name(decoder_err)
        );
      }
    } else {
      ESP_LOGW(TAG, "P4 JPEG DMA buffers unavailable; using TJPGD fallback");
    }
  }
  return ESP_OK;
}

esp_err_t pet_p4_renderer_render(
  const pet_p4_runtime_state_t *state,
  const pet_p4_view_model_t *view,
  unsigned long long now_ms
) {
  ESP_RETURN_ON_ERROR(pet_p4_renderer_init(), TAG, "allocate render buffers");
  g_native_framebuffer_index =
    (g_native_framebuffer_index + 1U) % PET_P4_NATIVE_OUTPUT_BUFFER_COUNT;
  g_native_framebuffer = g_native_framebuffers[g_native_framebuffer_index];
  int64_t render_started_us = esp_timer_get_time();
  int64_t overlay_started_us = render_started_us;
  uint32_t asset_us = 0;
  g_last_decode_us = 0;
  g_last_h264_pack_us = 0;
  g_last_h264_convert_us = 0;
  if (!g_framebuffer_initialized) {
    fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, rgb565(0, 0, 0));
    g_framebuffer_initialized = true;
  }
  const char *page = view && view->page ? view->page : "main";
  if (strcmp(page, "components") == 0) {
    render_component_center_page();
  } else if (strcmp(page, "app") == 0) {
    render_miniapp_page(state, now_ms);
  } else {
    if (g_last_render_page[0] && strcmp(g_last_render_page, "main") != 0) {
      fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, rgb565(0, 0, 0));
    }
    bool show_session_queue = state && state->session_queue_count > 0;
    int64_t asset_started_us = esp_timer_get_time();
    bool drew_asset = render_asset_pet_frame(state, view, now_ms);
    overlay_started_us = esp_timer_get_time();
    asset_us = elapsed_us_clamped(asset_started_us, overlay_started_us);
    if (!drew_asset) {
      if (!g_logged_missing_asset) {
        ESP_LOGI(TAG, "no usable P4 appearance frames yet; keeping bubble/status visible");
        g_logged_missing_asset = true;
      }
    }
    if (!show_session_queue) draw_bubble(view, now_ms);
    draw_session_queue(state, now_ms);
  }
  draw_touch_feedback(state, now_ms);
  draw_page_indicator(page);
  draw_connection_banner(state, now_ms);
  snprintf(g_last_render_page, sizeof(g_last_render_page), "%s", page);
  draw_boot_diagnostic(now_ms);
  int64_t rotate_started_us = esp_timer_get_time();
  uint32_t overlay_us = elapsed_us_clamped(overlay_started_us, rotate_started_us);
  ESP_RETURN_ON_ERROR(rotate_landscape_to_panel(), TAG, "rotate logical framebuffer");
  int64_t lcd_started_us = esp_timer_get_time();
  uint32_t rotate_us = elapsed_us_clamped(rotate_started_us, lcd_started_us);
  esp_err_t lcd_err = pet_p4_lcd_draw_rgb565(
    0,
    0,
    BSP_LCD_H_RES,
    BSP_LCD_V_RES,
    g_native_framebuffer
  );
  int64_t render_ended_us = esp_timer_get_time();
  record_render_performance(
    render_started_us,
    render_ended_us,
    asset_us,
    overlay_us,
    rotate_us,
    elapsed_us_clamped(lcd_started_us, render_ended_us)
  );
  return lcd_err;
}

esp_err_t pet_p4_renderer_render_transfer_status(
  bool firmware_update,
  unsigned long long now_ms
) {
  ESP_RETURN_ON_ERROR(pet_p4_renderer_init(), TAG, "allocate transfer render buffers");
  g_native_framebuffer_index =
    (g_native_framebuffer_index + 1U) % PET_P4_NATIVE_OUTPUT_BUFFER_COUNT;
  g_native_framebuffer = g_native_framebuffers[g_native_framebuffer_index];

  const uint16_t background = rgb565(5, 7, 6);
  const uint16_t panel = rgb565(14, 18, 16);
  const uint16_t outline = rgb565(52, 58, 53);
  const uint16_t orange = rgb565(255, 163, 31);
  const uint16_t ivory = rgb565(250, 241, 204);
  const uint16_t muted = rgb565(126, 133, 126);
  const char *title = firmware_update ? "固件更新中" : "形象同步中";
  const char *detail = firmware_update
    ? "正在写入并校验新固件"
    : "正在传输并校验形象素材";

  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, background);
  fill_round_rect_outline(64, 102, 512, 276, 20, panel, outline);
  fill_ellipse(320, 170, 30, 30, orange);
  fill_ellipse(320, 170, 15, 15, panel);
  unsigned int active_dot = (unsigned int) ((now_ms / 300ULL) % 3ULL);
  for (unsigned int index = 0; index < 3; index += 1) {
    int dot_radius = index == active_dot ? 6 : 3;
    fill_ellipse(
      292 + (int) index * 28,
      170,
      dot_radius,
      dot_radius,
      index == active_dot ? ivory : muted
    );
  }
  draw_text_center(title, 320, 222, 440, ivory, 3);
  draw_text_center(detail, 320, 278, 440, muted, 1);
  draw_text_center("请保持设备连接，不要拔出数据线", 320, 322, 440, orange, 1);
  snprintf(g_last_render_page, sizeof(g_last_render_page), "%s", "transfer");
  ESP_RETURN_ON_ERROR(rotate_landscape_to_panel(), TAG, "rotate transfer framebuffer");
  return pet_p4_lcd_draw_rgb565(
    0,
    0,
    BSP_LCD_H_RES,
    BSP_LCD_V_RES,
    g_native_framebuffer
  );
}

const uint16_t *pet_p4_renderer_logical_framebuffer(int *width, int *height) {
  if (width) *width = PET_P4_UI_WIDTH;
  if (height) *height = PET_P4_UI_HEIGHT;
  return g_framebuffer;
}
