#include "pet_p4_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"

static const char *TAG = "pet-wifi";
#define PET_WIFI_NVS_NAMESPACE "pet_wifi"
#define PET_WIFI_TCP_PORT 3232
#define PET_WIFI_TCP_CLIENT_QUEUE 1

static pet_p4_wifi_callbacks_t s_callbacks;
static SemaphoreHandle_t s_send_mutex;
static volatile int s_client_fd = -1;
static volatile bool s_started = false;
static volatile bool s_ready = false;
static volatile bool s_has_credentials = false;
static char s_c6_version[16] = "unknown";
static bool s_c6_compatible = false;
static char s_ip[16];
static int s_rssi = 0;

static void query_c6_version(void);

static bool load_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
  nvs_handle_t handle;
  if (nvs_open(PET_WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t len = ssid_size;
  esp_err_t ssid_err = nvs_get_str(handle, "ssid", ssid, &len);
  len = password_size;
  esp_err_t pass_err = nvs_get_str(handle, "password", password, &len);
  nvs_close(handle);
  return ssid_err == ESP_OK && pass_err == ESP_OK && ssid[0] != '\0';
}

static esp_err_t save_credentials(const char *ssid, const char *password)
{
  nvs_handle_t handle;
  esp_err_t err = nvs_open(PET_WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_str(handle, "ssid", ssid);
  if (err == ESP_OK) err = nvs_set_str(handle, "password", password);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

static void apply_credentials_to_driver(const char *ssid, const char *password)
{
  s_has_credentials = true;
  wifi_config_t config = { 0 };
  strncpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid) - 1);
  strncpy((char *)config.sta.password, password, sizeof(config.sta.password) - 1);
  config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  config.sta.pmf_cfg.capable = true;
  config.sta.pmf_cfg.required = false;
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set config failed: %s", esp_err_to_name(err));
    return;
  }
  if (s_started) {
    (void) esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK) ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
  }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
  (void) arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (s_has_credentials) {
      ESP_LOGI(TAG, "STA started, connecting");
      (void) esp_wifi_connect();
    } else {
      ESP_LOGI(TAG, "STA started; unconfigured (use wifi/config)");
    }
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *) data;
    s_ready = false;
    s_rssi = 0;
    ESP_LOGW(TAG, "disconnected reason=%d, retrying", event ? event->reason : -1);
    (void) esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_ready = true;
    ESP_LOGI(TAG, "got ip %s", s_ip);
    wifi_ap_record_t ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      s_rssi = ap.rssi;
      ESP_LOGI(TAG, "AP '%s' rssi=%d ch=%u", ap.ssid, ap.rssi, ap.primary);
    }
    query_c6_version();
  }
}

static void send_bytes_or_fail(const char *data, size_t len)
{
  int fd = s_client_fd;
  if (fd < 0) return;
  if (s_send_mutex) xSemaphoreTake(s_send_mutex, portMAX_DELAY);
  size_t offset = 0;
  while (offset < len) {
    int n = send(fd, data + offset, len - offset, 0);
    if (n <= 0) {
      ESP_LOGW(TAG, "send failed: errno=%d", errno);
      break;
    }
    offset += (size_t) n;
  }
  if (s_send_mutex) xSemaphoreGive(s_send_mutex);
}

bool pet_p4_wifi_send_bytes(const char *data, size_t len)
{
  if (!data || len == 0 || s_client_fd < 0) return false;
  send_bytes_or_fail(data, len);
  return true;
}

bool pet_p4_wifi_send_line(const char *line)
{
  if (!line || s_client_fd < 0) return false;
  size_t len = strlen(line);
  send_bytes_or_fail(line, len);
  send_bytes_or_fail("\n", 1);
  return true;
}

