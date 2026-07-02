/*
 * board_server_state.h — shared board-server core types.
 *
 * br_server_state is the aggregate runtime state passed by pointer to every
 * board-server handler. It used to live inside board_server.c, which made the
 * OTA / HTTP / session subsystems impossible to split into their own
 * translation units. Hoisting it (plus the small value types it embeds and the
 * capacity constants) into this header lets those subsystems compile
 * separately while board_server.c keeps ownership of the lifecycle.
 */
#ifndef BOARD_SERVER_STATE_H
#define BOARD_SERVER_STATE_H

#include <stdarg.h>
#include <stdbool.h>

#include "runtime_common.h"
#include "runtime_mqtt.h"
#include "runtime_pairing.h"
#include "runtime_session_state.h"
#include "runtime_usb_serial.h"

#define BR_HTTP_BUFFER 16384
#define BR_MAX_WS_CLIENTS 16
#define BR_MAX_SOURCE_FRAMES 16
#define BR_MAX_SPEECH_RECORDS 4
#define BR_DISCOVERY_PACKET_MAX 2048
#define BR_SPEECH_HOLD_MS 30000LL

typedef struct {
  int fd;
  bool active;
} br_ws_client;

typedef struct {
  char source[128];
  char json[BR_MAX_JSON];
  bool active;
} br_source_frame;

typedef struct {
  bool active;
  char key[256];
  char source[128];
  char session_id[128];
  char title[256];
  char body[768];
  char text[1024];
  long long updated_at_ms;
  long long expires_at_ms;
} br_speech_record;

typedef struct {
  char root_dir[BR_MAX_PATH];
  char device_config_path[BR_MAX_PATH];
  char network_config_path[BR_MAX_PATH];
  char current_state_path[BR_MAX_PATH];
  char current_event_path[BR_MAX_PATH];
  char current_speech_path[BR_MAX_PATH];
  char current_speech_hold_until_path[BR_MAX_PATH];
  char current_debug_speech_path[BR_MAX_PATH];
  char screen_interrupt_path[BR_MAX_PATH];
  char screen_page_path[BR_MAX_PATH];
  char audio_bridge_config_path[BR_MAX_PATH];
  char voice_button_config_path[BR_MAX_PATH];
  char button_config_path[BR_MAX_PATH];
  char sound_script_path[BR_MAX_PATH];
  char http_host[64];
  int http_port;
  char mqtt_url[256];
  char mqtt_username[128];
  char mqtt_password[128];
  char admin_token[256];
  char mqtt_namespace[64];
  char local_device_id[128];
  char board_device_id[128];
  char screen_name[128];
  char screen_model[128];
  char screen_fw[64];
  char target_device_id[128];
  char target_source[128];
  char public_host[128];
  char public_url[256];
  char ap_ip[64];
  char ap_ssid[64];
  char ap_psk[64];
  int discovery_udp_port;
  int discovery_mdns_port;
  int discovery_timeout_ms;
  int discovery_announce_interval_ms;
  char ap_up_cmd[256];
  char ap_down_cmd[256];
  char sta_apply_cmd[256];
} br_server_config;

typedef enum {
  BR_TRANSPORT_MQTT = 0,
  BR_TRANSPORT_USB = 1
} br_transport_mode;

typedef struct {
  br_server_config config;
  br_transport_mode transport_mode;
  br_mqtt_client mqtt;
  bool mqtt_online;
  br_usb_serial usb_serial;
  int listen_fd;
  int discovery_fd;
  int mdns_fd;
  bool shutdown_requested;
  bool ap_mode_active;
  unsigned int ws_seq;
  long long last_discovery_announce_ms;
  br_ws_client ws_clients[BR_MAX_WS_CLIENTS];
  br_pairing_machine pairing;
  char current_state_topic[BR_MAX_TOPIC];
  char wildcard_state_topic[BR_MAX_TOPIC];
  char current_speech_topic[BR_MAX_TOPIC];
  char control_topic[BR_MAX_TOPIC];
  char command_topic[BR_MAX_TOPIC];
  char input_action_topic[BR_MAX_TOPIC];
  char hello_topic[BR_MAX_TOPIC];
  char availability_topic[BR_MAX_TOPIC];
  char active_frame[BR_MAX_JSON];
  bool has_active_frame;
  char pairing_message[256];
  char last_discovery_peer[64];
  br_source_frame source_frames[BR_MAX_SOURCE_FRAMES];
  br_speech_record speech_records[BR_MAX_SPEECH_RECORDS];
  char last_state[64];
  char last_reason[128];
  long long last_speech_rewrite_ms;
  long long last_state_update_ms;
  long long last_stats_flush_ms;
  br_session_machine session_machine;
  long long next_session_tick_ms;
  long long next_speech_tick_ms;
  bool use_legacy_active_topic;
  char screen_page_topic[BR_MAX_TOPIC];
  char usb_touch_action_path[BR_MAX_PATH];
  long long last_usb_touch_check_ms;
} br_server_state;

/* Defined in board_server.c; exposed so subsystem TUs (e.g. board_asset_ota.c)
 * can log through the same "[board-server]" prefix. */
void br_server_logf(const char *format, ...);

#endif /* BOARD_SERVER_STATE_H */
