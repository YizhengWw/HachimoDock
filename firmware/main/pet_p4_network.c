/*
 * [Input] See pet_p4_network.h.
 * [Output] Lazily started Wi-Fi station over the ESP32-C6, one authenticated TCP JSONL client and
 *          UDP discovery, with crash-loop and failure containment so a bad radio never costs USB.
 * [Pos] ESP32-P4 Wi-Fi transport node beside the UART, USB Serial/JTAG and native USB links.
 * [Sync] If this file changes, update `pet_p4_network.h`, `protocol.md`, and `firmware/.folder.md`.
 */

#include "pet_p4_network.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "nvs.h"

static const char *TAG = "pet-net";

#define PET_P4_NET_NVS_NAMESPACE "p4net"
#define PET_P4_NET_TASK_STACK_BYTES 8192
#define PET_P4_NET_TASK_PRIORITY 5
/* Past the 8 s OTA validation window: a stack that crashes on first use is rolled back by the
 * bootloader instead of becoming a boot loop on the freshly installed image. */
#define PET_P4_NET_AUTOSTART_DELAY_MS 12000ULL
#define PET_P4_NET_TICK_MS 50
#define PET_P4_NET_SEND_BUDGET_MS 100
#define PET_P4_NET_HANDSHAKE_LINE_MAX 1024
#define PET_P4_NET_RX_CHUNK 1024
#define PET_P4_NET_HOSTED_ATTEMPTS 3

typedef enum {
  NET_UNCONFIGURED = 0,
  NET_IDLE,
  NET_STARTING,
  NET_CONNECTING,
  NET_CONNECTED,
  NET_FAILED,
} net_state_t;

static const char *const k_state_names[] = {
  "unconfigured", "idle", "starting", "connecting", "connected", "failed",
};

typedef struct {
  int fd;
  char line[PET_P4_NET_HANDSHAKE_LINE_MAX];
  size_t used;
  uint64_t accepted_ms;
  char device_nonce[PET_P4_NET_NONCE_HEX_LEN + 1];
} net_pending_t;

typedef struct {
  SemaphoreHandle_t lock;        /* guards the status fields below */
  SemaphoreHandle_t client_lock; /* serializes sends and closes of the authenticated client */
  pet_p4_network_config_t config;
  net_state_t state;
  char ssid[PET_P4_NET_SSID_MAX + 1];
  char password[PET_P4_NET_PASSWORD_MAX + 1];
  char secret[PET_P4_NET_SECRET_LEN + 1];
  bool configured;
  bool stack_started;
  bool reconfigure;
  bool disconnect_request;
  bool close_sockets_request;
  bool sockets_wanted;
  int rssi;
  bool rssi_valid;
  uint64_t rssi_refresh_ms;
  bool crash_loop;
  uint32_t attempts;
  uint64_t next_attempt_ms;
  uint64_t not_before_ms;
  int disconnect_reason;
  char last_error[24];
  char ip[16];
  char cp_version[24];
  int listen_fd;
  int udp_fd;
  int client_fd;
  bool client_authed;
  bool client_drop;
  uint64_t client_last_rx_ms;
  uint64_t client_last_tx_ms;
  net_pending_t pending;
  pet_p4_net_auth_guard_t guard;
  esp_netif_t *netif;
} net_ctx_t;

static net_ctx_t s;

/* The ESP-Hosted host component registers a constructor that initializes the SDIO transport and
 * resets the C6 at every boot, and aborts on failure. The build wraps esp_hosted_init so that boot
 * path becomes a no-op; the real initialization runs here, only after Wi-Fi was configured. */
extern int __real_esp_hosted_init(void);
int __wrap_esp_hosted_init(void) {
  return 0;
}

static uint64_t now_ms(void) {
  return (uint64_t) (esp_timer_get_time() / 1000ULL);
}

static void lock(void) {
  xSemaphoreTake(s.lock, portMAX_DELAY);
}

static void unlock(void) {
  xSemaphoreGive(s.lock);
}

static void set_error(const char *code) {
  snprintf(s.last_error, sizeof(s.last_error), "%s", code ? code : "");
}

/* ---- persistence ------------------------------------------------------------------------- */