static void wifi_server_task(void *arg)
{
  (void) arg;
  int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listen_fd < 0) {
    ESP_LOGE(TAG, "socket failed: errno=%d", errno);
    vTaskDelete(NULL);
    return;
  }
  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = { 0 };
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(PET_WIFI_TCP_PORT);
  if (bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "bind failed: errno=%d", errno);
    close(listen_fd);
    vTaskDelete(NULL);
    return;
  }
  listen(listen_fd, PET_WIFI_TCP_CLIENT_QUEUE);
  ESP_LOGI(TAG, "TCP server listening on port %d", PET_WIFI_TCP_PORT);

  while (true) {
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int fd = accept(listen_fd, (struct sockaddr *) &peer, &peer_len);
    if (fd < 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    struct timeval send_timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    struct timeval recv_timeout = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
    s_client_fd = fd;
    ESP_LOGI(TAG, "client connected: %s", inet_ntoa(peer.sin_addr));
    if (s_callbacks.on_client_connected) {
      ESP_LOGI(TAG, "sending hello to Wi-Fi client");
      s_callbacks.on_client_connected(s_callbacks.ctx);
      ESP_LOGI(TAG, "hello callback returned");
    }

    uint8_t buffer[1024];
    while (true) {
      int n = recv(fd, buffer, sizeof(buffer), 0);
      if (n <= 0) {
        ESP_LOGI(TAG, "client recv end n=%d errno=%d", n, errno);
        break;
      }
      if (s_callbacks.on_bytes) {
        s_callbacks.on_bytes(buffer, (size_t) n, s_callbacks.ctx);
      }
    }
    ESP_LOGI(TAG, "client disconnected");
    close(fd);
    s_client_fd = -1;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static void wifi_boot_task(void *arg)
{
  (void) arg;
  if (s_started) {
    vTaskDelete(NULL);
    return;
  }
  char ssid[33] = { 0 };
  char password[65] = { 0 };
  bool has_credentials = load_credentials(ssid, sizeof(ssid), password, sizeof(password));

  s_send_mutex = xSemaphoreCreateMutex();
  if (!s_send_mutex) {
    ESP_LOGE(TAG, "no mutex");
    vTaskDelete(NULL);
    return;
  }
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
    return;
  }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
    return;
  }
  esp_netif_create_default_wifi_sta();

  // ESP32-P4 has no radio; the C6 link must be brought up through ESP-Hosted
  // before esp_wifi_init() (which is implemented by esp_wifi_remote).
  esp_err_t hosted_err = esp_hosted_init();
  if (hosted_err != ESP_OK && hosted_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_hosted_init: %s", esp_err_to_name(hosted_err));
    vTaskDelete(NULL);
    return;
  }

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&init);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
    return;
  }
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err == ESP_OK && !has_credentials) {
    // The C6 co-processor keeps its own last station config across P4 reflashes.
    // Do not inherit it when this host has no stored credentials.
    wifi_config_t empty = { 0 };
    (void) esp_wifi_set_config(WIFI_IF_STA, &empty);
  }
  if (err == ESP_OK) err = esp_wifi_start();
  if (err == ESP_OK) esp_wifi_set_ps(WIFI_PS_NONE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
    return;
  }
  for (int attempt = 0; attempt < 5; attempt += 1) {
    query_c6_version();
    if (strcmp(s_c6_version, "unknown") != 0) break;
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  s_started = true;
  s_has_credentials = has_credentials;
  xTaskCreate(wifi_server_task, "pet_wifi_srv", 16384, NULL, 6, NULL);
  if (has_credentials) {
    apply_credentials_to_driver(ssid, password);
  } else {
    ESP_LOGI(TAG, "Wi-Fi ready; no credentials (scan/config available over USB/UART)");
  }
  vTaskDelete(NULL);
}

