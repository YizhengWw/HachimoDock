/*
 * [Input] UART/native USB protocol traffic, display/input state, and assets.
 * [Output] initialized P4 runtime with serialized state access, origin-aware
 *          protocol replies, non-destructive storage mounting, and independent
 *          render/RX health samples, plus post-OTA synchronization of the
 *          built-in component bundle carried inside the application image,
 *          idle appearance-transfer recovery, a lightweight live transfer
 *          screen, and a backlight-gated first complete frame.
 * [Pos] ESP32-P4 firmware entry point and task coordinator.
 * [Sync] If this file changes, update firmware/.folder.md.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"

#include "pet_p4_lcd.h"
#include "pet_p4_audio.h"
#include "pet_p4_diagnostics.h"
#include "pet_p4_input.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_ota.h"
#include "pet_p4_protocol.h"
#include "pet_p4_transport_config.h"
#include "pet_p4_renderer.h"
#include "pet_p4_touch.h"
#include "pet_p4_usb_native.h"
#include "pet_p4_view.h"

static const char *TAG = "pet-p4";

#define PET_P4_UART_NUM UART_NUM_0
#define PET_P4_UART_RX_BUFFER_BYTES (128 * 1024)
#define PET_P4_UART_READ_CHUNK_BYTES 2048
#define PET_P4_TRANSPORT_RX_TASK_STACK_BYTES 8192
#define PET_P4_PROTOCOL_TASK_STACK_BYTES 12288
#define PET_P4_PROTOCOL_QUEUE_DEPTH 24
#define PET_P4_MAIN_RENDER_INTERVAL_MS 50ULL
#define PET_P4_MAIN_LOOP_DELAY_MS 1
#define PET_P4_MAIN_RX_HEALTH_TIMEOUT_MS 1000ULL

static pet_p4_runtime_state_t g_state;
static char g_line_buffer[32768];
static size_t g_line_used;
static char g_uart_line_buffer[32768];
static size_t g_uart_line_used;
static SemaphoreHandle_t g_render_mutex;
static SemaphoreHandle_t g_transport_mutex;
static SemaphoreHandle_t g_state_mutex;
static QueueHandle_t g_protocol_queue;
static pet_p4_runtime_state_t *g_render_state;
static portMUX_TYPE g_rx_health_mutex = portMUX_INITIALIZER_UNLOCKED;
static TickType_t g_usb_rx_last_tick;
static TickType_t g_uart_rx_last_tick;
static bool g_usb_rx_seen;
static bool g_uart_rx_seen;
static bool g_usb_protocol_seen;

typedef enum {
  PET_P4_TRANSPORT_BROADCAST = 0,
  PET_P4_TRANSPORT_UART,
  PET_P4_TRANSPORT_USB_SERIAL_JTAG,
} pet_p4_transport_route_t;

typedef struct {
  char *line;
  pet_p4_send_line_fn send_line;
  void *reply_ctx;
} pet_p4_protocol_request_t;

static const pet_p4_transport_route_t g_uart_route = PET_P4_TRANSPORT_UART;
static const pet_p4_transport_route_t g_usb_serial_jtag_route = PET_P4_TRANSPORT_USB_SERIAL_JTAG;

static void mark_usb_protocol_seen(void);
static bool usb_protocol_seen(void);

#define PET_P4_SCREENSHOT_W 320
#define PET_P4_SCREENSHOT_H 240
#define PET_P4_SCREENSHOT_RAW_CHUNK 3072

static esp_err_t build_board_device_id(char *out, size_t out_size) {
  uint8_t mac[6] = {0};
  if (!out || out_size < 16) return ESP_ERR_INVALID_ARG;
  esp_err_t err = esp_read_mac(mac, ESP_MAC_BASE);
  if (err != ESP_OK) {
    snprintf(out, out_size, "p4-unknown");
    return err;
  }
  snprintf(
    out,
    out_size,
    "p4-%02x%02x%02x%02x%02x%02x",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );
  return ESP_OK;
}

static void mark_rx_healthy(bool usb) {
  TickType_t now = xTaskGetTickCount();
  portENTER_CRITICAL(&g_rx_health_mutex);
  if (usb) {
    g_usb_rx_last_tick = now;
    g_usb_rx_seen = true;
  } else {
    g_uart_rx_last_tick = now;
    g_uart_rx_seen = true;
  }
  portEXIT_CRITICAL(&g_rx_health_mutex);
}

static bool rx_tasks_healthy(void) {
  TickType_t usb_last;
  TickType_t uart_last;
  bool usb_seen;
  bool uart_seen;
  TickType_t now = xTaskGetTickCount();
  TickType_t timeout = pdMS_TO_TICKS(PET_P4_MAIN_RX_HEALTH_TIMEOUT_MS);
  portENTER_CRITICAL(&g_rx_health_mutex);
  usb_last = g_usb_rx_last_tick;
  uart_last = g_uart_rx_last_tick;
  usb_seen = g_usb_rx_seen;
  uart_seen = g_uart_rx_seen;
  portEXIT_CRITICAL(&g_rx_health_mutex);
  return usb_seen && uart_seen
    && now - usb_last <= timeout
    && now - uart_last <= timeout;
}

static void usb_write_all(const char *data, size_t len) {
  size_t written = 0;
  while (written < len) {
    int n = usb_serial_jtag_write_bytes(data + written, len - written, pdMS_TO_TICKS(20));
    if (n <= 0) break;
    written += (size_t) n;
  }
  (void) usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
}

static void uart_write_all(const char *data, size_t len) {
  size_t written = 0;
  while (written < len) {
    int n = uart_write_bytes(PET_P4_UART_NUM, data + written, len - written);
    if (n <= 0) break;
    written += (size_t) n;
  }
  (void) uart_wait_tx_done(PET_P4_UART_NUM, pdMS_TO_TICKS(100));
}

static int transport_log_vprintf(const char *format, va_list args) {
  if (g_transport_mutex) xSemaphoreTake(g_transport_mutex, portMAX_DELAY);
  int written = vprintf(format, args);
  fflush(stdout);
  if (g_transport_mutex) xSemaphoreGive(g_transport_mutex);
  return written;
}

static void transport_send_line(const char *line, void *ctx) {
  pet_p4_transport_route_t route = ctx
    ? *((const pet_p4_transport_route_t *) ctx)
    : PET_P4_TRANSPORT_BROADCAST;
  if (!line) return;
  if (g_transport_mutex) xSemaphoreTake(g_transport_mutex, portMAX_DELAY);
  if (route == PET_P4_TRANSPORT_BROADCAST || route == PET_P4_TRANSPORT_UART) {
    uart_write_all(line, strlen(line));
    uart_write_all("\n", 1);
  }
  if (route == PET_P4_TRANSPORT_USB_SERIAL_JTAG
      || (route == PET_P4_TRANSPORT_BROADCAST && usb_protocol_seen())) {
    usb_write_all(line, strlen(line));
    usb_write_all("\n", 1);
  }
  if (route == PET_P4_TRANSPORT_BROADCAST) {
    pet_p4_native_usb_send_json_line(line, NULL);
  }
  if (g_transport_mutex) xSemaphoreGive(g_transport_mutex);
}

static void send_screenshot_chunk(
  const char *id,
  int index,
  const uint8_t *raw,
  size_t raw_len,
  char *b64,
  size_t b64_capacity,
  void *reply_ctx
) {
  size_t b64_len = 0;
  char header[192];
  if (mbedtls_base64_encode((unsigned char *) b64, b64_capacity - 1, &b64_len, raw, raw_len) != 0) {
    return;
  }
  b64[b64_len] = '\0';
  snprintf(
    header,
    sizeof(header),
    "{\"topic\":\"debug/screenshot_chunk\",\"payload\":{\"id\":\"%s\",\"index\":%d,\"data\":\"",
    id,
    index
  );
  pet_p4_transport_route_t route = reply_ctx
    ? *((const pet_p4_transport_route_t *) reply_ctx)
    : PET_P4_TRANSPORT_BROADCAST;
  if (g_transport_mutex) xSemaphoreTake(g_transport_mutex, portMAX_DELAY);
  if (route == PET_P4_TRANSPORT_BROADCAST || route == PET_P4_TRANSPORT_UART) {
    uart_write_all(header, strlen(header));
    uart_write_all(b64, b64_len);
    uart_write_all("\"}}\n", 4);
  }
  if (route == PET_P4_TRANSPORT_USB_SERIAL_JTAG
      || (route == PET_P4_TRANSPORT_BROADCAST && usb_protocol_seen())) {
    usb_write_all(header, strlen(header));
    usb_write_all(b64, b64_len);
    usb_write_all("\"}}\n", 4);
  }
  if (g_transport_mutex) xSemaphoreGive(g_transport_mutex);
}

static bool maybe_handle_debug_line(const char *line, void *reply_ctx) {
  if (!line || !strstr(line, "\"debug/screenshot\"")) return false;

  uint8_t *raw = (uint8_t *) malloc(PET_P4_SCREENSHOT_W * PET_P4_SCREENSHOT_H * 2);
  char *b64 = (char *) malloc(((PET_P4_SCREENSHOT_RAW_CHUNK + 2) / 3) * 4 + 8);
  if (!raw || !b64) {
    free(raw);
    free(b64);
    transport_send_line("{\"topic\":\"debug/screenshot_error\",\"payload\":{\"error\":\"no memory\"}}", reply_ctx);
    return true;
  }
  if (!g_render_mutex || xSemaphoreTake(g_render_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    free(raw);
    free(b64);
    transport_send_line("{\"topic\":\"debug/screenshot_error\",\"payload\":{\"error\":\"renderer busy\"}}", reply_ctx);
    return true;
  }

  int src_w = 0;
  int src_h = 0;
  const uint16_t *src = pet_p4_renderer_logical_framebuffer(&src_w, &src_h);
  if (!src || src_w <= 0 || src_h <= 0) {
    xSemaphoreGive(g_render_mutex);
    free(raw);
    free(b64);
    transport_send_line("{\"topic\":\"debug/screenshot_error\",\"payload\":{\"error\":\"no framebuffer\"}}", reply_ctx);
    return true;
  }

  unsigned int non_black = 0;
  unsigned int checksum = 2166136261u;
  for (int y = 0; y < PET_P4_SCREENSHOT_H; y += 1) {
    int sy = y * src_h / PET_P4_SCREENSHOT_H;
    for (int x = 0; x < PET_P4_SCREENSHOT_W; x += 1) {
      int sx = x * src_w / PET_P4_SCREENSHOT_W;
      uint16_t px = src[sy * src_w + sx];
      if (px != 0) non_black += 1;
      checksum ^= (unsigned int) px;
      checksum *= 16777619u;
      size_t off = (size_t) (y * PET_P4_SCREENSHOT_W + x) * 2;
      raw[off] = (uint8_t) (px & 0xff);
      raw[off + 1] = (uint8_t) (px >> 8);
    }
  }
  xSemaphoreGive(g_render_mutex);

  char id[32];
  char begin[192];
  snprintf(id, sizeof(id), "%llu", (unsigned long long) (esp_timer_get_time() / 1000ULL));
  snprintf(
    begin,
    sizeof(begin),
    "{\"topic\":\"debug/screenshot_begin\",\"payload\":{\"id\":\"%s\",\"width\":%d,\"height\":%d,\"format\":\"rgb565le\"}}",
    id,
    PET_P4_SCREENSHOT_W,
    PET_P4_SCREENSHOT_H
  );
  transport_send_line(begin, reply_ctx);

  size_t total = PET_P4_SCREENSHOT_W * PET_P4_SCREENSHOT_H * 2;
  int index = 0;
  for (size_t offset = 0; offset < total; offset += PET_P4_SCREENSHOT_RAW_CHUNK) {
    size_t n = total - offset;
    if (n > PET_P4_SCREENSHOT_RAW_CHUNK) n = PET_P4_SCREENSHOT_RAW_CHUNK;
    send_screenshot_chunk(
      id,
      index++,
      raw + offset,
      n,
      b64,
      ((PET_P4_SCREENSHOT_RAW_CHUNK + 2) / 3) * 4 + 8,
      reply_ctx
    );
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  char end[160];
  snprintf(
    end,
    sizeof(end),
    "{\"topic\":\"debug/screenshot_end\",\"payload\":{\"id\":\"%s\",\"chunks\":%d,\"nonBlack\":%u,\"checksum\":\"%08x\"}}",
    id,
    index,
    non_black,
    checksum
  );
  transport_send_line(end, reply_ctx);
  free(raw);
  free(b64);
  return true;
}

static bool enqueue_protocol_line(
  const char *line,
  pet_p4_send_line_fn send_line,
  void *reply_ctx,
  void *enqueue_ctx
) {
  (void) enqueue_ctx;
  if (!line || !line[0] || !send_line || !g_protocol_queue) return false;
  size_t line_len = strlen(line);
  char *line_copy = (char *) heap_caps_malloc(
    line_len + 1,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
  if (!line_copy) line_copy = (char *) malloc(line_len + 1);
  if (!line_copy) return false;
  memcpy(line_copy, line, line_len + 1);
  pet_p4_protocol_request_t request = {
    .line = line_copy,
    .send_line = send_line,
    .reply_ctx = reply_ctx,
  };
  if (xQueueSend(g_protocol_queue, &request, pdMS_TO_TICKS(20)) == pdTRUE) {
    return true;
  }
  free(line_copy);
  send_line(
    "{\"topic\":\"protocol/error\",\"payload\":{\"ok\":false,\"error\":\"command_queue_busy\"}}",
    reply_ctx
  );
  return false;
}

static void protocol_worker_task(void *arg) {
  (void) arg;
  pet_p4_protocol_request_t request;
  while (true) {
    if (xQueueReceive(g_protocol_queue, &request, portMAX_DELAY) != pdTRUE) continue;
    bool handled_debug = request.send_line == transport_send_line
      && maybe_handle_debug_line(request.line, request.reply_ctx);
    if (!handled_debug) {
      xSemaphoreTake(g_state_mutex, portMAX_DELAY);
      pet_p4_handle_line(&g_state, request.line, request.send_line, request.reply_ctx);
      xSemaphoreGive(g_state_mutex);
    }
    free(request.line);
  }
}

static bool consume_byte(
  char ch,
  char *line_buffer,
  size_t *line_used,
  size_t capacity,
  void *reply_ctx
) {
  if (ch == '\r') return false;
  if (ch == '\n') {
    line_buffer[*line_used] = '\0';
    if (*line_used > 0) {
      (void) enqueue_protocol_line(
        line_buffer,
        transport_send_line,
        reply_ctx,
        NULL
      );
    }
    *line_used = 0;
    return pet_p4_raw_asset_chunk_active();
  }
  if (*line_used + 1 < capacity) {
    line_buffer[(*line_used)++] = ch;
  } else {
    *line_used = 0;
  }
  return false;
}

static void mark_usb_protocol_seen(void) {
  portENTER_CRITICAL(&g_rx_health_mutex);
  g_usb_protocol_seen = true;
  portEXIT_CRITICAL(&g_rx_health_mutex);
}

static bool usb_protocol_seen(void) {
  bool seen;
  portENTER_CRITICAL(&g_rx_health_mutex);
  seen = g_usb_protocol_seen;
  portEXIT_CRITICAL(&g_rx_health_mutex);
  return seen;
}

static void consume_transport_bytes(
  const uint8_t *data,
  size_t len,
  char *line_buffer,
  size_t *line_used,
  size_t line_capacity,
  bool *raw_mode,
  void *reply_ctx
) {
  size_t offset = 0;
  while (offset < len) {
    if (!*raw_mode && pet_p4_raw_asset_chunk_active()) *raw_mode = true;
    if (*raw_mode) {
      xSemaphoreTake(g_state_mutex, portMAX_DELAY);
      size_t consumed = pet_p4_consume_raw_asset_bytes(
        data + offset,
        len - offset,
        transport_send_line,
        reply_ctx
      );
      xSemaphoreGive(g_state_mutex);
      if (consumed > 0) {
        offset += consumed;
      }
      if (!pet_p4_raw_asset_chunk_active() || consumed == 0) {
        *raw_mode = false;
      }
      continue;
    }
    if (consume_byte((char) data[offset], line_buffer, line_used, line_capacity, reply_ctx)) {
      *raw_mode = true;
    }
    offset += 1;
  }
}

static esp_err_t init_storage(void) {
  esp_vfs_spiffs_conf_t conf = {
    .base_path = "/spiffs",
    .partition_label = "storage",
    .max_files = 8,
    .format_if_mount_failed = false,
  };
  return esp_vfs_spiffs_register(&conf);
}

static esp_err_t init_usb(void) {
  if (usb_serial_jtag_is_driver_installed()) return ESP_OK;
  usb_serial_jtag_driver_config_t config = {
    .tx_buffer_size = 4096,
    .rx_buffer_size = 32768,
  };
  return usb_serial_jtag_driver_install(&config);
}

static esp_err_t init_uart(void) {
  uart_config_t config = {
    .baud_rate = PET_P4_UART_BAUD,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_RETURN_ON_ERROR(uart_param_config(PET_P4_UART_NUM, &config), TAG, "configure UART0");
  ESP_RETURN_ON_ERROR(
    uart_set_pin(PET_P4_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
    TAG,
    "configure UART0 pins"
  );
  esp_err_t err = uart_driver_install(
    PET_P4_UART_NUM,
    PET_P4_UART_RX_BUFFER_BYTES,
    4096,
    0,
    NULL,
    0
  );
  if (err == ESP_ERR_INVALID_STATE) return ESP_OK;
  return err;
}

static void usb_rx_task(void *arg) {
  (void) arg;
  uint8_t buf[512];
  bool raw_mode = false;
  while (true) {
    int len = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(20));
    if (len >= 0) mark_rx_healthy(true);
    if (len > 0) {
      mark_usb_protocol_seen();
      consume_transport_bytes(
        buf,
        (size_t) len,
        g_line_buffer,
        &g_line_used,
        sizeof(g_line_buffer),
        &raw_mode,
        (void *) &g_usb_serial_jtag_route
      );
    }
  }
}

static void uart_rx_task(void *arg) {
  (void) arg;
  uint8_t buf[PET_P4_UART_READ_CHUNK_BYTES];
  bool raw_mode = false;
  while (true) {
    int len = uart_read_bytes(PET_P4_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(20));
    if (len >= 0) mark_rx_healthy(false);
    if (len > 0) {
      consume_transport_bytes(
        buf,
        (size_t) len,
        g_uart_line_buffer,
        &g_uart_line_used,
        sizeof(g_uart_line_buffer),
        &raw_mode,
        (void *) &g_uart_route
      );
    }
  }
}

void app_main(void) {
  unsigned long long last_logged_update = 0;
  unsigned long long last_hello_ms = 0;
  unsigned long long last_render_ms = 0;
  unsigned long long last_transfer_render_ms = 0;
  unsigned long long last_lcd_keepalive_ms = 0;
  unsigned long long last_rx_health_warning_ms = 0;
  bool transfer_was_active = false;
  bool runtime_render_healthy = false;
  char lcd_status[192];
  char i2c_devices[128];
  char board_device_id[PET_P4_DEVICE_ID_MAX];
  esp_err_t backlight_hide_err = pet_p4_lcd_prepare_boot();
  if (backlight_hide_err != ESP_OK) {
    ESP_LOGW(TAG, "LCD boot backlight could not be hidden: %s", esp_err_to_name(backlight_hide_err));
  }
  ESP_ERROR_CHECK(nvs_flash_init());
  g_render_mutex = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(g_render_mutex ? ESP_OK : ESP_ERR_NO_MEM);
  g_transport_mutex = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(g_transport_mutex ? ESP_OK : ESP_ERR_NO_MEM);
  g_state_mutex = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(g_state_mutex ? ESP_OK : ESP_ERR_NO_MEM);
  esp_log_set_vprintf(transport_log_vprintf);
  esp_err_t storage_err = init_storage();
  if (storage_err != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS mount failed; preserving storage for explicit recovery: %s",
             esp_err_to_name(storage_err));
  }
  esp_err_t diagnostics_err = pet_p4_diagnostics_init();
  if (diagnostics_err != ESP_OK) {
    ESP_LOGW(TAG, "diagnostics persistence unavailable; continuing without boot history: %s",
             esp_err_to_name(diagnostics_err));
  }
  esp_err_t device_id_err = build_board_device_id(board_device_id, sizeof(board_device_id));
  if (device_id_err != ESP_OK) {
    ESP_LOGE(TAG, "base MAC unavailable; using degraded device id: %s", esp_err_to_name(device_id_err));
  }
  pet_p4_state_init(&g_state, board_device_id);
  g_render_state = (pet_p4_runtime_state_t *) heap_caps_calloc(
    1,
    sizeof(*g_render_state),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
  if (!g_render_state) g_render_state = calloc(1, sizeof(*g_render_state));
  ESP_ERROR_CHECK(g_render_state ? ESP_OK : ESP_ERR_NO_MEM);
  g_protocol_queue = xQueueCreate(
    PET_P4_PROTOCOL_QUEUE_DEPTH,
    sizeof(pet_p4_protocol_request_t)
  );
  ESP_ERROR_CHECK(g_protocol_queue ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(
    xTaskCreate(
      protocol_worker_task,
      "pet_p4_protocol",
      PET_P4_PROTOCOL_TASK_STACK_BYTES,
      NULL,
      11,
      NULL
    ) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM
  );
  esp_err_t miniapp_err = pet_p4_miniapp_init();
  if (miniapp_err != ESP_OK && miniapp_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "mini-app init failed: %s", esp_err_to_name(miniapp_err));
  }
  esp_err_t builtin_sync_err = pet_p4_miniapp_sync_builtins();
  if (builtin_sync_err != ESP_OK) {
    ESP_LOGW(TAG, "built-in component sync failed: %s", esp_err_to_name(builtin_sync_err));
  }
  // Parse the large appearance manifest before any USB/UART receive task can
  // concurrently parse command JSON during boot.
  pet_p4_load_asset_manifest(&g_state);
  ESP_ERROR_CHECK(init_usb());
  ESP_ERROR_CHECK(init_uart());
  ESP_ERROR_CHECK(pet_p4_input_init());
  esp_err_t lcd_err = pet_p4_lcd_init();
  esp_err_t touch_err = lcd_err == ESP_OK ? pet_p4_touch_init() : ESP_ERR_INVALID_STATE;
  if (touch_err != ESP_OK) {
    ESP_LOGW(TAG, "P4 touchscreen unavailable, keeping buttons and display alive: %s",
             esp_err_to_name(touch_err));
  }
  esp_err_t audio_err = pet_p4_audio_init(board_device_id, transport_send_line, NULL);
  if (audio_err != ESP_OK) {
    ESP_LOGW(TAG, "P4 microphone unavailable, keeping non-voice controls alive: %s",
             esp_err_to_name(audio_err));
  }
  esp_err_t render_err = ESP_ERR_INVALID_STATE;
  if (lcd_err == ESP_OK) {
    pet_p4_view_model_t boot_view;
    pet_p4_build_view_model(&g_state, &boot_view);
    render_err = pet_p4_renderer_render(&g_state, &boot_view, 0);
    if (render_err == ESP_OK) {
      render_err = pet_p4_lcd_reveal();
    }
    runtime_render_healthy = render_err == ESP_OK;
  } else {
    ESP_LOGW(TAG, "LCD init failed, keep USB runtime alive: %s", esp_err_to_name(lcd_err));
  }
  (void) pet_p4_lcd_scan_i2c(i2c_devices, sizeof(i2c_devices));
  snprintf(
    lcd_status,
    sizeof(lcd_status),
    "{\"topic\":\"debug/lcd\",\"payload\":{\"init\":\"%s\",\"backlight\":\"%s\",\"render\":\"%s\",\"i2c\":\"%s\"}}",
    esp_err_to_name(lcd_err),
    esp_err_to_name(pet_p4_lcd_backlight_status()),
    esp_err_to_name(render_err),
    i2c_devices
  );
  transport_send_line(lcd_status, NULL);
  esp_err_t ota_err = pet_p4_ota_init(&g_state, transport_send_line, NULL);
  if (ota_err != ESP_OK) {
    ESP_LOGW(TAG, "A/B firmware OTA unavailable: %s", esp_err_to_name(ota_err));
  }
  esp_err_t native_usb_err = pet_p4_native_usb_init(
    &g_state,
    g_state_mutex,
    enqueue_protocol_line,
    NULL
  );
  if (native_usb_err != ESP_OK) {
    ESP_LOGW(TAG, "native USB init failed, keeping UART transports alive: %s", esp_err_to_name(native_usb_err));
  }
  // UART0 is the default Waveshare Type-C route, so reserve its task first.
  BaseType_t uart_rx_created = xTaskCreate(
    uart_rx_task,
    "pet_p4_uart_rx",
    PET_P4_TRANSPORT_RX_TASK_STACK_BYTES,
    NULL,
    10,
    NULL
  );
  BaseType_t usb_rx_created = xTaskCreate(
    usb_rx_task,
    "pet_p4_usj_rx",
    PET_P4_TRANSPORT_RX_TASK_STACK_BYTES,
    NULL,
    10,
    NULL
  );
  if (usb_rx_created != pdPASS || uart_rx_created != pdPASS) {
    ESP_LOGE(TAG, "transport RX task creation failed usb=%ld uart=%ld",
             (long) usb_rx_created, (long) uart_rx_created);
  }
  ESP_LOGI(TAG, "Pet Manager ESP-P4 USB Serial/JTAG, USB-UART and native USB runtime ready");
  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  pet_p4_send_hello(&g_state, transport_send_line, NULL);
  xSemaphoreGive(g_state_mutex);
  pet_p4_audio_send_status();
  if (lcd_err != ESP_OK || render_err != ESP_OK
      || usb_rx_created != pdPASS || uart_rx_created != pdPASS) {
    pet_p4_ota_runtime_failed((unsigned long long) (esp_timer_get_time() / 1000ULL));
  }

  while (true) {
    unsigned long long now_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
    bool render_frame = false;
    bool render_transfer_frame = false;
    bool firmware_transfer_active = false;
    bool keep_lcd_awake = false;
    pet_p4_view_model_t render_view = {0};
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    pet_p4_diagnostics_process(now_ms, &g_state);
    pet_p4_ota_process(now_ms);
    pet_p4_asset_transfer_process(&g_state, now_ms);
    pet_p4_native_usb_process(now_ms);
    pet_p4_miniapp_process(now_ms);
    pet_p4_state_process(&g_state, now_ms);
    pet_p4_touch_process(&g_state, transport_send_line, NULL);
    pet_p4_input_process(&g_state, transport_send_line, NULL);
    pet_p4_audio_process(&g_state, now_ms);
    firmware_transfer_active = pet_p4_ota_transfer_active();
    bool transfer_active = g_state.asset_transfer_active || firmware_transfer_active;
    if (transfer_was_active && !transfer_active) {
      pet_p4_renderer_reset_playback();
    }
    transfer_was_active = transfer_active;
    if (!g_state.desktop_device_id[0] && now_ms - last_hello_ms > 2000ULL) {
      pet_p4_send_hello(&g_state, transport_send_line, NULL);
      last_hello_ms = now_ms;
    }
    if (g_state.last_update_ms != last_logged_update) {
      pet_p4_view_model_t view;
      pet_p4_build_view_model(&g_state, &view);
      ESP_LOGI(
        TAG,
        "view page=%s agent=%s status=%d compact=%d title=%s body=%s",
        view.page,
        view.agent,
        (int) view.status,
        view.compact_bubble ? 1 : 0,
        view.title ? view.title : "",
        view.body ? view.body : ""
      );
      last_logged_update = g_state.last_update_ms;
    }
    if (lcd_err == ESP_OK && now_ms - last_lcd_keepalive_ms >= 1000ULL) {
      keep_lcd_awake = true;
      last_lcd_keepalive_ms = now_ms;
    }
    if (lcd_err == ESP_OK && transfer_active
        && now_ms - last_transfer_render_ms >= 500ULL) {
      render_transfer_frame = true;
      last_transfer_render_ms = now_ms;
    } else if (lcd_err == ESP_OK && !transfer_active
        && now_ms - last_render_ms >= PET_P4_MAIN_RENDER_INTERVAL_MS) {
      memcpy(g_render_state, &g_state, sizeof(*g_render_state));
      pet_p4_build_view_model(g_render_state, &render_view);
      render_frame = true;
      last_render_ms = now_ms;
    }
    xSemaphoreGive(g_state_mutex);
    if (keep_lcd_awake) {
      esp_err_t keepalive_err = pet_p4_lcd_keep_awake();
      if (keepalive_err != ESP_OK) {
        ESP_LOGW(TAG, "LCD keepalive failed: %s", esp_err_to_name(keepalive_err));
      }
    }
    if (render_transfer_frame) {
      esp_err_t err;
      xSemaphoreTake(g_render_mutex, portMAX_DELAY);
      err = pet_p4_renderer_render_transfer_status(firmware_transfer_active, now_ms);
      xSemaphoreGive(g_render_mutex);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "transfer status render failed: %s", esp_err_to_name(err));
      }
    } else if (render_frame) {
      esp_err_t err;
      xSemaphoreTake(g_render_mutex, portMAX_DELAY);
      err = pet_p4_renderer_render(g_render_state, &render_view, now_ms);
      xSemaphoreGive(g_render_mutex);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "render failed: %s", esp_err_to_name(err));
        runtime_render_healthy = false;
        pet_p4_ota_runtime_failed(now_ms);
      } else {
        runtime_render_healthy = true;
      }
    }
    bool transports_healthy = rx_tasks_healthy();
    if (!transports_healthy && (!last_rx_health_warning_ms
        || now_ms - last_rx_health_warning_ms >= PET_P4_MAIN_RX_HEALTH_TIMEOUT_MS)) {
      ESP_LOGW(TAG, "transport RX health check failed");
      last_rx_health_warning_ms = now_ms;
    }
    if (lcd_err == ESP_OK && runtime_render_healthy && transports_healthy) {
      pet_p4_ota_runtime_ready(now_ms);
    } else {
      pet_p4_ota_runtime_failed(now_ms);
    }
    vTaskDelay(pdMS_TO_TICKS(PET_P4_MAIN_LOOP_DELAY_MS));
  }
}