static esp_err_t nvs_read_string(nvs_handle_t handle, const char *key, char *out, size_t out_size) {
  size_t size = out_size;
  out[0] = '\0';
  esp_err_t err = nvs_get_str(handle, key, out, &size);
  if (err != ESP_OK) out[0] = '\0';
  return err;
}

static uint8_t boot_try_read(void) {
  nvs_handle_t handle;
  uint8_t value = 0;
  if (nvs_open(PET_P4_NET_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;
  if (nvs_get_u8(handle, "boot_try", &value) != ESP_OK) value = 0;
  nvs_close(handle);
  return value;
}

static void boot_try_write(uint8_t value) {
  nvs_handle_t handle;
  if (nvs_open(PET_P4_NET_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
  if (nvs_set_u8(handle, "boot_try", value) == ESP_OK) nvs_commit(handle);
  nvs_close(handle);
}

static esp_err_t credentials_store(const char *ssid, const char *password, const char *secret) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(PET_P4_NET_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_str(handle, "ssid", ssid);
  if (err == ESP_OK) err = nvs_set_str(handle, "pass", password);
  if (err == ESP_OK) err = nvs_set_str(handle, "secret", secret);
  if (err == ESP_OK) err = nvs_set_u8(handle, "boot_try", 0);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

static void credentials_erase(void) {
  nvs_handle_t handle;
  if (nvs_open(PET_P4_NET_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
  nvs_erase_all(handle);
  nvs_commit(handle);
  nvs_close(handle);
}

static void credentials_load(void) {
  nvs_handle_t handle;
  s.configured = false;
  if (nvs_open(PET_P4_NET_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
  bool ok = nvs_read_string(handle, "ssid", s.ssid, sizeof(s.ssid)) == ESP_OK
    && nvs_read_string(handle, "pass", s.password, sizeof(s.password)) == ESP_OK
    && nvs_read_string(handle, "secret", s.secret, sizeof(s.secret)) == ESP_OK;
  nvs_close(handle);
  s.configured = ok && pet_p4_net_credentials_valid(s.ssid, s.password) && pet_p4_net_secret_valid(s.secret);
  if (!s.configured) {
    memset(s.ssid, 0, sizeof(s.ssid));
    memset(s.password, 0, sizeof(s.password));
    memset(s.secret, 0, sizeof(s.secret));
  }
}

/* ---- sockets ----------------------------------------------------------------------------- */

static void fd_close(int *fd) {
  if (*fd >= 0) {
    shutdown(*fd, SHUT_RDWR);
    close(*fd);
    *fd = -1;
  }
}

static void fd_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool send_all(int fd, const char *data, size_t len, uint32_t budget_ms) {
  uint64_t deadline = now_ms() + budget_ms;
  size_t offset = 0;
  while (offset < len) {
    int sent = send(fd, data + offset, len - offset, 0);
    if (sent > 0) {
      offset += (size_t) sent;
      continue;
    }
    if (sent < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      uint64_t now = now_ms();
      if (now >= deadline) return false;
      fd_set writable;
      FD_ZERO(&writable);
      FD_SET(fd, &writable);
      struct timeval wait = {.tv_sec = 0, .tv_usec = (suseconds_t) ((deadline - now) * 1000)};
      if (select(fd + 1, NULL, &writable, NULL, &wait) <= 0) return false;
      continue;
    }
    return false;
  }
  return true;
}

static bool send_json(int fd, const char *topic, cJSON *payload) {
  cJSON *root = cJSON_CreateObject();
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return false;
  }
  cJSON_AddStringToObject(root, "topic", topic);
  cJSON_AddItemToObject(root, "payload", payload);
  char *line = cJSON_PrintUnformatted(root);
  bool ok = line && send_all(fd, line, strlen(line), PET_P4_NET_SEND_BUDGET_MS)
    && send_all(fd, "\n", 1, PET_P4_NET_SEND_BUDGET_MS);
  cJSON_free(line);
  cJSON_Delete(root);
  return ok;
}

static void client_close_locked(void) {
  bool was_authed = s.client_authed;
  fd_close(&s.client_fd);
  s.client_authed = false;
  s.client_drop = false;
  if (was_authed && s.config.on_client) s.config.on_client(false, s.config.ctx);
}

static void sockets_close(void) {
  xSemaphoreTake(s.client_lock, portMAX_DELAY);
  client_close_locked();
  xSemaphoreGive(s.client_lock);
  fd_close(&s.pending.fd);
  fd_close(&s.listen_fd);
  fd_close(&s.udp_fd);
}

static bool sockets_open(void) {
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PET_P4_NET_TCP_PORT)};
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  int reuse = 1;
  s.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s.listen_fd < 0) return false;
  setsockopt(s.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (bind(s.listen_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(s.listen_fd, 2) != 0) {
    ESP_LOGW(TAG, "TCP listen failed: errno %d", errno);
    fd_close(&s.listen_fd);
    return false;
  }
  fd_nonblocking(s.listen_fd);
  s.udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (s.udp_fd >= 0) {
    if (bind(s.udp_fd, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
      fd_nonblocking(s.udp_fd);
    } else {
      fd_close(&s.udp_fd);
    }
  }
  return true;
}

/* ---- status ------------------------------------------------------------------------------ */

static void fill_status(cJSON *payload) {
  cJSON_AddStringToObject(payload, "boardDeviceId", s.config.board_device_id);
  cJSON_AddBoolToObject(payload, "supported", true);
  cJSON_AddBoolToObject(payload, "configured", s.configured);
  cJSON_AddBoolToObject(payload, "paired", s.configured);
  cJSON_AddStringToObject(payload, "state", k_state_names[s.state]);
  if (s.configured) cJSON_AddStringToObject(payload, "ssid", s.ssid);
  cJSON_AddNumberToObject(payload, "port", PET_P4_NET_TCP_PORT);
  cJSON_AddStringToObject(payload, "protocol", PET_P4_NET_WIRE_PROTOCOL);
  cJSON_AddBoolToObject(payload, "client", s.client_authed);
  if (s.state == NET_CONNECTED) {
    cJSON_AddStringToObject(payload, "ip", s.ip);
    /* Cached by the network task: asking the C6 here would hold the protocol mutex over SDIO. */
    if (s.rssi_valid) cJSON_AddNumberToObject(payload, "rssi", s.rssi);
  }
  if (s.last_error[0]) cJSON_AddStringToObject(payload, "lastError", s.last_error);
  if (s.disconnect_reason) cJSON_AddNumberToObject(payload, "disconnectReason", s.disconnect_reason);
  if (s.cp_version[0]) cJSON_AddStringToObject(payload, "coprocessorVersion", s.cp_version);
  cJSON_AddNumberToObject(payload, "attempts", s.attempts);
}

static void reply_status(
  pet_p4_send_line_fn send_line, void *ctx, const cJSON *request, bool ok, const char *error
) {
  cJSON *payload = cJSON_CreateObject();
  cJSON *root = cJSON_CreateObject();
  if (!payload || !root) {
    cJSON_Delete(payload);
    cJSON_Delete(root);
    return;
  }
  const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(request, "requestId");
  if (cJSON_IsString(request_id)) cJSON_AddStringToObject(payload, "requestId", request_id->valuestring);
  cJSON_AddBoolToObject(payload, "ok", ok);
  if (error && error[0]) cJSON_AddStringToObject(payload, "error", error);
  lock();
  fill_status(payload);
  unlock();
  cJSON_AddStringToObject(root, "topic", "network/status");
  cJSON_AddItemToObject(root, "payload", payload);
  char *line = cJSON_PrintUnformatted(root);
  if (line && send_line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

void pet_p4_network_describe(cJSON *capabilities) {
  cJSON *network = cJSON_AddObjectToObject(capabilities, "network");
  if (!network) return;
  cJSON_AddBoolToObject(network, "wifi", true);
  cJSON_AddStringToObject(network, "protocol", PET_P4_NET_WIRE_PROTOCOL);
  cJSON_AddNumberToObject(network, "port", PET_P4_NET_TCP_PORT);
  cJSON_AddStringToObject(network, "provisioning", "usb");
  lock();
  cJSON_AddBoolToObject(network, "configured", s.configured);
  cJSON_AddStringToObject(network, "state", k_state_names[s.state]);
  unlock();
}

/* ---- station ----------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void) arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *) data;
    lock();
    s.disconnect_reason = event ? event->reason : 0;
    set_error(pet_p4_net_disconnect_error(s.disconnect_reason));
    s.ip[0] = '\0';
    s.sockets_wanted = false;
    if (s.configured && !s.disconnect_request) {
      s.state = NET_FAILED;
      s.next_attempt_ms = now_ms() + pet_p4_net_backoff_ms(s.attempts);
      s.attempts += 1;
    }
    unlock();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *) data;
    lock();
    snprintf(s.ip, sizeof(s.ip), IPSTR, IP2STR(&event->ip_info.ip));
    s.state = NET_CONNECTED;
    s.attempts = 0;
    s.disconnect_reason = 0;
    set_error("");
    s.sockets_wanted = true;
    unlock();
    ESP_LOGI(TAG, "station connected, address " IPSTR, IP2STR(&event->ip_info.ip));
  } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
    lock();
    s.ip[0] = '\0';
    s.sockets_wanted = false;
    if (s.state == NET_CONNECTED) s.state = NET_CONNECTING;
    unlock();
  }
}

static esp_err_t apply_station_config(void) {
  wifi_config_t config = {0};
  memcpy(config.sta.ssid, s.ssid, strlen(s.ssid));
  memcpy(config.sta.password, s.password, strlen(s.password));
  config.sta.threshold.authmode = s.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  config.sta.pmf_cfg.capable = true;
  config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
  memset(&config, 0, sizeof(config));
  return err;
}

static esp_err_t stack_bring_up(void) {
  esp_err_t err = ESP_FAIL;
  for (int attempt = 0; attempt < PET_P4_NET_HOSTED_ATTEMPTS; ++attempt) {
    if (__real_esp_hosted_init() == 0) {
      err = ESP_OK;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  if (err != ESP_OK) return err;
  err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  if (!s.netif) {
    s.netif = esp_netif_create_default_wifi_sta();
    if (!s.netif) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
  }
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&init);
  if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) return err;
  (void) esp_wifi_set_storage(WIFI_STORAGE_RAM);
  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) return err;
  err = apply_station_config();
  if (err != ESP_OK) return err;
  err = esp_wifi_start();
  if (err != ESP_OK) return err;
  esp_hosted_coprocessor_fwver_t version = {0};
  if (esp_hosted_get_coprocessor_fwversion(&version) == 0) {
    lock();
    snprintf(s.cp_version, sizeof(s.cp_version), "%u.%u.%u",
             (unsigned) version.major1, (unsigned) version.minor1, (unsigned) version.patch1);
    unlock();
  }
  return ESP_OK;
}

static esp_err_t stack_start(void) {
  /* Count the attempt first: a crash anywhere in the hosted/Wi-Fi bring-up leaves the counter set,
   * and PET_P4_NET_CRASH_LOOP_LIMIT consecutive crashes disable the radio until the next config.
   * Any return, success or error, proves the firmware survived and clears it. */
  boot_try_write((uint8_t) (boot_try_read() + 1));
  esp_err_t err = stack_bring_up();
  boot_try_write(0);
  return err;
}

static void rssi_refresh(uint64_t now) {
  lock();
  bool connected = s.state == NET_CONNECTED;
  bool due = now >= s.rssi_refresh_ms;
  unlock();
  if (!connected || !due) return;
  wifi_ap_record_t ap;
  bool ok = esp_wifi_sta_get_ap_info(&ap) == ESP_OK; /* SDIO RPC: never under a lock */
  lock();
  s.rssi_valid = ok;
  if (ok) s.rssi = ap.rssi;
  s.rssi_refresh_ms = now + 5000;
  unlock();
}

static void begin_connect(void) {
  lock();
  s.state = NET_CONNECTING;
  s.disconnect_request = false;
  unlock();
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    lock();
    s.state = NET_FAILED;
    set_error("connect_failed");
    s.next_attempt_ms = now_ms() + pet_p4_net_backoff_ms(s.attempts++);
    unlock();
  }
}

static void supervisor_step(void) {
  uint64_t now = now_ms();
  lock();
  bool configured = s.configured;
  bool reconfigure = s.reconfigure;
  bool disconnect = s.disconnect_request;
  bool close_sockets = s.close_sockets_request;
  bool started = s.stack_started;
  net_state_t state = s.state;
  bool due = now >= s.next_attempt_ms;
  bool allowed = now >= s.not_before_ms && !s.crash_loop;
  s.reconfigure = false;
  s.close_sockets_request = false;
  unlock();

  /* Sockets belong to this task: other tasks only raise a request so no fd is closed under select. */
  if (close_sockets || disconnect) sockets_close();
  if (disconnect) {
    if (started) (void) esp_wifi_disconnect();
    lock();
    s.disconnect_request = false;
    unlock();
    return;
  }
  rssi_refresh(now);
  if (!configured || !allowed) return;
  if (!started) {
    if (!due) return;
    lock();
    s.state = NET_STARTING;
    set_error("");
    unlock();
    esp_err_t err = stack_start();
    lock();
    if (err == ESP_OK) {
      s.stack_started = true;
      s.state = NET_CONNECTING;
    } else {
      s.state = NET_FAILED;
      set_error("c6_unavailable");
      s.next_attempt_ms = now + pet_p4_net_backoff_ms(s.attempts++);
      ESP_LOGW(TAG, "wireless stack unavailable: %s", esp_err_to_name(err));
    }
    unlock();
    if (err == ESP_OK) begin_connect();
    return;
  }
  if (reconfigure) {
    (void) esp_wifi_disconnect();
    if (apply_station_config() == ESP_OK) begin_connect();
    return;
  }
  if (state == NET_FAILED && due) begin_connect();
}

/* ---- client handling --------------------------------------------------------------------- */

static bool hmac_hex(const char *secret, const char *message, char out[PET_P4_NET_PROOF_HEX_LEN + 1]) {
  uint8_t mac[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  int rc = mbedtls_md_hmac(
    info, (const unsigned char *) secret, strlen(secret),
    (const unsigned char *) message, strlen(message), mac);
  if (rc != 0) return false;
  pet_p4_net_hex_encode(mac, sizeof(mac), out);
  return true;
}

static void pending_close(void) {
  fd_close(&s.pending.fd);
  memset(&s.pending.line, 0, sizeof(s.pending.line));
  s.pending.used = 0;
}

static void pending_accept(void) {
  struct sockaddr_in peer;
  socklen_t peer_len = sizeof(peer);
  int fd = accept(s.listen_fd, (struct sockaddr *) &peer, &peer_len);
  if (fd < 0) return;
  uint64_t now = now_ms();
  if (pet_p4_net_auth_locked(&s.guard, now)) {
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
      cJSON_AddStringToObject(payload, "error", "locked");
      fd_nonblocking(fd);
      send_json(fd, "net/auth_error", payload);
    }
    close(fd);
    return;
  }
  pending_close(); /* a newer attempt replaces a stalled one */
  fd_nonblocking(fd);
  int nodelay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  s.pending.fd = fd;
  s.pending.accepted_ms = now;
  uint8_t random[PET_P4_NET_NONCE_HEX_LEN / 2];
  esp_fill_random(random, sizeof(random));
  pet_p4_net_hex_encode(random, sizeof(random), s.pending.device_nonce);
  cJSON *payload = cJSON_CreateObject();
  if (payload) {
    cJSON_AddStringToObject(payload, "boardDeviceId", s.config.board_device_id);
    cJSON_AddStringToObject(payload, "nonce", s.pending.device_nonce);
    cJSON_AddStringToObject(payload, "protocol", PET_P4_NET_WIRE_PROTOCOL);
  }
  if (!send_json(fd, "net/challenge", payload)) pending_close();
}

static void pending_fail(const char *code) {
  cJSON *payload = cJSON_CreateObject();
  if (payload) {
    cJSON_AddStringToObject(payload, "error", code);
    send_json(s.pending.fd, "net/auth_error", payload);
  }
  pet_p4_net_auth_failure(&s.guard, now_ms());
  pending_close();
}

static void pending_authenticate(const char *line) {
  cJSON *root = cJSON_Parse(line);
  const cJSON *topic = cJSON_GetObjectItemCaseSensitive(root, "topic");
  const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
  const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(payload, "nonce");
  const cJSON *proof = cJSON_GetObjectItemCaseSensitive(payload, "proof");
  char message[192];
  char expected[PET_P4_NET_PROOF_HEX_LEN + 1];
  char reply[PET_P4_NET_PROOF_HEX_LEN + 1];
  bool ok = cJSON_IsString(topic) && strcmp(topic->valuestring, "net/auth") == 0
    && cJSON_IsString(nonce) && pet_p4_net_nonce_valid(nonce->valuestring)
    && cJSON_IsString(proof)
    && pet_p4_net_proof_message("client", s.config.board_device_id, s.pending.device_nonce, nonce->valuestring, message, sizeof(message))
    && hmac_hex(s.secret, message, expected)
    && pet_p4_net_ct_equal(expected, proof->valuestring)
    && pet_p4_net_proof_message("server", s.config.board_device_id, s.pending.device_nonce, nonce->valuestring, message, sizeof(message))
    && hmac_hex(s.secret, message, reply);
  memset(message, 0, sizeof(message));
  cJSON_Delete(root);
  if (!ok) {
    pending_fail("auth_failed");
    return;
  }
  cJSON *response = cJSON_CreateObject();
  if (response) cJSON_AddStringToObject(response, "proof", reply);
  if (!send_json(s.pending.fd, "net/auth_ok", response)) {
    pending_close();
    return;
  }
  pet_p4_net_auth_success(&s.guard);
  xSemaphoreTake(s.client_lock, portMAX_DELAY);
  client_close_locked(); /* the desktop reconnected: its stale socket yields to the proven one */
  s.client_fd = s.pending.fd;
  s.pending.fd = -1;
  s.client_authed = true;
  s.client_drop = false;
  s.client_last_rx_ms = s.client_last_tx_ms = now_ms();
  xSemaphoreGive(s.client_lock);
  pending_close();
  if (s.config.on_client) s.config.on_client(true, s.config.ctx);
}

static void pending_poll(void) {
  if (s.pending.fd < 0) return;
  uint64_t now = now_ms();
  if (now - s.pending.accepted_ms > PET_P4_NET_AUTH_TIMEOUT_MS) {
    pending_fail("auth_timeout");
    return;
  }
  char byte;
  while (s.pending.fd >= 0) {
    int n = recv(s.pending.fd, &byte, 1, 0);
    if (n == 0) {
      pending_close();
      return;
    }
    if (n < 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN) pending_close();
      return;
    }
    if (byte == '\r') continue;
    if (byte != '\n') {
      if (s.pending.used + 1 >= sizeof(s.pending.line)) {
        pending_fail("line_too_long");
        return;
      }
      s.pending.line[s.pending.used++] = byte;
      continue;
    }
    s.pending.line[s.pending.used] = '\0';
    char line[PET_P4_NET_HANDSHAKE_LINE_MAX];
    memcpy(line, s.pending.line, s.pending.used + 1);
    s.pending.used = 0;
    pending_authenticate(line);
    return;
  }
}

static void client_poll(void) {
  uint64_t now = now_ms();
  xSemaphoreTake(s.client_lock, portMAX_DELAY);
  int fd = s.client_fd;
  bool authed = s.client_authed;
  xSemaphoreGive(s.client_lock);
  if (fd < 0 || !authed) return;
  uint8_t buffer[PET_P4_NET_RX_CHUNK];
  bool dead = false;
  for (int round = 0; round < 4 && !dead; ++round) {
    int n = recv(fd, buffer, sizeof(buffer), 0);
    if (n > 0) {
      s.client_last_rx_ms = now;
      if (s.config.on_bytes) s.config.on_bytes(buffer, (size_t) n, s.config.ctx);
      continue;
    }
    if (n == 0 || (errno != EWOULDBLOCK && errno != EAGAIN)) dead = true;
    break;
  }
  if (!dead && now - s.client_last_rx_ms > PET_P4_NET_CLIENT_IDLE_MS) dead = true;
  if (!dead && now - s.client_last_tx_ms >= PET_P4_NET_KEEPALIVE_MS) {
    xSemaphoreTake(s.client_lock, portMAX_DELAY);
    if (s.client_fd == fd) {
      cJSON *payload = cJSON_CreateObject();
      if (payload) cJSON_AddNumberToObject(payload, "uptimeMs", (double) now);
      if (send_json(fd, "net/keepalive", payload)) s.client_last_tx_ms = now;
      else dead = true;
    }
    xSemaphoreGive(s.client_lock);
  }
  xSemaphoreTake(s.client_lock, portMAX_DELAY);
  if (dead || s.client_drop) {
    if (s.client_fd == fd) client_close_locked();
  }
  xSemaphoreGive(s.client_lock);
}

static void discovery_poll(void) {
  if (s.udp_fd < 0) return;
  char datagram[256];
  struct sockaddr_in from;
  socklen_t from_len = sizeof(from);
  int n = recvfrom(s.udp_fd, datagram, sizeof(datagram) - 1, 0, (struct sockaddr *) &from, &from_len);
  if (n <= 0) return;
  datagram[n] = '\0';
  cJSON *root = cJSON_Parse(datagram);
  const cJSON *topic = cJSON_GetObjectItemCaseSensitive(root, "topic");
  const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
  const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(payload, "nonce");
  if (cJSON_IsString(topic) && strcmp(topic->valuestring, "net/discover") == 0
      && cJSON_IsString(nonce) && strlen(nonce->valuestring) <= 32) {
    cJSON *reply = cJSON_CreateObject();
    cJSON *envelope = cJSON_CreateObject();
    if (reply && envelope) {
      cJSON_AddStringToObject(reply, "boardDeviceId", s.config.board_device_id);
      cJSON_AddStringToObject(reply, "nonce", nonce->valuestring);
      cJSON_AddNumberToObject(reply, "port", PET_P4_NET_TCP_PORT);
      cJSON_AddStringToObject(reply, "fw", s.config.firmware_version);
      cJSON_AddBoolToObject(reply, "paired", s.configured);
      cJSON_AddStringToObject(envelope, "topic", "net/announce");
      cJSON_AddItemToObject(envelope, "payload", reply);
      reply = NULL;
      char *text = cJSON_PrintUnformatted(envelope);
      if (text) sendto(s.udp_fd, text, strlen(text), 0, (struct sockaddr *) &from, from_len);
      cJSON_free(text);
    }
    cJSON_Delete(reply);
    cJSON_Delete(envelope);
  }
  cJSON_Delete(root);
}

static void serve_sockets(void) {
  lock();
  bool wanted = s.sockets_wanted && s.state == NET_CONNECTED;
  unlock();
  if (!wanted) {
    if (s.listen_fd >= 0 || s.client_fd >= 0 || s.pending.fd >= 0) sockets_close();
    vTaskDelay(pdMS_TO_TICKS(PET_P4_NET_TICK_MS));
    return;
  }
  if (s.listen_fd < 0 && !sockets_open()) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }
  fd_set readable;
  FD_ZERO(&readable);
  int max_fd = s.listen_fd;
  FD_SET(s.listen_fd, &readable);
  if (s.udp_fd >= 0) {
    FD_SET(s.udp_fd, &readable);
    if (s.udp_fd > max_fd) max_fd = s.udp_fd;
  }
  if (s.pending.fd >= 0) {
    FD_SET(s.pending.fd, &readable);
    if (s.pending.fd > max_fd) max_fd = s.pending.fd;
  }
  if (s.client_fd >= 0) {
    FD_SET(s.client_fd, &readable);
    if (s.client_fd > max_fd) max_fd = s.client_fd;
  }
  struct timeval wait = {.tv_sec = 0, .tv_usec = PET_P4_NET_TICK_MS * 1000};
  (void) select(max_fd + 1, &readable, NULL, NULL, &wait);
  if (FD_ISSET(s.listen_fd, &readable)) pending_accept();
  discovery_poll();
  pending_poll();
  client_poll();
}

static void network_task(void *arg) {
  (void) arg;
  for (;;) {
    supervisor_step();
    serve_sockets();
  }
}

/* ---- public API -------------------------------------------------------------------------- */

esp_err_t pet_p4_network_init(const pet_p4_network_config_t *config) {
  if (!config || !config->board_device_id || s.lock) return ESP_ERR_INVALID_ARG;
  memset(&s, 0, sizeof(s));
  s.config = *config;
  s.listen_fd = s.udp_fd = s.client_fd = s.pending.fd = -1;
  s.lock = xSemaphoreCreateMutex();
  s.client_lock = xSemaphoreCreateMutex();
  if (!s.lock || !s.client_lock) return ESP_ERR_NO_MEM;
  credentials_load();
  s.state = s.configured ? NET_IDLE : NET_UNCONFIGURED;
  s.not_before_ms = now_ms() + PET_P4_NET_AUTOSTART_DELAY_MS;
  if (s.configured && boot_try_read() >= PET_P4_NET_CRASH_LOOP_LIMIT) {
    s.crash_loop = true;
    s.state = NET_FAILED;
    set_error("crash_loop");
    ESP_LOGW(TAG, "wireless stack disabled after repeated crashes; send network/config over USB to retry");
  }
  return xTaskCreate(network_task, "pet_p4_net", PET_P4_NET_TASK_STACK_BYTES, NULL,
                     PET_P4_NET_TASK_PRIORITY, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool pet_p4_network_client_ready(void) {
  return s.lock && s.client_authed;
}

bool pet_p4_network_send_line(const char *line) {
  if (!s.lock || !line || !s.client_authed) return false;
  xSemaphoreTake(s.client_lock, portMAX_DELAY);
  bool ok = false;
  if (s.client_fd >= 0 && s.client_authed) {
    ok = send_all(s.client_fd, line, strlen(line), PET_P4_NET_SEND_BUDGET_MS)
      && send_all(s.client_fd, "\n", 1, PET_P4_NET_SEND_BUDGET_MS);
    if (ok) s.client_last_tx_ms = now_ms();
    else s.client_drop = true; /* the network task closes it; never close under a sender */
  }
  xSemaphoreGive(s.client_lock);
  return ok;
}

bool pet_p4_network_handle_topic(
  const char *topic, const cJSON *payload, pet_p4_send_line_fn send_line, void *ctx, bool via_wifi
) {
  if (!topic || strncmp(topic, "network/", 8) != 0) return false;
  if (strcmp(topic, "network/query") == 0) {
    reply_status(send_line, ctx, payload, true, NULL);
    return true;
  }
  bool is_config = strcmp(topic, "network/config") == 0;
  bool is_clear = strcmp(topic, "network/clear") == 0;
  if (!is_config && !is_clear) return false;
  if (via_wifi) {
    reply_status(send_line, ctx, payload, false, "usb_required");
    return true;
  }
  if (is_clear) {
    credentials_erase();
    lock();
    s.close_sockets_request = true;
    s.configured = false;
    s.reconfigure = false;
    s.disconnect_request = true;
    s.crash_loop = false;
    s.attempts = 0;
    s.state = NET_UNCONFIGURED;
    s.ip[0] = '\0';
    set_error("");
    s.disconnect_reason = 0;
    memset(s.ssid, 0, sizeof(s.ssid));
    memset(s.password, 0, sizeof(s.password));
    memset(s.secret, 0, sizeof(s.secret));
    unlock();
    reply_status(send_line, ctx, payload, true, NULL);
    return true;
  }
  const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(payload, "ssid");
  const cJSON *password = cJSON_GetObjectItemCaseSensitive(payload, "password");
  const cJSON *secret = cJSON_GetObjectItemCaseSensitive(payload, "secret");
  if (!cJSON_IsString(ssid) || !cJSON_IsString(password)
      || !pet_p4_net_credentials_valid(ssid->valuestring, password->valuestring)) {
    reply_status(send_line, ctx, payload, false, "invalid_credentials");
    return true;
  }
  if (!cJSON_IsString(secret) || !pet_p4_net_secret_valid(secret->valuestring)) {
    reply_status(send_line, ctx, payload, false, "invalid_secret");
    return true;
  }
  if (credentials_store(ssid->valuestring, password->valuestring, secret->valuestring) != ESP_OK) {
    reply_status(send_line, ctx, payload, false, "storage_failed");
    return true;
  }
  lock();
  s.close_sockets_request = true; /* a new secret invalidates any session proven with the old one */
  snprintf(s.ssid, sizeof(s.ssid), "%s", ssid->valuestring);
  snprintf(s.password, sizeof(s.password), "%s", password->valuestring);
  snprintf(s.secret, sizeof(s.secret), "%s", secret->valuestring);
  s.configured = true;
  s.crash_loop = false;
  s.reconfigure = s.stack_started;
  s.disconnect_request = false;
  s.attempts = 0;
  s.next_attempt_ms = 0;
  s.not_before_ms = 0; /* the user is waiting: skip the boot-time autostart delay */
  s.state = NET_STARTING;
  s.disconnect_reason = 0;
  set_error("");
  unlock();
  reply_status(send_line, ctx, payload, true, NULL);
  return true;
}
