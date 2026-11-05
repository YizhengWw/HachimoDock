/* StopWatch target: existing HachimoDock JSONL topics, FreeRTOS and raw RGB565.
 * RX framing and rendering have different tasks; state has one owner.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_stopwatch.h"
#include "stopwatch_renderer.h"
#include "pet_device_sessions.h"
#include "pet_p4_input_core.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

enum { MAX_LINE = 16384, RX_QUEUE = 8 };
static QueueHandle_t lines;
static QueueHandle_t input_events;
static QueueHandle_t vibration_requests;
static pet_device_sessions_t model;
static char board_id[40], boot_id[17];
static uint64_t now_ms(void) { return esp_timer_get_time()/1000ULL; }
static void send_topic(const char *topic, cJSON *payload) {
  if (!payload) return;
  cJSON *root = cJSON_CreateObject();
  if (!root) { cJSON_Delete(payload); return; }
  cJSON_AddStringToObject(root,"topic",topic);
  cJSON_AddItemToObject(root,"payload",payload);
  char *text = cJSON_PrintUnformatted(root);
  if (text) {
    size_t n = strlen(text), sent = 0;
    uint64_t deadline = now_ms()+200;
    while (sent < n && now_ms() < deadline) {
      int written = usb_serial_jtag_write_bytes(text+sent,n-sent,pdMS_TO_TICKS(20));
      if (written > 0) sent += written;
    }
    // A failed partial write is delimited before the next frame; never replay it.
    (void)usb_serial_jtag_write_bytes("\n",1,pdMS_TO_TICKS(20));
    free(text);
  }
  cJSON_Delete(root);
}
static void hello(void) {
  const esp_app_desc_t *app = esp_app_get_description();
  cJSON *p = cJSON_CreateObject();
  cJSON_AddStringToObject(p,"boardDeviceId",board_id);
  cJSON_AddStringToObject(p,"runtime","esp-s3");
  cJSON_AddStringToObject(p,"deviceModel","M5Stack StopWatch v1.0");
  cJSON_AddStringToObject(p,"fw",app->version);
  cJSON_AddStringToObject(p,"buildId",HACHIMO_BUILD_ID);
  cJSON_AddStringToObject(p,"gitSha",HACHIMO_GIT_SHA);
  cJSON_AddBoolToObject(p,"buildDirty",HACHIMO_BUILD_DIRTY != 0);
  cJSON_AddNumberToObject(p,"protocolSchema",7);
  cJSON_AddStringToObject(p,"wireProtocol","pet-usb-jsonl-v3");
  cJSON *cap = cJSON_AddObjectToObject(p,"capabilities");
  cJSON_AddNumberToObject(cap,"version",1);
  cJSON_AddStringToObject(cap,"boardId","m5stack-stopwatch");
  cJSON_AddStringToObject(cap,"hardwareRevision","1.0");
  cJSON_AddStringToObject(cap,"debugBootId",boot_id);
  cJSON_AddBoolToObject(cap,"usbOnly",true);
  cJSON_AddBoolToObject(cap,"sessionSync",true);
  cJSON_AddBoolToObject(cap,"protocolAck",true);
  cJSON_AddBoolToObject(cap,"inputs",true);
  cJSON_AddBoolToObject(cap,"touch",stopwatch_board_touch_ready());
  cJSON_AddBoolToObject(cap,"inputConfig",false);
  cJSON_AddBoolToObject(cap,"voice",false);
  cJSON_AddBoolToObject(cap,"audio",false);
  cJSON_AddBoolToObject(cap,"widgets",false);
  cJSON_AddBoolToObject(cap,"firmwareOta",false);
  cJSON_AddNumberToObject(cap,"maxAppearanceSlots",0);
  cJSON_AddArrayToObject(cap,"assetFormats");
  cJSON *display = cJSON_AddObjectToObject(cap,"display");
  cJSON_AddNumberToObject(display,"width",466);
  cJSON_AddNumberToObject(display,"height",466);
  cJSON_AddStringToObject(display,"shape","round");
  cJSON_AddStringToObject(display,"pixelFormat","rgb565");
  cJSON *features = cJSON_AddObjectToObject(cap,"features");
  cJSON_AddBoolToObject(features,"mainPage",true);
  cJSON_AddBoolToObject(features,"voiceCapture",false);
  cJSON_AddBoolToObject(features,"audioPlayback",false);
  cJSON_AddBoolToObject(features,"widgets",false);
  cJSON_AddBoolToObject(features,"firmwareOta",false);
  send_topic("hello",p);
}
static void ack(const char *topic, const cJSON *payload, bool ok, const char *code) {
  cJSON *p = cJSON_CreateObject();
  cJSON_AddStringToObject(p,"requestTopic",topic);
  cJSON_AddStringToObject(p,"requestId",pet_device_json_string(payload,"requestId"));
  cJSON_AddBoolToObject(p,"ok",ok);
  if (code) cJSON_AddStringToObject(p,"code",code);
  send_topic("protocol/ack",p);
}
static void receive_line(const char *line) {
  cJSON *root = cJSON_ParseWithOpts(line,NULL,true);
  const char *topic = pet_device_json_string(root,"topic");
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(root,"payload");
  if (!topic[0] || !cJSON_IsObject(p)) {
    ack(topic,p,false,"invalid_message");
  } else if (!strcmp(topic,"hello") || !strcmp(topic,"ack") || !strcmp(topic,"bind")) {
    model.host_seen = true; model.last_host_ms = now_ms();
    hello();
  } else if (pet_device_sessions_handle(&model,topic,p,now_ms())) {
    if (pet_device_json_string(p,"requestId")[0]) ack(topic,p,true,NULL);
  } else if (!strcmp(topic,"system/heartbeat") || !strcmp(topic,"ping")) {
    model.host_seen = true; model.last_host_ms = now_ms();
    if (pet_device_json_string(p,"requestId")[0]) ack(topic,p,true,NULL);
  } else if (!strcmp(topic,"control/remote-cli-binding")
             || (!strcmp(topic,"control/screen-page") && !strcmp(pet_device_json_string(p,"page"),"main"))) {
    // The host owns routing. This merely acknowledges delivery, like P4.
    model.host_seen = true; model.last_host_ms = now_ms();
    ack(topic,p,true,NULL);
  } else if (!strcmp(topic,"diagnostics/query")) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply,"requestId",pet_device_json_string(p,"requestId"));
    cJSON_AddStringToObject(reply,"boardDeviceId",board_id);
    cJSON_AddStringToObject(reply,"debugBootId",boot_id);
    cJSON *runtime = cJSON_AddObjectToObject(reply,"runtime");
    cJSON_AddStringToObject(runtime,"projectName","hachimo_stopwatch");
    cJSON_AddNumberToObject(runtime,"freeHeap",esp_get_free_heap_size());
    cJSON_AddNumberToObject(runtime,"batteryMv",stopwatch_board_battery_mv());
    send_topic("diagnostics/status",reply);
  } else ack(topic,p,false,"unsupported_topic");
  cJSON_Delete(root);
}
static void rx_task(void *unused) {
  (void)unused;
  char *line = heap_caps_malloc(MAX_LINE,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!line) { vTaskDelete(NULL); return; }
  size_t used = 0;
  bool overflow = false;
  uint8_t bytes[512];
  for (;;) {
    int n = usb_serial_jtag_read_bytes(bytes,sizeof(bytes),pdMS_TO_TICKS(20));
    for (int i = 0; i < n; ++i) {
      if (bytes[i] == '\n') {
        if (!overflow && used) {
          line[used] = 0;
          char *copy = heap_caps_malloc(used+1,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if (copy) {
            memcpy(copy,line,used+1);
            if (xQueueSend(lines,&copy,0) != pdTRUE) free(copy);
          }
        }
        used = 0; overflow = false;
      } else if (!overflow) {
        if (used+1 == MAX_LINE) overflow = true;
        else line[used++] = bytes[i];
      }
    }
  }
}
static void select_session(int direction, const char *event) {
  if (!pet_device_sessions_select(&model,direction)) return;
  const pet_device_session_t *card = pet_device_sessions_selected(&model);
  cJSON *p = cJSON_CreateObject();
  cJSON_AddStringToObject(p,"boardDeviceId",board_id);
  cJSON_AddStringToObject(p,"action",direction < 0 ? "session_previous" : "session_next");
  cJSON_AddStringToObject(p,"event",event);
  cJSON_AddStringToObject(p,"context","main");
  cJSON_AddBoolToObject(p,"handledLocally",true);
  cJSON_AddStringToObject(p,"sessionId",card->id);
  cJSON_AddStringToObject(p,"sessionTitle",card->title);
  cJSON_AddNumberToObject(p,"sessionIndex",model.selected+1);
  cJSON_AddNumberToObject(p,"sessionCount",model.count);
  send_topic("input/event",p);
}
static void input_task(void *unused) {
  (void)unused;
  pet_p4_button_decoder_t buttons[2];
  for (unsigned i = 0; i < 2; ++i)
    pet_p4_button_decoder_init(&buttons[i],stopwatch_board_button(i),30,700);
  uint64_t previous = now_ms();
  int start_x = 0, last_x = 0;
  bool touching = false;
  uint64_t vibration_until = 0;
  for (;;) {
    uint64_t now = now_ms();
    uint32_t duration;
    if (xQueueReceive(vibration_requests,&duration,0) == pdTRUE) {
      (void)stopwatch_board_vibrate(true);
      vibration_until = now+duration;
    }
    if (vibration_until && now >= vibration_until) {
      (void)stopwatch_board_vibrate(false);
      vibration_until = 0;
    }
    for (unsigned i = 0; i < 2; ++i) {
      uint8_t events = pet_p4_button_decoder_update(&buttons[i],stopwatch_board_button(i),(uint32_t)(now-previous));
      int event = 0;
      if (events & PET_P4_BUTTON_EVENT_SHORT_PRESS) event = i ? -1 : 1;
      if (events & PET_P4_BUTTON_EVENT_LONG_PRESS) event = i ? -2 : 2;
      if (event) (void)xQueueSend(input_events,&event,0);
    }
    previous = now;
    int x,y;
    bool down = stopwatch_board_touch(&x,&y);
    if (down && !touching) start_x = x;
    if (down) last_x = x;
    if (!down && touching && abs(last_x-start_x) > 60) {
      int event = last_x < start_x ? 1 : -1;
      (void)xQueueSend(input_events,&event,0);
    }
    touching = down;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
void app_main(void) {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs);
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
  snprintf(board_id,sizeof(board_id),"stopwatch-%02x%02x%02x%02x%02x%02x",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  snprintf(boot_id,sizeof(boot_id),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
  usb_serial_jtag_driver_config_t usb = {.tx_buffer_size = 4096,.rx_buffer_size = 8192};
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
  lines = xQueueCreate(RX_QUEUE,sizeof(char *));
  input_events = xQueueCreate(8,sizeof(int));
  vibration_requests = xQueueCreate(1,sizeof(uint32_t));
  if (!lines || !input_events || !vibration_requests) abort();
  if (xTaskCreate(rx_task,"stopwatch-rx",4096,NULL,8,NULL) != pdPASS) abort();
  ESP_ERROR_CHECK(stopwatch_board_init());
  ESP_ERROR_CHECK(stopwatch_renderer_init());
  if (xTaskCreate(input_task,"stopwatch-input",4096,NULL,5,NULL) != pdPASS) abort();
  uint64_t next_frame = 0, next_hello = 0;
  uint64_t next_battery = 0;
  int battery = -1;
  char last_life[32] = "";
  for (;;) {
    uint64_t now = now_ms();
    char *line;
    for (unsigned i = 0; i < RX_QUEUE && xQueueReceive(lines,&line,0) == pdTRUE; ++i) {
      receive_line(line); free(line);
    }
    pet_device_sessions_tick(&model,now);
    int input;
    while (xQueueReceive(input_events,&input,0) == pdTRUE) {
      if (input == 2 || input == -2) (void)stopwatch_board_brightness(input == 2 ? 160 : 64);
      else select_session(input,"screen.navigation");
    }
    const char *life = pet_device_sessions_lifecycle(&model);
    if (strcmp(last_life,life)) {
      if (!strcmp(life,"done") || !strcmp(life,"error") || !strcmp(life,"waiting_user")) {
        uint32_t duration = 120;
        (void)xQueueOverwrite(vibration_requests,&duration);
      }
      pet_device_text_copy(last_life,sizeof(last_life),life);
    }
    if (now >= next_hello) { hello(); next_hello = now+5000; }
    if (now >= next_battery) { battery = stopwatch_board_battery_mv(); next_battery = now+5000; }
    if (now >= next_frame) {
      esp_err_t err = stopwatch_renderer_draw(&model,now,battery);
      if (err != ESP_OK) ESP_LOGW("stopwatch","display: %s",esp_err_to_name(err));
      next_frame = now+125;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