esp_err_t pet_p4_wifi_init(const pet_p4_wifi_callbacks_t *callbacks)
{
  if (callbacks) s_callbacks = *callbacks;
  if (xTaskCreate(wifi_boot_task, "pet_wifi_boot", 8192, NULL, 5, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

bool pet_p4_wifi_client_connected(void)
{
  return s_client_fd >= 0;
}

bool pet_p4_wifi_ready(void)
{
  return s_ready;
}

static bool wait_started(uint32_t timeout_ms)
{
  TickType_t start = xTaskGetTickCount();
  while (!s_started && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return s_started;
}

static void send_json(cJSON *root, pet_p4_send_line_fn send_line, void *reply_ctx)
{
  if (!root || !send_line) return;
  char *json = cJSON_PrintUnformatted(root);
  if (json) {
    send_line(json, reply_ctx);
    free(json);
  }
  cJSON_Delete(root);
}

static void send_wifi_error(const char *topic, esp_err_t err, pet_p4_send_line_fn send_line, void *reply_ctx)
{
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "topic", topic);
  cJSON *payload = cJSON_AddObjectToObject(root, "payload");
  cJSON_AddBoolToObject(payload, "ok", false);
  cJSON_AddStringToObject(payload, "error", esp_err_to_name(err));
  send_json(root, send_line, reply_ctx);
}

static void send_scan_result(pet_p4_send_line_fn send_line, void *reply_ctx)
{
  if (!wait_started(3000)) {
    send_wifi_error("wifi/scan_result", ESP_ERR_INVALID_STATE, send_line, reply_ctx);
    return;
  }
  wifi_scan_config_t scan = { 0 };
  esp_err_t err = esp_wifi_scan_start(&scan, true);
  if (err != ESP_OK) {
    send_wifi_error("wifi/scan_result", err, send_line, reply_ctx);
    return;
  }
  uint16_t total = 0;
  esp_wifi_scan_get_ap_num(&total);
  uint16_t count = total;
  if (count > 20) count = 20;
  wifi_ap_record_t *records = count ? calloc(count, sizeof(*records)) : NULL;
  uint16_t got = count;
  if (!records || (count && esp_wifi_scan_get_ap_records(&got, records) != ESP_OK)) {
    free(records);
    send_wifi_error("wifi/scan_result", ESP_ERR_NO_MEM, send_line, reply_ctx);
    return;
  }
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "topic", "wifi/scan_result");
  cJSON *payload = cJSON_AddObjectToObject(root, "payload");
  cJSON_AddBoolToObject(payload, "ok", true);
  cJSON_AddNumberToObject(payload, "count", got);
  cJSON *aps = cJSON_AddArrayToObject(payload, "aps");
  for (uint16_t i = 0; i < got; i += 1) {
    cJSON *ap = cJSON_CreateObject();
    char ssid[33] = { 0 };
    memcpy(ssid, records[i].ssid, sizeof(records[i].ssid));
    cJSON_AddStringToObject(ap, "ssid", ssid);
    cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
    cJSON_AddNumberToObject(ap, "channel", records[i].primary);
    cJSON_AddNumberToObject(ap, "auth", records[i].authmode);
    cJSON_AddItemToArray(aps, ap);
  }
  free(records);
  send_json(root, send_line, reply_ctx);
}

static void reset_credentials(pet_p4_send_line_fn send_line, void *reply_ctx)
{
  nvs_handle_t handle;
  esp_err_t err = nvs_open(PET_WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    (void) nvs_erase_key(handle, "ssid");
    (void) nvs_erase_key(handle, "password");
    err = nvs_commit(handle);
    nvs_close(handle);
  }
  s_has_credentials = false;
  if (s_started) {
    (void) esp_wifi_disconnect();
    wifi_config_t empty = { 0 };
    (void) esp_wifi_set_config(WIFI_IF_STA, &empty);
    s_ready = false;
    s_ip[0] = '\0';
    s_rssi = 0;
  }
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "topic", "wifi/reset_result");
  cJSON *payload = cJSON_AddObjectToObject(root, "payload");
  cJSON_AddBoolToObject(payload, "ok", err == ESP_OK);
  if (err != ESP_OK) cJSON_AddStringToObject(payload, "error", esp_err_to_name(err));
  send_json(root, send_line, reply_ctx);
}

static void query_c6_version(void)
{
  esp_hosted_coprocessor_fwver_t ver = {0};
  if (esp_hosted_get_coprocessor_fwversion(&ver) != 0) return;
  snprintf(
    s_c6_version,
    sizeof(s_c6_version),
    "%u.%u.%u",
    (unsigned) ver.major1,
    (unsigned) ver.minor1,
    (unsigned) ver.patch1
  );
  s_c6_compatible = (ver.major1 > 2)
    || (ver.major1 == 2 && ver.minor1 > 12)
    || (ver.major1 == 2 && ver.minor1 == 12 && ver.patch1 >= 11);
  ESP_LOGI(TAG, "C6 firmware %s compatible=%d", s_c6_version, (int) s_c6_compatible);
}

void pet_p4_wifi_c6_version(char *out, size_t out_len)
{
  if (!out || out_len == 0) return;
  strncpy(out, s_c6_version, out_len - 1);
  out[out_len - 1] = '\0';
}

bool pet_p4_wifi_tcp_compatible(void)
{
  return s_c6_compatible;
}

static void send_status(pet_p4_send_line_fn send_line, void *reply_ctx)
{
  if (s_ready && strcmp(s_c6_version, "unknown") == 0) {
    query_c6_version();
  }
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "topic", "wifi/status_result");
  cJSON *payload = cJSON_AddObjectToObject(root, "payload");
  cJSON_AddBoolToObject(payload, "ok", true);
  cJSON_AddBoolToObject(payload, "connected", s_ready);
  cJSON_AddBoolToObject(payload, "clientConnected", s_client_fd >= 0);
  cJSON_AddStringToObject(payload, "ip", s_ip);
  cJSON_AddNumberToObject(payload, "rssi", s_rssi);
  cJSON_AddStringToObject(payload, "c6Version", s_c6_version);
  cJSON_AddBoolToObject(payload, "wifiTcpCompatible", s_c6_compatible);
  char *json = cJSON_PrintUnformatted(root);
  if (json) {
    send_line(json, reply_ctx);
    free(json);
  }
  cJSON_Delete(root);
}

bool pet_p4_wifi_handle_command(const char *line, pet_p4_send_line_fn send_line, void *reply_ctx)
{
  if (!line || !send_line || !strstr(line, "\"wifi/")) return false;
  cJSON *root = cJSON_Parse(line);
  if (!root) return false;
  const cJSON *topic = cJSON_GetObjectItemCaseSensitive(root, "topic");
  if (!cJSON_IsString(topic)) {
    cJSON_Delete(root);
    return false;
  }
  bool handled = true;
  if (strcmp(topic->valuestring, "wifi/status") == 0) {
    send_status(send_line, reply_ctx);
  } else if (strcmp(topic->valuestring, "wifi/scan") == 0) {
    send_scan_result(send_line, reply_ctx);
  } else if (strcmp(topic->valuestring, "wifi/reset") == 0) {
    reset_credentials(send_line, reply_ctx);
  } else if (strcmp(topic->valuestring, "wifi/config") == 0) {
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(payload, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(payload, "password");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid) && cJSON_IsString(password) && ssid->valuestring[0] != '\0') {
      err = save_credentials(ssid->valuestring, password->valuestring);
      if (err == ESP_OK) {
        if (s_started) {
          apply_credentials_to_driver(ssid->valuestring, password->valuestring);
        } else if (xTaskCreate(wifi_boot_task, "pet_wifi_boot", 8192, NULL, 5, NULL) != pdPASS) {
          err = ESP_ERR_NO_MEM;
        }
      }
    }
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "topic", "wifi/config_result");
    cJSON *body = cJSON_AddObjectToObject(response, "payload");
    cJSON_AddBoolToObject(body, "ok", err == ESP_OK);
    cJSON_AddStringToObject(body, "error", err == ESP_OK ? "" : esp_err_to_name(err));
    char *json = cJSON_PrintUnformatted(response);
    if (json) {
      send_line(json, reply_ctx);
      free(json);
    }
    cJSON_Delete(response);
  } else {
    handled = false;
  }
  cJSON_Delete(root);
  return handled;
}
