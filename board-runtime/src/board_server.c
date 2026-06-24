/*
 * [Input] Board runtime root, MQTT/USB transport env, pairing/network config, and local device input/control events.
 * [Output] HTTP/WebSocket/MQTT/USB board service that updates session state files, publishes availability, and binds to the selected desktop agent source.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

#include "runtime_common.h"
#include "runtime_debug.h"
#include "runtime_mqtt.h"
#include "runtime_pairing.h"
#include "runtime_protocol.h"
#include "runtime_session_state.h"
#include "runtime_stats.h"
#include "runtime_usb_serial.h"
#include "runtime_wifi.h"
#include "screen_page.h"

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

typedef struct {
  int total;
  int passed;
  int failed;
  int warnings;
  bool json;
  bool first;
} br_self_check_report;

static br_server_state *g_server = NULL;
static void br_server_subscribe_topics(br_server_state *server);

static void br_server_request_shutdown(int signal_number) {
  (void) signal_number;
  if (g_server) {
    g_server->shutdown_requested = true;
  }
}

typedef struct {
  uint32_t h[5];
  uint64_t total_bits;
  unsigned char buffer[64];
  size_t buffer_used;
} br_sha1_ctx;

static uint32_t br_sha1_rotl(uint32_t value, uint32_t bits) {
  return (value << bits) | (value >> (32U - bits));
}

static void br_sha1_block(br_sha1_ctx *ctx, const unsigned char *block) {
  uint32_t w[80];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;

  for (int i = 0; i < 16; i += 1) {
    w[i] = ((uint32_t) block[i * 4] << 24U) |
           ((uint32_t) block[i * 4 + 1] << 16U) |
           ((uint32_t) block[i * 4 + 2] << 8U) |
           (uint32_t) block[i * 4 + 3];
  }
  for (int i = 16; i < 80; i += 1) {
    w[i] = br_sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1U);
  }

  a = ctx->h[0];
  b = ctx->h[1];
  c = ctx->h[2];
  d = ctx->h[3];
  e = ctx->h[4];

  for (int i = 0; i < 80; i += 1) {
    uint32_t f;
    uint32_t k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5a827999U;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ed9eba1U;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8f1bbcdcU;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6U;
    }
    uint32_t temp = br_sha1_rotl(a, 5U) + f + e + k + w[i];
    e = d;
    d = c;
    c = br_sha1_rotl(b, 30U);
    b = a;
    a = temp;
  }

  ctx->h[0] += a;
  ctx->h[1] += b;
  ctx->h[2] += c;
  ctx->h[3] += d;
  ctx->h[4] += e;
}

static void br_sha1_init(br_sha1_ctx *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->h[0] = 0x67452301U;
  ctx->h[1] = 0xefcdab89U;
  ctx->h[2] = 0x98badcfeU;
  ctx->h[3] = 0x10325476U;
  ctx->h[4] = 0xc3d2e1f0U;
}

static void br_sha1_update(br_sha1_ctx *ctx, const unsigned char *data, size_t length) {
  while (length > 0) {
    size_t copy_size = 64U - ctx->buffer_used;
    if (copy_size > length) {
      copy_size = length;
    }
    memcpy(ctx->buffer + ctx->buffer_used, data, copy_size);
    ctx->buffer_used += copy_size;
    data += copy_size;
    length -= copy_size;
    ctx->total_bits += (uint64_t) copy_size * 8ULL;
    if (ctx->buffer_used == 64U) {
      br_sha1_block(ctx, ctx->buffer);
      ctx->buffer_used = 0;
    }
  }
}

static void br_sha1_final(br_sha1_ctx *ctx, unsigned char output[20]) {
  ctx->buffer[ctx->buffer_used++] = 0x80U;
  if (ctx->buffer_used > 56U) {
    while (ctx->buffer_used < 64U) {
      ctx->buffer[ctx->buffer_used++] = 0x00U;
    }
    br_sha1_block(ctx, ctx->buffer);
    ctx->buffer_used = 0;
  }
  while (ctx->buffer_used < 56U) {
    ctx->buffer[ctx->buffer_used++] = 0x00U;
  }
  for (int i = 7; i >= 0; i -= 1) {
    ctx->buffer[ctx->buffer_used++] = (unsigned char) ((ctx->total_bits >> (unsigned int) (i * 8)) & 0xffU);
  }
  br_sha1_block(ctx, ctx->buffer);
  for (int i = 0; i < 5; i += 1) {
    output[i * 4] = (unsigned char) ((ctx->h[i] >> 24U) & 0xffU);
    output[i * 4 + 1] = (unsigned char) ((ctx->h[i] >> 16U) & 0xffU);
    output[i * 4 + 2] = (unsigned char) ((ctx->h[i] >> 8U) & 0xffU);
    output[i * 4 + 3] = (unsigned char) (ctx->h[i] & 0xffU);
  }
}

static void br_base64_encode(const unsigned char *input, size_t length, char *output, size_t output_size) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t used = 0;
  for (size_t i = 0; i < length && used + 4 < output_size; i += 3) {
    uint32_t value = (uint32_t) input[i] << 16U;
    int pad = 0;
    if (i + 1 < length) {
      value |= (uint32_t) input[i + 1] << 8U;
    } else {
      pad += 1;
    }
    if (i + 2 < length) {
      value |= input[i + 2];
    } else {
      pad += 1;
    }
    output[used++] = table[(value >> 18U) & 0x3fU];
    output[used++] = table[(value >> 12U) & 0x3fU];
    output[used++] = pad >= 2 ? '=' : table[(value >> 6U) & 0x3fU];
    output[used++] = pad >= 1 ? '=' : table[value & 0x3fU];
  }
  output[used] = '\0';
}

static int br_set_fd_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void br_build_topics(br_server_state *server) {
  snprintf(server->current_state_topic, sizeof(server->current_state_topic),
           "%s/%s/state/%s",
           server->config.mqtt_namespace,
           server->config.target_device_id,
           server->config.target_source[0] ? server->config.target_source : "active");
  snprintf(server->wildcard_state_topic, sizeof(server->wildcard_state_topic),
           "%s/%s/state/+",
           server->config.mqtt_namespace,
           server->config.target_device_id);
  snprintf(server->current_speech_topic, sizeof(server->current_speech_topic),
           "%s/%s/speech/text",
           server->config.mqtt_namespace,
           server->config.target_device_id);
  snprintf(server->control_topic, sizeof(server->control_topic),
           "%s/%s/control/remote-cli-binding",
           server->config.mqtt_namespace,
           server->config.local_device_id);
  snprintf(server->command_topic, sizeof(server->command_topic),
           "claw-pet/board/%s/control/command",
           server->config.board_device_id);
  snprintf(server->input_action_topic, sizeof(server->input_action_topic),
           "claw-pet/board/%s/input/action",
           server->config.board_device_id);
  snprintf(server->hello_topic, sizeof(server->hello_topic),
           "claw-pet/board/%s/hello",
           server->config.board_device_id);
  snprintf(server->availability_topic, sizeof(server->availability_topic),
           "claw-pet/board/%s/availability",
           server->config.board_device_id);
  snprintf(server->screen_page_topic, sizeof(server->screen_page_topic),
           "%s/%s/control/screen-page",
           server->config.mqtt_namespace,
           server->config.board_device_id);
}

static void br_server_log(const char *message) {
  fprintf(stdout, "[board-server] %s\n", message);
  fflush(stdout);
}

static void br_server_logf(const char *format, ...) {
  va_list args;
  fprintf(stdout, "[board-server] ");
  va_start(args, format);
  vfprintf(stdout, format, args);
  va_end(args);
  fputc('\n', stdout);
  fflush(stdout);
}

static void br_ws_close(br_ws_client *client) {
  if (client->active && client->fd >= 0) {
    close(client->fd);
  }
  client->fd = -1;
  client->active = false;
}

static void br_ws_send_json(br_ws_client *client, const char *json_text) {
  unsigned char header[10];
  size_t payload_length;
  size_t header_length = 0;
  if (!client || !client->active || !json_text) {
    return;
  }
  payload_length = strlen(json_text);
  header[header_length++] = 0x81U;
  if (payload_length <= 125U) {
    header[header_length++] = (unsigned char) payload_length;
  } else if (payload_length <= 65535U) {
    header[header_length++] = 126U;
    header[header_length++] = (unsigned char) ((payload_length >> 8U) & 0xffU);
    header[header_length++] = (unsigned char) (payload_length & 0xffU);
  } else {
    return;
  }
  if (send(client->fd, header, header_length, 0) < 0) {
    br_ws_close(client);
    return;
  }
  if (send(client->fd, json_text, payload_length, 0) < 0) {
    br_ws_close(client);
  }
}

static void br_broadcast_json(br_server_state *server, const char *json_text) {
  for (size_t i = 0; i < BR_MAX_WS_CLIENTS; i += 1) {
    if (server->ws_clients[i].active) {
      br_ws_send_json(&server->ws_clients[i], json_text);
    }
  }
}

static bool br_append_json_with_replayed(
  const char *json_text,
  char *output,
  size_t output_size,
  size_t *used
) {
  size_t length;
  if (!json_text || json_text[0] != '{') {
    return br_snprintf_append(output, output_size, used, "null") == 0;
  }
  length = strlen(json_text);
  if (length < 1 || json_text[length - 1] != '}') {
    return false;
  }
  if (*used + length + 20 >= output_size) {
    return false;
  }
  memcpy(output + *used, json_text, length - 1);
  *used += length - 1;
  output[*used] = '\0';
  return br_snprintf_append(output, output_size, used, ",\"replayed\":true}") == 0;
}

static bool br_build_snapshot_json(br_server_state *server, char *output, size_t output_size) {
  size_t used = 0;
  bool has_source = false;
  bool wrote_source = false;
  if (!server->has_active_frame) {
    for (size_t i = 0; i < BR_MAX_SOURCE_FRAMES; i += 1) {
      if (server->source_frames[i].active) {
        has_source = true;
        break;
      }
    }
    if (!has_source) {
      return false;
    }
  }
  output[0] = '\0';
  if (br_snprintf_append(output, output_size, &used, "{\"type\":\"bridge_snapshot\",\"replayed\":true,\"activeFrame\":") != 0) {
    return false;
  }
  if (server->has_active_frame) {
    if (!br_append_json_with_replayed(server->active_frame, output, output_size, &used)) {
      return false;
    }
  } else if (br_snprintf_append(output, output_size, &used, "null") != 0) {
    return false;
  }
  if (br_snprintf_append(output, output_size, &used, ",\"sourceFrames\":[") != 0) {
    return false;
  }
  for (size_t i = 0; i < BR_MAX_SOURCE_FRAMES; i += 1) {
    if (!server->source_frames[i].active) {
      continue;
    }
    if (wrote_source) {
      if (br_snprintf_append(output, output_size, &used, ",") != 0) {
        return false;
      }
    }
    if (!br_append_json_with_replayed(server->source_frames[i].json, output, output_size, &used)) {
      return false;
    }
    wrote_source = true;
  }
  return br_snprintf_append(output, output_size, &used, "]}") == 0;
}

static void br_store_source_frame(br_server_state *server, const char *source, const char *json_text) {
  size_t empty_index = BR_MAX_SOURCE_FRAMES;
  if (!source || !*source || strcmp(source, "none") == 0) {
    return;
  }
  for (size_t i = 0; i < BR_MAX_SOURCE_FRAMES; i += 1) {
    if (!server->source_frames[i].active) {
      if (empty_index == BR_MAX_SOURCE_FRAMES) {
        empty_index = i;
      }
      continue;
    }
    if (strcmp(server->source_frames[i].source, source) == 0) {
      br_normalize_text(json_text, "{}", server->source_frames[i].json, sizeof(server->source_frames[i].json));
      return;
    }
  }
  if (empty_index < BR_MAX_SOURCE_FRAMES) {
    server->source_frames[empty_index].active = true;
    br_normalize_text(source, "", server->source_frames[empty_index].source, sizeof(server->source_frames[empty_index].source));
    br_normalize_text(json_text, "{}", server->source_frames[empty_index].json, sizeof(server->source_frames[empty_index].json));
  }
}

static bool br_topic_ends_with(const char *topic, const char *suffix) {
  size_t topic_length = strlen(topic);
  size_t suffix_length = strlen(suffix);
  return topic_length >= suffix_length && strcmp(topic + topic_length - suffix_length, suffix) == 0;
}

static bool br_is_legacy_active_topic(br_server_state *server, const char *topic) {
  char active_topic[BR_MAX_TOPIC];
  if (!server || !topic) {
    return false;
  }
  snprintf(active_topic, sizeof(active_topic),
           "%s/%s/state/active",
           server->config.mqtt_namespace,
           server->config.target_device_id);
  return strcmp(topic, active_topic) == 0;
}

static void br_update_snapshot(br_server_state *server, const char *topic, const char *source, const char *json_text) {
  if (br_topic_ends_with(topic, "/state/active")) {
    br_normalize_text(json_text, "{}", server->active_frame, sizeof(server->active_frame));
    server->has_active_frame = true;
    return;
  }
  br_store_source_frame(server, source, json_text);
}

static void br_write_session_debug_snapshot(
  br_server_state *server,
  const char *state,
  const char *event,
  const char *reason,
  long long now_ms
) {
  char path[BR_MAX_PATH];
  char json[BR_MAX_JSON];
  br_session_resolution resolution;
  if (!server) {
    return;
  }
  br_debug_session_snapshot_path(server->config.root_dir, path, sizeof(path));
  memset(&resolution, 0, sizeof(resolution));
  br_normalize_text(state && state[0] ? state : server->session_machine.current_state,
                    "idle",
                    resolution.state,
                    sizeof(resolution.state));
  br_normalize_text(event ? event : server->session_machine.current_event,
                    "",
                    resolution.event,
                    sizeof(resolution.event));
  br_normalize_text(reason ? reason : server->session_machine.current_reason,
                    "",
                    resolution.reason,
                    sizeof(resolution.reason));
  br_normalize_text(server->session_machine.current_key,
                    "",
                    resolution.active_key,
                    sizeof(resolution.active_key));
  resolution.updated_at_ms = now_ms;
  br_session_machine_debug_json(&server->session_machine, &resolution, json, sizeof(json));
  br_atomic_write_text(path, json);
}

static void br_write_state_files_with_reason(
  br_server_state *server,
  const char *state,
  const char *event,
  const char *reason
) {
  if (state && *state) {
    br_atomic_write_text(server->config.current_state_path, state);
    if (strcmp(state, "idle") == 0) {
      br_atomic_write_text(server->config.current_speech_hold_until_path, "");
    }
  }
  br_atomic_write_text(server->config.current_event_path, event ? event : "");
  br_write_session_debug_snapshot(server,
                                  state && state[0] ? state : server->last_state,
                                  event,
                                  reason ? reason : server->last_reason,
                                  br_now_ms());
}

static void br_write_state_files(br_server_state *server, const char *state, const char *event) {
  br_write_state_files_with_reason(server, state, event, server->last_reason);
}

static void br_write_screen_interrupt(br_server_state *server, const char *reason) {
  char marker[128];
  snprintf(marker, sizeof(marker), "%lld %s\n", br_now_ms(), reason ? reason : "interrupt");
  br_atomic_write_text(server->config.screen_interrupt_path, marker);
}

static void br_write_speech_hold_until_abs(br_server_state *server, long long until_ms) {
  char hold_until[64];
  if (!server) return;
  if (until_ms <= 0) {
    br_atomic_write_text(server->config.current_speech_hold_until_path, "");
    return;
  }
  snprintf(hold_until, sizeof(hold_until), "%lld\n", until_ms);
  br_atomic_write_text(server->config.current_speech_hold_until_path, hold_until);
}

static void br_server_speech_build_key(
  const br_speech_update *update,
  char *output,
  size_t output_size
) {
  const char *source = update && update->source[0] ? update->source : "unknown";
  const char *kind = "source";
  const char *id = source;
  if (update && update->session_id[0]) {
    kind = "session";
    id = update->session_id;
  } else if (update && update->run_id[0]) {
    kind = "run";
    id = update->run_id;
  } else if (update && update->session_key[0]) {
    kind = "key";
    id = update->session_key;
  }
  snprintf(output, output_size, "%s:%s:%s", source, kind, id);
}

static br_speech_record *br_server_find_speech_slot(br_server_state *server, const char *key) {
  int empty_index = -1;
  int oldest_index = 0;
  if (!server || !key) return NULL;
  for (int i = 0; i < BR_MAX_SPEECH_RECORDS; i += 1) {
    br_speech_record *record = &server->speech_records[i];
    if (record->active && strcmp(record->key, key) == 0) {
      return record;
    }
    if (!record->active && empty_index < 0) {
      empty_index = i;
    }
    if (record->updated_at_ms < server->speech_records[oldest_index].updated_at_ms) {
      oldest_index = i;
    }
  }
  return empty_index >= 0 ? &server->speech_records[empty_index] : &server->speech_records[oldest_index];
}

static void br_server_speech_normalize_body_for_match(
  const char *input,
  char *output,
  size_t output_size
) {
  const char *body = input ? input : "";
  const char *colon = NULL;
  size_t used = 0;
  if (!output || output_size == 0) return;
  output[0] = '\0';
  if (body[0] == '\0') return;
  {
    const char *newline = strchr(body, '\n');
    if (newline && newline[1]) {
      body = newline + 1;
    }
  }
  colon = strstr(body, ": ");
  if (colon && colon > body && (colon - body) <= 80) {
    body = colon + 2;
  }
  while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') {
    body += 1;
  }
  for (const char *cursor = body; *cursor && used + 1 < output_size; cursor += 1) {
    if (*cursor == '\r' || *cursor == '\n' || *cursor == '\t' || *cursor == ' ') {
      if (used > 0 && output[used - 1] != ' ') {
        output[used++] = ' ';
      }
      continue;
    }
    output[used++] = *cursor;
  }
  while (used > 0 && output[used - 1] == ' ') {
    used -= 1;
  }
  output[used] = '\0';
}

static br_speech_record *br_server_find_duplicate_speech_slot(
  br_server_state *server,
  const br_speech_update *update,
  const char *key
) {
  const char *source;
  const char *body;
  char match_body[1024];
  if (!server || !update) return NULL;
  source = update->source[0] ? update->source : "unknown";
  body = update->body[0] ? update->body : update->text;
  if (!body[0]) return NULL;
  br_server_speech_normalize_body_for_match(body, match_body, sizeof(match_body));
  for (int i = 0; i < BR_MAX_SPEECH_RECORDS; i += 1) {
    br_speech_record *record = &server->speech_records[i];
    const char *record_body;
    char record_match_body[1024];
    if (!record->active) continue;
    if (key && record->key[0] && strcmp(record->key, key) == 0) {
      return record;
    }
    if (record->source[0] && strcmp(record->source, source) != 0) {
      continue;
    }
    record_body = record->body[0] ? record->body : record->text;
    if (record_body[0] && strcmp(record_body, body) == 0) {
      return record;
    }
    br_server_speech_normalize_body_for_match(record_body, record_match_body, sizeof(record_match_body));
    if (match_body[0] && record_match_body[0] && strcmp(record_match_body, match_body) == 0) {
      return record;
    }
  }
  return NULL;
}

static bool br_server_cleanup_speech_records(br_server_state *server, long long now_ms) {
  bool changed = false;
  if (!server) return false;
  for (int i = 0; i < BR_MAX_SPEECH_RECORDS; i += 1) {
    br_speech_record *record = &server->speech_records[i];
    if (!record->active) continue;
    if (record->expires_at_ms > 0 && now_ms >= record->expires_at_ms) {
      memset(record, 0, sizeof(*record));
      changed = true;
    }
  }
  return changed;
}

static long long br_server_latest_speech_expiry_ms(br_server_state *server) {
  long long latest = 0;
  if (!server) return 0;
  for (int i = 0; i < BR_MAX_SPEECH_RECORDS; i += 1) {
    br_speech_record *record = &server->speech_records[i];
    if (record->active && record->expires_at_ms > latest) {
      latest = record->expires_at_ms;
    }
  }
  return latest;
}

static void br_server_append_speech_line(
  char *output,
  size_t output_size,
  size_t *used,
  const br_speech_record *record
) {
  char line[512];
  const char *title;
  const char *body;
  if (!output || !used || !record) return;
  title = record->title[0] ? record->title : (record->source[0] ? record->source : "session");
  body = record->body[0] ? record->body : record->text;
  if (record->title[0] && body[0] && strcmp(record->title, body) != 0) {
    snprintf(line, sizeof(line), "%s: %s", title, body);
  } else {
    snprintf(line, sizeof(line), "%s", body[0] ? body : title);
  }
  for (char *cursor = line; *cursor; cursor += 1) {
    if (*cursor == '\r' || *cursor == '\n') {
      *cursor = ' ';
    }
  }
  if (line[0] == '\0') return;
  if (*used > 0) {
    br_snprintf_append(output, output_size, used, "\n");
  }
  br_snprintf_append(output, output_size, used, "%s", line);
}

static bool br_server_render_speech_records(
  br_server_state *server,
  char *output,
  size_t output_size,
  long long now_ms
) {
  bool used_indices[BR_MAX_SPEECH_RECORDS] = {0};
  size_t used = 0;
  int appended = 0;
  if (!server || !output || output_size == 0) return false;
  output[0] = '\0';
  br_server_cleanup_speech_records(server, now_ms);
  for (;;) {
    int best_index = -1;
    for (int i = 0; i < BR_MAX_SPEECH_RECORDS; i += 1) {
      br_speech_record *record = &server->speech_records[i];
      if (used_indices[i] || !record->active) continue;
      if (best_index < 0 || record->updated_at_ms > server->speech_records[best_index].updated_at_ms) {
        best_index = i;
      }
    }
    if (best_index < 0) break;
    used_indices[best_index] = true;
    br_server_append_speech_line(output, output_size, &used, &server->speech_records[best_index]);
    appended += 1;
  }
  return appended > 0 && output[0] != '\0';
}

static bool br_server_apply_speech_update(
  br_server_state *server,
  const br_speech_update *update,
  long long now_ms,
  char *rendered,
  size_t rendered_size
) {
  char key[256];
  br_speech_record *record;
  if (!server || !update || !update->text[0]) return false;
  br_server_speech_build_key(update, key, sizeof(key));
  record = br_server_find_duplicate_speech_slot(server, update, key);
  if (!record) {
    record = br_server_find_speech_slot(server, key);
  }
  if (!record) return false;
  memset(record, 0, sizeof(*record));
  record->active = true;
  snprintf(record->key, sizeof(record->key), "%s", key);
  snprintf(record->source, sizeof(record->source), "%s", update->source[0] ? update->source : "unknown");
  snprintf(record->session_id, sizeof(record->session_id), "%s", update->session_id);
  snprintf(record->title, sizeof(record->title), "%s", update->title);
  snprintf(record->body, sizeof(record->body), "%s", update->body);
  snprintf(record->text, sizeof(record->text), "%s", update->text);
  record->updated_at_ms = update->has_payload_ts_ms && update->payload_ts_ms > 0
    ? update->payload_ts_ms
    : now_ms;
  record->expires_at_ms = update->has_expires_at_ms && update->expires_at_ms > now_ms
    ? update->expires_at_ms
    : now_ms + BR_SPEECH_HOLD_MS;
  return br_server_render_speech_records(server, rendered, rendered_size, now_ms);
}

static bool br_server_extract_string_key(
  const char *json_text,
  const br_json_token *tokens,
  int token_count,
  const char *key,
  char *output,
  size_t output_size
) {
  int value_index;
  if (!json_text || !tokens || token_count <= 0 || !key || !output || output_size == 0) {
    return false;
  }
  output[0] = '\0';
  value_index = br_json_find_key(json_text, tokens, token_count, 0, key);
  if (value_index < 0 || tokens[value_index].type != BR_JSON_STRING) {
    return false;
  }
  return br_json_token_to_string(json_text, &tokens[value_index], output, output_size);
}

static bool br_server_parse_network_config_json(
  const char *json_text,
  char *ssid,
  size_t ssid_size,
  char *password,
  size_t password_size,
  char *mqtt_url,
  size_t mqtt_url_size,
  char *mqtt_namespace,
  size_t mqtt_namespace_size,
  char *desktop_device_id,
  size_t desktop_device_id_size
) {
  br_json_token tokens[128];
  int count;
  if (!json_text) {
    return false;
  }
  if (ssid && ssid_size > 0) ssid[0] = '\0';
  if (password && password_size > 0) password[0] = '\0';
  if (mqtt_url && mqtt_url_size > 0) mqtt_url[0] = '\0';
  if (mqtt_namespace && mqtt_namespace_size > 0) mqtt_namespace[0] = '\0';
  if (desktop_device_id && desktop_device_id_size > 0) desktop_device_id[0] = '\0';

  count = br_json_parse(json_text, strlen(json_text), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) {
    return false;
  }

  br_server_extract_string_key(json_text, tokens, count, "ssid", ssid, ssid_size);
  br_server_extract_string_key(json_text, tokens, count, "password", password, password_size);
  br_server_extract_string_key(json_text, tokens, count, "mqttUrl", mqtt_url, mqtt_url_size);
  if (mqtt_namespace && mqtt_namespace_size > 0) {
    if (!br_server_extract_string_key(json_text, tokens, count, "mqttNamespace", mqtt_namespace, mqtt_namespace_size)) {
      br_server_extract_string_key(json_text, tokens, count, "namespace", mqtt_namespace, mqtt_namespace_size);
    }
  }
  if (desktop_device_id && desktop_device_id_size > 0) {
    if (!br_server_extract_string_key(json_text, tokens, count, "desktopDeviceId", desktop_device_id, desktop_device_id_size)) {
      br_server_extract_string_key(json_text, tokens, count, "targetDeviceId", desktop_device_id, desktop_device_id_size);
    }
  }

  // The portal no longer surfaces the MQTT broker URL -- we treat it as a
  // firmware-level default and let the device reuse its previous value.  As
  // long as the SSID is non-empty the request is considered valid.
  return ssid && ssid[0] != '\0';
}

static bool br_server_load_network_config(br_server_state *server, char *detail, size_t detail_size) {
  char buffer[4096];
  char ssid[128];
  char password[128];
  char mqtt_url[256];
  char mqtt_namespace[64];
  char desktop_device_id[128];
  bool valid;

  if (!server) {
    return false;
  }
  if (!br_read_text_file(server->config.network_config_path, buffer, sizeof(buffer))) {
    if (detail && detail_size > 0) {
      snprintf(detail, detail_size, "missing %s", server->config.network_config_path);
    }
    return false;
  }

  valid = br_server_parse_network_config_json(
    buffer,
    ssid, sizeof(ssid),
    password, sizeof(password),
    mqtt_url, sizeof(mqtt_url),
    mqtt_namespace, sizeof(mqtt_namespace),
    desktop_device_id, sizeof(desktop_device_id)
  );
  if (!valid) {
    if (detail && detail_size > 0) {
      snprintf(detail, detail_size, "invalid network-config.json");
    }
    return false;
  }

  if (detail && detail_size > 0) {
    snprintf(detail, detail_size, "ssid=%s mqtt=%s", ssid, mqtt_url);
  }
  if ((!getenv("PET_CLAW_MQTT_URL") || getenv("PET_CLAW_MQTT_URL")[0] == '\0') &&
      (!getenv("MQTT_URL") || getenv("MQTT_URL")[0] == '\0')) {
    br_normalize_text(mqtt_url, server->config.mqtt_url, server->config.mqtt_url, sizeof(server->config.mqtt_url));
  }
  if (mqtt_namespace[0] != '\0' &&
      (!getenv("PET_CLAW_MQTT_NAMESPACE") || getenv("PET_CLAW_MQTT_NAMESPACE")[0] == '\0') &&
      (!getenv("STATUS_NAMESPACE") || getenv("STATUS_NAMESPACE")[0] == '\0')) {
    br_normalize_topic_part(mqtt_namespace, server->config.mqtt_namespace, server->config.mqtt_namespace, sizeof(server->config.mqtt_namespace));
  }
  if (desktop_device_id[0] != '\0' &&
      (!getenv("PET_CLAW_TARGET_DEVICE_ID") || getenv("PET_CLAW_TARGET_DEVICE_ID")[0] == '\0')) {
    br_normalize_topic_part(desktop_device_id,
                            server->config.target_device_id,
                            server->config.target_device_id,
                            sizeof(server->config.target_device_id));
  }
  return true;
}

static int br_server_open_udp_listener(int port, bool broadcast) {
  int fd;
  int yes = 1;
  struct sockaddr_in address;
  if (port <= 0 || port > 65535) {
    return -1;
  }

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  // FD_CLOEXEC keeps this listen socket from leaking into child processes
  // spawned via system()/exec() (e.g. board-ap-up.sh starting wpa_supplicant
  // -B which would otherwise inherit the fd and keep the port pinned after
  // board-server itself exits).
  (void) fcntl(fd, F_SETFD, FD_CLOEXEC);
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (broadcast) {
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  }
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t) port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  if (br_set_fd_nonblocking(fd) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int br_server_open_udp_sender(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  int yes = 1;
  unsigned char ttl = 1;
  if (fd < 0) {
    return -1;
  }
  (void) fcntl(fd, F_SETFD, FD_CLOEXEC);
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  return fd;
}

static void br_server_build_discovery_json(
  br_server_state *server,
  const char *transport,
  const char *requester,
  char *output,
  size_t output_size
) {
  size_t used = 0;
  const char *pairing_state = br_pairing_state_name(server->pairing.state);
  const char *pairing_mode = br_pairing_mode_name(server->pairing.state);
  const char *host = (server->pairing.state == BR_PAIRING_AP_FALLBACK && server->config.ap_ip[0] != '\0')
    ? server->config.ap_ip
    : (server->config.public_host[0] ? server->config.public_host : "127.0.0.1");
  output[0] = '\0';
  br_snprintf_append(output, output_size, &used, "{");
  br_snprintf_append(output, output_size, &used, "\"type\":\"board_discovery\",\"transport\":\"");
  br_json_escape_append(output, output_size, &used, transport ? transport : "udp");
  br_snprintf_append(output, output_size, &used, "\",\"boardDeviceId\":\"");
  br_json_escape_append(output, output_size, &used, server->config.board_device_id);
  br_snprintf_append(output, output_size, &used, "\",\"boardName\":\"");
  br_json_escape_append(output, output_size, &used, server->config.screen_name);
  br_snprintf_append(output, output_size, &used, "\",\"pairingState\":\"");
  br_json_escape_append(output, output_size, &used, pairing_state);
  br_snprintf_append(output, output_size, &used, "\",\"pairingMode\":\"");
  br_json_escape_append(output, output_size, &used, pairing_mode);
  br_snprintf_append(output, output_size, &used, "\",\"host\":\"");
  br_json_escape_append(output, output_size, &used, host);
  br_snprintf_append(output, output_size, &used, "\",\"httpPort\":%d", server->config.http_port);
  br_snprintf_append(output, output_size, &used, ",\"discoveryUdpPort\":%d", server->config.discovery_udp_port);
  br_snprintf_append(output, output_size, &used, ",\"discoveryMdnsPort\":%d", server->config.discovery_mdns_port);
  br_snprintf_append(output, output_size, &used, ",\"apIp\":\"");
  br_json_escape_append(output, output_size, &used, server->config.ap_ip);
  br_snprintf_append(output, output_size, &used, "\",\"apSsid\":\"");
  br_json_escape_append(output, output_size, &used, server->config.ap_ssid);
  br_snprintf_append(output, output_size, &used, "\",\"apPsk\":\"");
  br_json_escape_append(output, output_size, &used, server->config.ap_psk);
  br_snprintf_append(output, output_size, &used, "\",\"mqttNamespace\":\"");
  br_json_escape_append(output, output_size, &used, server->config.mqtt_namespace);
  br_snprintf_append(output, output_size, &used, "\",\"requester\":\"");
  br_json_escape_append(output, output_size, &used, requester ? requester : "");
  br_snprintf_append(output, output_size, &used, "\",\"tsMs\":%lld}", br_now_ms());
}

static void br_server_spawn_shell(const char *cmd);

/// Check whether hostapd is actually running.  Returns true when at least one
/// hostapd process exists on the system.  This is a lightweight heuristic that
/// avoids relying solely on the in-memory `ap_mode_active` flag, which can get
/// out of sync when the ap_up script fails or the process is killed externally.
static bool br_is_hostapd_running(void) {
  // `pidof hostapd` exits 0 when at least one instance is running.
  int rc = system("pidof hostapd >/dev/null 2>&1");
  return rc == 0;
}

static void br_server_set_ap_mode(br_server_state *server, bool enabled) {
  if (!server) {
    return;
  }

  if (enabled) {
    // When requesting AP-on, re-run the script if hostapd isn't actually
    // running — this handles the case where a previous ap_up_cmd failed or
    // hostapd crashed while ap_mode_active was already set to true.
    if (server->ap_mode_active && br_is_hostapd_running()) {
      return;  // genuinely up, nothing to do
    }
    if (server->config.ap_up_cmd[0]) {
      br_server_logf("AP up command: %s%s", server->config.ap_up_cmd,
                     server->ap_mode_active ? " (re-run: hostapd not running)" : "");
      br_server_spawn_shell(server->config.ap_up_cmd);
    }
    server->ap_mode_active = true;
    return;
  }

  // Requesting AP-off
  if (!server->ap_mode_active) {
    return;
  }
  if (server->config.ap_down_cmd[0]) {
    br_server_logf("AP down command: %s", server->config.ap_down_cmd);
    br_server_spawn_shell(server->config.ap_down_cmd);
  }
  server->ap_mode_active = false;
}

static void br_server_spawn_shell(const char *cmd) {
  // Run an ap_up_cmd / ap_down_cmd asynchronously so the caller (usually an
  // HTTP request handler) can return immediately instead of blocking while
  // hostapd / wpa_supplicant reconfigure the iface and tear down the current
  // TCP connection.  Double-fork so the grandchild is reparented to init and
  // we don't accumulate zombies.
  if (!cmd || !cmd[0]) {
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    return;
  }
  if (pid > 0) {
    int status = 0;
    (void) waitpid(pid, &status, 0);
    return;
  }
  pid_t inner = fork();
  if (inner < 0) {
    _exit(127);
  }
  if (inner > 0) {
    _exit(0);
  }
  // Brief delay so the outgoing HTTP response has time to flush out of the
  // kernel before we tear down wlan0 in the AP scripts.  500ms is well below
  // a human-noticeable latency but long enough for a LAN-round-trip ACK.
  struct timespec delay = { 0, 500 * 1000 * 1000 };
  nanosleep(&delay, NULL);
  (void) execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
  _exit(127);
}

static void br_server_shell_quote(const char *input, char *output, size_t output_size) {
  size_t used = 0;
  if (!output || output_size == 0) return;
  output[0] = '\0';
  br_snprintf_append(output, output_size, &used, "'");
  for (const char *cursor = input ? input : ""; *cursor; cursor += 1) {
    if (*cursor == '\'') {
      br_snprintf_append(output, output_size, &used, "'\\''");
    } else if (used + 1 < output_size) {
      output[used++] = *cursor;
      output[used] = '\0';
    }
  }
  br_snprintf_append(output, output_size, &used, "'");
}

static bool br_server_env_enabled(const char *name, bool fallback) {
  const char *value = getenv(name);
  if (!value || value[0] == '\0') return fallback;
  return strcmp(value, "0") != 0 &&
         strcasecmp(value, "false") != 0 &&
         strcasecmp(value, "no") != 0 &&
         strcasecmp(value, "off") != 0;
}

static void br_server_play_task_done_sound(br_server_state *server) {
  char script_q[BR_MAX_PATH * 2];
  char root_q[BR_MAX_PATH * 2];
  char dev_q[192];
  char volume_q[64];
  char command[BR_MAX_PATH * 4];
  const char *dev;
  const char *volume;

  if (!server || !br_server_env_enabled("PET_TASK_DONE_SOUND_ENABLED", true)) {
    return;
  }
  if (access(server->config.sound_script_path, R_OK) != 0) {
    return;
  }

  dev = getenv("PET_TASK_DONE_SOUND_DEV");
  if (!dev || dev[0] == '\0') dev = "plughw:0,0";
  volume = getenv("PET_TASK_DONE_SOUND_VOLUME");
  if (!volume || volume[0] == '\0') volume = "0.18";

  br_server_shell_quote(server->config.sound_script_path, script_q, sizeof(script_q));
  br_server_shell_quote(server->config.root_dir, root_q, sizeof(root_q));
  br_server_shell_quote(dev, dev_q, sizeof(dev_q));
  br_server_shell_quote(volume, volume_q, sizeof(volume_q));

  snprintf(command,
           sizeof(command),
           "PET_TASK_DONE_SOUND_DEV=%s PET_TASK_DONE_SOUND_VOLUME=%s sh %s task_done %s >/dev/null 2>&1",
           dev_q,
           volume_q,
           script_q,
           root_q);
  br_server_spawn_shell(command);
  br_server_logf("task done sound queued");
}

static void br_server_build_pairing_hint(br_server_state *server, br_pairing_state state, char *output, size_t output_size) {
  if (!output || output_size == 0) {
    return;
  }
  output[0] = '\0';
  if (state == BR_PAIRING_WAITING_CONFIG || state == BR_PAIRING_LAN_DISCOVERY ||
      state == BR_PAIRING_AP_FALLBACK) {
    br_normalize_text("请打开电脑端 Pet Manager 进行配网。", "", output, output_size);
    return;
  }
  br_normalize_text("", "", output, output_size);
}

static void br_server_broadcast_pairing_state(br_server_state *server, const char *reason) {
  char message[BR_MAX_JSON];
  size_t used = 0;
  message[0] = '\0';
  br_snprintf_append(message, sizeof(message), &used, "{\"type\":\"pairing_state\",\"state\":\"");
  br_json_escape_append(message, sizeof(message), &used, br_pairing_state_name(server->pairing.state));
  br_snprintf_append(message, sizeof(message), &used, "\",\"mode\":\"");
  br_json_escape_append(message, sizeof(message), &used, br_pairing_mode_name(server->pairing.state));
  br_snprintf_append(message, sizeof(message), &used, "\",\"reason\":\"");
  br_json_escape_append(message, sizeof(message), &used, reason ? reason : "");
  br_snprintf_append(message, sizeof(message), &used, "\",\"boardDeviceId\":\"");
  br_json_escape_append(message, sizeof(message), &used, server->config.board_device_id);
  br_snprintf_append(message, sizeof(message), &used, "\",\"apIp\":\"");
  br_json_escape_append(message, sizeof(message), &used, server->config.ap_ip);
  br_snprintf_append(message, sizeof(message), &used, "\",\"apSsid\":\"");
  br_json_escape_append(message, sizeof(message), &used, server->config.ap_ssid);
  br_snprintf_append(message, sizeof(message), &used, "\",\"apPsk\":\"");
  br_json_escape_append(message, sizeof(message), &used, server->config.ap_psk);
  br_snprintf_append(message, sizeof(message), &used, "\",\"discoveryUdpPort\":%d,\"discoveryMdnsPort\":%d,\"tsMs\":%lld}",
                     server->config.discovery_udp_port,
                     server->config.discovery_mdns_port,
                     br_now_ms());
  br_broadcast_json(server, message);
}

static void br_server_apply_pairing_runtime_state(br_server_state *server, br_pairing_state state) {
  char hint[256];
  if (!server) {
    return;
  }

  br_server_build_pairing_hint(server, state, hint, sizeof(hint));
  br_normalize_text(hint, "", server->pairing_message, sizeof(server->pairing_message));
  if (br_pairing_is_waiting(state)) {
    br_write_state_files(server, "waiting_user", "PairingWaiting");
  } else if (server->ap_mode_active) {
    br_write_state_files(server, "idle", "PairingReady");
  } else {
    br_write_state_files(server, "idle", "");
  }
  br_atomic_write_text(server->config.current_speech_path, server->pairing_message);
}

static void br_server_on_pairing_transition(
  br_server_state *server,
  br_pairing_state previous_state,
  br_pairing_state next_state,
  const char *reason
) {
  if (!server) {
    return;
  }
  if (next_state == BR_PAIRING_AP_FALLBACK) {
    br_server_set_ap_mode(server, true);
  } else if (previous_state == BR_PAIRING_AP_FALLBACK && next_state != BR_PAIRING_AP_FALLBACK) {
    br_server_set_ap_mode(server, false);
  }
  br_server_apply_pairing_runtime_state(server, next_state);
  br_server_broadcast_pairing_state(server, reason);
  br_server_logf("pairing state: %s -> %s (%s)",
                 br_pairing_state_name(previous_state),
                 br_pairing_state_name(next_state),
                 reason ? reason : "");
}

static void br_server_discovery_send(br_server_state *server, int fd, const char *ip, int port, const char *transport, const char *requester) {
  struct sockaddr_in target;
  char payload[BR_MAX_JSON];
  if (!server || fd < 0 || !ip || port <= 0 || port > 65535) {
    return;
  }
  memset(&target, 0, sizeof(target));
  target.sin_family = AF_INET;
  target.sin_port = htons((uint16_t) port);
  target.sin_addr.s_addr = inet_addr(ip);
  br_server_build_discovery_json(server, transport, requester, payload, sizeof(payload));
  (void) sendto(fd, payload, strlen(payload), 0, (const struct sockaddr *) &target, sizeof(target));
}

static void br_server_discovery_announce(br_server_state *server, long long now_ms) {
  if (!server) {
    return;
  }
  if (!br_pairing_is_waiting(server->pairing.state)) {
    return;
  }
  if (server->last_discovery_announce_ms > 0 &&
      now_ms - server->last_discovery_announce_ms < server->config.discovery_announce_interval_ms) {
    return;
  }
  server->last_discovery_announce_ms = now_ms;
  br_server_discovery_send(server,
                           server->discovery_fd,
                           "255.255.255.255",
                           server->config.discovery_udp_port,
                           "udp_broadcast",
                           "");
  br_server_discovery_send(server,
                           server->mdns_fd,
                           "224.0.0.251",
                           server->config.discovery_mdns_port,
                           "mdns_multicast",
                           "");
}

static void br_server_poll_discovery(br_server_state *server) {
  struct sockaddr_in peer;
  socklen_t peer_size = sizeof(peer);
  char packet[BR_DISCOVERY_PACKET_MAX];
  if (!server || server->discovery_fd < 0) {
    return;
  }

  while (true) {
    ssize_t read_size = recvfrom(server->discovery_fd,
                                 packet,
                                 sizeof(packet) - 1,
                                 0,
                                 (struct sockaddr *) &peer,
                                 &peer_size);
    if (read_size <= 0) {
      break;
    }
    packet[read_size] = '\0';
    if (strstr(packet, "PET_MANAGER_DISCOVER") ||
        strstr(packet, "\"type\":\"discover\"") ||
        strstr(packet, "\"command\":\"discover\"")) {
      br_pairing_state previous = server->pairing.state;
      long long now_ms = br_now_ms();
      char peer_ip[64];
      const char *resolved_ip = inet_ntoa(peer.sin_addr);
      br_normalize_text(resolved_ip, "", peer_ip, sizeof(peer_ip));
      br_normalize_text(peer_ip, "", server->last_discovery_peer, sizeof(server->last_discovery_peer));
      br_server_discovery_send(server,
                               server->discovery_fd,
                               peer_ip,
                               ntohs(peer.sin_port),
                               "udp_reply",
                               peer_ip);
      if (br_pairing_mark_discovered(&server->pairing, now_ms)) {
        br_server_on_pairing_transition(server, previous, server->pairing.state, "discovered_by_pet_manager");
      }
    }
    peer_size = sizeof(peer);
  }
}

static void br_server_open_discovery_channels(br_server_state *server) {
  // LAN discovery disabled: we rely exclusively on MQTT for PC <-> board
  // rendezvous. HTTP `/discovery` remains available as a local status probe
  // but the UDP broadcast / mDNS multicast channels are no longer opened.
  if (!server) {
    return;
  }
  server->discovery_fd = -1;
  server->mdns_fd = -1;
}

static void br_server_close_discovery_channels(br_server_state *server) {
  if (!server) {
    return;
  }
  if (server->discovery_fd >= 0) {
    close(server->discovery_fd);
    server->discovery_fd = -1;
  }
  if (server->mdns_fd >= 0) {
    close(server->mdns_fd);
    server->mdns_fd = -1;
  }
}

static void br_publish_online_payload(br_server_state *server, bool online, char *output, size_t output_size) {
  char ts[64];
  char broker_url[256];
  br_mqtt_endpoint endpoint;
  size_t used = 0;
  const char *host = server->config.public_host[0] ? server->config.public_host : "127.0.0.1";
  const char *mode = br_pairing_mode_name(server->pairing.state);

  br_iso8601_now(ts, sizeof(ts));
  if (server->mqtt.host[0] != '\0' && server->mqtt.port > 0) {
    snprintf(broker_url, sizeof(broker_url), "mqtt://%s:%d", server->mqtt.host, server->mqtt.port);
  } else if (br_parse_mqtt_url(server->config.mqtt_url, &endpoint)) {
    snprintf(broker_url, sizeof(broker_url), "mqtt://%s:%d", endpoint.host, endpoint.port);
  } else {
    snprintf(broker_url, sizeof(broker_url), "%s", server->config.mqtt_url);
  }

  output[0] = '\0';
  br_snprintf_append(output, output_size, &used, "{");
  br_snprintf_append(output, output_size, &used, "\"boardDeviceId\":\"");
  br_json_escape_append(output, output_size, &used, server->config.board_device_id);
  br_snprintf_append(output, output_size, &used, "\",\"name\":\"");
  br_json_escape_append(output, output_size, &used, server->config.screen_name);
  br_snprintf_append(output, output_size, &used, "\",\"model\":\"");
  br_json_escape_append(output, output_size, &used, server->config.screen_model);
  br_snprintf_append(output, output_size, &used, "\",\"mode\":\"");
  br_json_escape_append(output, output_size, &used, mode);
  br_snprintf_append(output, output_size, &used, "\",\"pairingState\":\"");
  br_json_escape_append(output, output_size, &used, br_pairing_state_name(server->pairing.state));
  br_snprintf_append(output, output_size, &used, "\",\"broker\":\"client\",\"brokerUrl\":\"");
  br_json_escape_append(output, output_size, &used, broker_url);
  br_snprintf_append(output, output_size, &used, "\",\"fw\":\"");
  br_json_escape_append(output, output_size, &used, server->config.screen_fw);
  br_snprintf_append(output, output_size, &used, "\",\"runtime\":\"board-server-c\",\"online\":%s", online ? "true" : "false");
  br_snprintf_append(output, output_size, &used, ",\"host\":\"");
  br_json_escape_append(output, output_size, &used, host);
  br_snprintf_append(output, output_size, &used, "\",\"localDeviceId\":\"");
  br_json_escape_append(output, output_size, &used, server->config.local_device_id);
  br_snprintf_append(output, output_size, &used, "\",\"desktopDeviceId\":\"");
  br_json_escape_append(output, output_size, &used, server->config.target_device_id);
  br_snprintf_append(output, output_size, &used, "\",\"targetDeviceId\":\"");
  br_json_escape_append(output, output_size, &used, server->config.target_device_id);
  br_snprintf_append(output, output_size, &used, "\",\"targetSource\":\"");
  br_json_escape_append(output, output_size, &used, server->config.target_source);
  br_snprintf_append(output, output_size, &used, "\",\"mqttNamespace\":\"");
  br_json_escape_append(output, output_size, &used, server->config.mqtt_namespace);
  br_snprintf_append(output, output_size, &used, "\",\"sourceStateTopic\":\"");
  br_json_escape_append(output, output_size, &used,
                        server->config.target_source[0] ? server->current_state_topic : server->wildcard_state_topic);
  br_snprintf_append(output, output_size, &used, "\",\"sourceSpeechTopic\":\"");
  br_json_escape_append(output, output_size, &used, server->current_speech_topic);
  br_snprintf_append(output, output_size, &used, "\",\"inputActionTopic\":\"");
  br_json_escape_append(output, output_size, &used, server->input_action_topic);
  br_snprintf_append(output, output_size, &used, "\",\"uiUrl\":\"");
  if (server->config.public_url[0]) {
    br_json_escape_append(output, output_size, &used, server->config.public_url);
  } else {
    br_snprintf_append(output, output_size, &used, "http://%s:%d", host, server->config.http_port);
  }
  br_snprintf_append(output, output_size, &used, "\",\"ts\":\"");
  br_json_escape_append(output, output_size, &used, ts);
  br_snprintf_append(output, output_size, &used, "\",\"tsMs\":%lld}", br_now_ms());
}

/* Forward declaration: defined after br_handle_mqtt_publish */
static int br_server_publish(br_server_state *server, const char *full_topic,
                             const char *virtual_topic, const char *payload, bool retain);

static void br_server_publish_presence(br_server_state *server, bool online) {
  char payload[BR_MAX_JSON];
  br_publish_online_payload(server, online, payload, sizeof(payload));
  br_server_publish(server, server->hello_topic, "hello", payload, true);
  br_server_publish(server, server->availability_topic, "availability", payload, true);
}

static void br_server_subscribe_topics(br_server_state *server) {
  if (server->config.target_source[0]) {
    br_mqtt_client_subscribe(&server->mqtt, server->current_state_topic);
  } else {
    br_mqtt_client_subscribe(&server->mqtt, server->wildcard_state_topic);
  }
  br_mqtt_client_subscribe(&server->mqtt, server->current_speech_topic);
  br_mqtt_client_subscribe(&server->mqtt, server->control_topic);
  br_mqtt_client_subscribe(&server->mqtt, server->command_topic);
  br_mqtt_client_subscribe(&server->mqtt, server->input_action_topic);
  br_mqtt_client_subscribe(&server->mqtt, server->screen_page_topic);
}

/* page resolution: input 可能是 JSON `{"page":"stats|main"}` 或纯字符串
 * "stats" / "main" / "➡" / "➜"。返回 true 时 out 收到归一化值。 */
static bool br_resolve_screen_page(const char *input, char *out, size_t out_size) {
  return br_screen_page_resolve(input, out, out_size);
}

static void br_server_apply_screen_page(br_server_state *server, const char *page,
                                        const char *origin) {
  if (!server || !page) return;
  if (br_atomic_write_text(server->config.screen_page_path, page)) {
    br_server_logf("screen-page: %s (origin=%s)", page, origin ? origin : "?");
  } else {
    br_server_logf("screen-page: write failed (%s) origin=%s",
                   server->config.screen_page_path, origin ? origin : "?");
  }
}

static void br_audio_bridge_sanitize_arg(
  const char *input,
  const char *fallback,
  char *output,
  size_t output_size
) {
  char trimmed[128];
  size_t used = 0;
  if (!output || output_size == 0) {
    return;
  }
  output[0] = '\0';
  br_normalize_text(input, fallback ? fallback : "", trimmed, sizeof(trimmed));
  for (size_t i = 0; trimmed[i] != '\0' && used + 1 < output_size; i += 1) {
    unsigned char ch = (unsigned char) trimmed[i];
    if ((ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') ||
        ch == '_' || ch == '-' || ch == '.' || ch == ':' ||
        ch == '/' || ch == ',') {
      output[used++] = (char) ch;
    }
  }
  output[used] = '\0';
  if (output[0] == '\0' && fallback && fallback[0] != '\0') {
    br_audio_bridge_sanitize_arg(fallback, "", output, output_size);
  }
}

static void br_server_run_audio_bridge_script(br_server_state *server, const char *action) {
  char cmd[BR_MAX_PATH + 160];
  if (!server || !action || action[0] == '\0') {
    return;
  }
  snprintf(cmd, sizeof(cmd), "sh '%s/board-audio-bridge.sh' %s '%s'",
           server->config.root_dir,
           action,
           server->config.root_dir);
  br_server_spawn_shell(cmd);
}

static bool br_server_write_audio_bridge_config(
  br_server_state *server,
  const br_audio_bridge_command *command
) {
  char pc_ip[64];
  char capture_dev[96];
  char play_dev[96];
  char content[768];

  if (!server || !command || !command->enabled) {
    return false;
  }
  br_audio_bridge_sanitize_arg(command->pc_ip, "", pc_ip, sizeof(pc_ip));
  br_audio_bridge_sanitize_arg(command->capture_dev, "default", capture_dev, sizeof(capture_dev));
  br_audio_bridge_sanitize_arg(command->play_dev, "default", play_dev, sizeof(play_dev));
  if (pc_ip[0] == '\0') {
    return false;
  }
  snprintf(content, sizeof(content),
           "AUDIO_BRIDGE_ENABLED=1\n"
           "AUDIO_BRIDGE_PC_IP=%s\n"
           "AUDIO_BRIDGE_PC_PORT=%d\n"
           "AUDIO_BRIDGE_LISTEN_PORT=%d\n"
           "AUDIO_BRIDGE_CAPTURE_DEV=%s\n"
           "AUDIO_BRIDGE_PLAY_DEV=%s\n"
           "AUDIO_BRIDGE_VOICE_BUTTON=%s\n",
           pc_ip,
           command->pc_port,
           command->listen_port,
           capture_dev,
           play_dev,
           command->voice_button);
  if (!br_atomic_write_text(server->config.audio_bridge_config_path, content)) {
    return false;
  }
  if (!br_atomic_write_text(server->config.voice_button_config_path, command->voice_button)) {
    return false;
  }
  return true;
}

static void br_server_handle_audio_bridge_command(
  br_server_state *server,
  const br_audio_bridge_command *command
) {
  if (!server || !command) {
    return;
  }
  if (command->enabled) {
    if (!br_server_write_audio_bridge_config(server, command)) {
      br_server_logf("MQTT command: audio_bridge start failed to write config");
      return;
    }
    br_server_logf("MQTT command: audio_bridge start pc=%s:%d listen=%d voice_button=%s",
                   command->pc_ip,
                   command->pc_port,
                   command->listen_port,
                   command->voice_button);
    br_server_run_audio_bridge_script(server, "start");
  } else {
    br_server_logf("MQTT command: audio_bridge stop");
    br_server_run_audio_bridge_script(server, "stop");
    (void) unlink(server->config.audio_bridge_config_path);
    (void) unlink(server->config.voice_button_config_path);
  }
}

static void br_server_send_button_config_ack(
  br_server_state *server,
  const br_button_config_command *command,
  bool ok,
  const char *message
) {
  char payload[BR_MAX_JSON];
  size_t used = 0;
  payload[0] = '\0';
  br_snprintf_append(payload, sizeof(payload), &used,
                     "{\"ok\":%s,\"requestId\":\"", ok ? "true" : "false");
  br_json_escape_append(payload, sizeof(payload), &used,
                        command ? command->request_id : "");
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"bindingCount\":%zu",
                     command ? command->binding_count : 0);
  if (message && message[0]) {
    br_snprintf_append(payload, sizeof(payload), &used, ",\"message\":\"");
    br_json_escape_append(payload, sizeof(payload), &used, message);
    br_snprintf_append(payload, sizeof(payload), &used, "\"");
  }
  br_snprintf_append(payload, sizeof(payload), &used, "}");
  (void) br_usb_serial_send(&server->usb_serial, "button-config-ack", payload);
}

static void br_server_handle_button_config_command(
  br_server_state *server,
  const br_button_config_command *command,
  const char *payload
) {
  if (!server || !command || !payload) {
    return;
  }
  if (!br_atomic_write_text(server->config.button_config_path, payload)) {
    br_server_logf("control command: button_config failed to write %s",
                   server->config.button_config_path);
    br_server_send_button_config_ack(server, command, false, "write .button-config failed");
    return;
  }
  if (command->voice_enabled) {
    (void) br_atomic_write_text(server->config.voice_button_config_path, command->voice_button);
  } else {
    (void) unlink(server->config.voice_button_config_path);
  }
  br_server_send_button_config_ack(server, command, true, "button config written");
  br_server_logf("control command: button_config bindings=%zu voice=%s enabled=%d",
                 command->binding_count,
                 command->voice_button,
                 command->voice_enabled ? 1 : 0);
}

static void br_server_rebind(br_server_state *server, const br_remote_binding *binding) {
  char old_state[BR_MAX_TOPIC];
  char old_wildcard[BR_MAX_TOPIC];
  char old_speech[BR_MAX_TOPIC];
  if (!binding || !binding->matched) {
    return;
  }
  br_normalize_text(server->current_state_topic, "", old_state, sizeof(old_state));
  br_normalize_text(server->wildcard_state_topic, "", old_wildcard, sizeof(old_wildcard));
  br_normalize_text(server->current_speech_topic, "", old_speech, sizeof(old_speech));
  br_normalize_text(binding->target_device_id, server->config.local_device_id, server->config.target_device_id, sizeof(server->config.target_device_id));
  br_normalize_text(binding->target_source, "", server->config.target_source, sizeof(server->config.target_source));
  br_build_topics(server);
  br_server_logf("remote binding: target=%s src=%s",
                 server->config.target_device_id,
                 server->config.target_source[0] ? server->config.target_source : "state/+");
  if (server->mqtt.connected) {
    if (old_state[0]) br_mqtt_client_unsubscribe(&server->mqtt, old_state);
    if (old_wildcard[0]) br_mqtt_client_unsubscribe(&server->mqtt, old_wildcard);
    if (old_speech[0]) br_mqtt_client_unsubscribe(&server->mqtt, old_speech);
    br_server_subscribe_topics(server);
    br_server_publish_presence(server, true);
  }
}

static void br_handle_mqtt_publish(const char *topic, const char *payload, void *userdata) {
  br_server_state *server = (br_server_state *) userdata;
  br_bridge_state_update update;
  char payload_object[BR_MAX_JSON];
  char source[128];
  long long payload_ts_ms = 0;
  long long received_at = br_now_ms();
  char message_json[BR_MAX_JSON];
  size_t used = 0;

  if (!server || !topic || !payload) {
    return;
  }

  if (strcmp(topic, server->control_topic) == 0) {
    br_remote_binding binding;
    if (br_parse_remote_binding_json(payload, &binding)) {
      br_server_rebind(server, &binding);
    }
    return;
  }

  if (strcmp(topic, server->screen_page_topic) == 0) {
    char page[16];
    if (br_resolve_screen_page(payload, page, sizeof(page))) {
      br_server_apply_screen_page(server, page, "mqtt");
    } else {
      br_server_logf("screen-page: ignored mqtt payload (%.64s)", payload);
    }
    return;
  }

  if (strcmp(topic, server->command_topic) == 0) {
    br_audio_bridge_command audio_command;
    br_button_config_command button_command;
    if (br_parse_audio_bridge_command_json(payload, &audio_command)) {
      br_server_handle_audio_bridge_command(server, &audio_command);
      return;
    }
    if (br_parse_button_config_command_json(payload, &button_command)) {
      br_server_handle_button_config_command(server, &button_command, payload);
      return;
    }
    if (strstr(payload, "factory_reset")) {
      /* Only honour factory_reset when the device is in STA_READY (paired)
         and has been in that state for at least 10 seconds, so a stale or
         duplicate message arriving right after pairing cannot immediately
         undo the new configuration. */
      if (server->pairing.state != BR_PAIRING_STA_READY) {
        br_server_logf("MQTT command: factory_reset ignored (state=%s, not sta_ready)",
                       br_pairing_state_name(server->pairing.state));
        return;
      }
      long long in_sta_ms = br_now_ms() - server->pairing.entered_ms;
      if (in_sta_ms < 10000) {
        br_server_logf("MQTT command: factory_reset ignored (sta_ready for %lld ms < 10s guard)",
                       in_sta_ms);
        return;
      }
      br_server_logf("MQTT command: factory_reset");
      br_pairing_state previous = server->pairing.state;
      (void) unlink(server->config.network_config_path);
      br_server_spawn_shell("killall wpa_supplicant udhcpc 2>/dev/null");
      if (br_pairing_reset_to_waiting(&server->pairing, br_now_ms())) {
        br_server_on_pairing_transition(server, previous, server->pairing.state, "mqtt_factory_reset");
      }
      previous = server->pairing.state;
      if (br_pairing_tick(&server->pairing, br_now_ms())) {
        br_server_on_pairing_transition(server, previous, server->pairing.state, "restart_discovery");
      }
    }
    return;
  }

  if (strcmp(topic, server->current_speech_topic) == 0) {
    br_speech_update speech_update;
    char speech[2048] = {0};
    char message_complete[BR_MAX_JSON];
    if (br_parse_speech_update(payload, &speech_update) &&
        br_server_apply_speech_update(server, &speech_update, br_now_ms(), speech, sizeof(speech))) {
      br_atomic_write_text(server->config.current_speech_path, speech);
      br_write_speech_hold_until_abs(server, br_server_latest_speech_expiry_ms(server));
      br_write_screen_interrupt(server, "speech");
    }
    if (br_build_message_complete_json(payload, speech, message_complete, sizeof(message_complete))) {
      br_broadcast_json(server, message_complete);
    }
    return;
  }

  if (!server->use_legacy_active_topic && br_is_legacy_active_topic(server, topic)) {
    return;
  }

  if (!br_payload_to_json_object(payload, payload_object, sizeof(payload_object), &payload_ts_ms, source, sizeof(source))) {
    return;
  }

  message_json[0] = '\0';
  br_snprintf_append(message_json, sizeof(message_json), &used, "{\"type\":\"bridge_state\",\"topic\":\"");
  br_json_escape_append(message_json, sizeof(message_json), &used, topic);
  br_snprintf_append(message_json, sizeof(message_json), &used, "\",\"payload\":%s,\"transport\":\"mqtt\",\"_bridgeReceivedAt\":%lld",
                     payload_object,
                     received_at);
  if (payload_ts_ms > 0) {
    br_snprintf_append(message_json, sizeof(message_json), &used, ",\"_bridgePayloadTsMs\":%lld,\"_bridgeAgeMs\":%lld",
                       payload_ts_ms,
                       received_at - payload_ts_ms);
  }
  br_snprintf_append(message_json, sizeof(message_json), &used, ",\"_wsSeq\":%u,\"_wsSentAt\":%lld}",
                     ++server->ws_seq,
                     br_now_ms());

  br_update_snapshot(server, topic, source, message_json);
  br_broadcast_json(server, message_json);

  if (br_bridge_state_from_message(topic, payload, &update)) {
    if (update.has_token_usage) {
      runtime_stats_ingest(&update, br_now_ms());
    }
    if (update.should_write) {
      br_session_resolution resolution;
      if (br_session_machine_apply(&server->session_machine, &update, br_now_ms(), &resolution)) {
        br_write_state_files_with_reason(server, resolution.state, resolution.event, resolution.reason);
        if (resolution.should_interrupt) {
          br_write_screen_interrupt(server, resolution.state);
        }
        if (strcmp(resolution.state, "done") == 0) {
          br_server_play_task_done_sound(server);
        }
        snprintf(server->last_state, sizeof(server->last_state), "%s", resolution.state);
        snprintf(server->last_reason, sizeof(server->last_reason), "%s", resolution.reason);
        server->last_speech_rewrite_ms = br_now_ms();
        server->last_state_update_ms = br_now_ms();
      }
    }
  }
}

/* --- Transport abstraction: publish via MQTT or USB serial --- */

static int br_server_publish(br_server_state *server, const char *full_topic,
                             const char *virtual_topic, const char *payload, bool retain) {
  if (server->transport_mode == BR_TRANSPORT_USB) {
    return br_usb_serial_send(&server->usb_serial, virtual_topic, payload);
  }
  return br_mqtt_client_publish(&server->mqtt, full_topic, payload, retain);
}

/* --- USB asset transfer utilities (ported from board_serial_bridge.c) --- */

static int br_asset_base64_value(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  if (ch == '=') return -2;
  return -1;
}

static int br_asset_base64_decode(const char *input, unsigned char *output, size_t output_size) {
  int value = 0, value_bits = -8;
  size_t used = 0;
  for (; input && *input; input++) {
    int d = br_asset_base64_value(*input);
    if (d == -2) break;
    if (d < 0) return -1;
    value = (value << 6) | d;
    value_bits += 6;
    if (value_bits >= 0) {
      if (used >= output_size) return -1;
      output[used++] = (unsigned char)((value >> value_bits) & 0xff);
      value_bits -= 8;
    }
  }
  return (int)used;
}

static int br_asset_write_all(int fd, const char *data, size_t len) {
  while (len > 0) {
    ssize_t w = write(fd, data, len);
    if (w < 0) { if (errno == EINTR) continue; return -1; }
    data += w; len -= (size_t)w;
  }
  return 0;
}

static int br_asset_remove_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
  if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *ent;
    if (!dir) return -1;
    while ((ent = readdir(dir)) != NULL) {
      char child[BR_MAX_PATH];
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      if (br_asset_remove_tree(child) != 0) { closedir(dir); return -1; }
    }
    closedir(dir);
    return rmdir(path);
  }
  return unlink(path);
}

static int br_asset_mkdir_p(const char *path) {
  char temp[BR_MAX_PATH]; char *p;
  br_normalize_text(path, "", temp, sizeof(temp));
  if (!temp[0]) return -1;
  for (p = temp + 1; *p; p++) {
    if (*p == '/') { *p = '\0'; if (mkdir(temp, 0755) != 0 && errno != EEXIST) return -1; *p = '/'; }
  }
  if (mkdir(temp, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
}

static int br_asset_ensure_parent(const char *path) {
  char dir[BR_MAX_PATH]; char *slash;
  br_normalize_text(path, "", dir, sizeof(dir));
  slash = strrchr(dir, '/');
  if (!slash) return 0;
  *slash = '\0';
  return br_asset_mkdir_p(dir);
}

static bool br_asset_safe_path(const char *rel) {
  const char *p;
  if (!rel || !rel[0] || rel[0] == '/' || strstr(rel, "..") || strchr(rel, '\\')) return false;
  for (p = rel; *p; p++) {
    char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '.' || c == '_' || c == '-' || c == '/') continue;
    return false;
  }
  return true;
}

static bool br_asset_is_audio_patch_path(const char *rel) {
  size_t len;
  const char *name;
  if (!br_asset_safe_path(rel)) return false;
  if (strncmp(rel, "videos/", 7) != 0) return false;
  name = rel + 7;
  if (!name[0] || strchr(name, '/')) return false;
  len = strlen(rel);
  return len > 11 && strcmp(rel + len - 4, ".wav") == 0;
}

static int br_asset_copy_file_atomic(const char *src, const char *dst) {
  char tmp[BR_MAX_PATH];
  char buffer[16384];
  int in_fd, out_fd;
  ssize_t nread;
  if (!src || !dst) return -1;
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", dst) >= (int)sizeof(tmp)) return -1;
  if (br_asset_ensure_parent(dst) != 0) return -1;
  in_fd = open(src, O_RDONLY);
  if (in_fd < 0) return -1;
  out_fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (out_fd < 0) {
    close(in_fd);
    return -1;
  }
  while ((nread = read(in_fd, buffer, sizeof(buffer))) > 0) {
    if (br_asset_write_all(out_fd, buffer, (size_t)nread) != 0) {
      close(in_fd);
      close(out_fd);
      unlink(tmp);
      return -1;
    }
  }
  if (nread < 0) {
    close(in_fd);
    close(out_fd);
    unlink(tmp);
    return -1;
  }
  close(in_fd);
  if (fsync(out_fd) != 0) {
    close(out_fd);
    unlink(tmp);
    return -1;
  }
  close(out_fd);
  if (rename(tmp, dst) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

static void br_asset_send_ack_ex(
  br_server_state *server,
  const char *tid,
  const char *phase,
  bool ok,
  const char *err,
  const char *path,
  bool has_size,
  unsigned long long size,
  const char *checksum
) {
  /* Build just the inner payload — let br_usb_serial_send wrap it with the
     {"topic":..,"payload":..} envelope + atomic newline. Used to construct
     the full envelope here and call send_raw, but send_raw historically
     wrote the json body and the trailing newline in two separate write_all
     calls; on a CPU-saturated host the newline could be dropped mid-stream
     and concatenate this ack onto the next packet → host BufReader rejected
     the whole line as "invalid JSON" and the desktop client's OTA waiter
     timed out (manifested as "未收到板端素材 OTA 确认" / failed appearance
     swap / silent widget install). send() builds one buffer and write_all's
     once, eliminating the race. */
  char payload[BR_MAX_JSON]; size_t used = 0;
  payload[0] = '\0';
  br_snprintf_append(payload, sizeof(payload), &used, "{\"transferId\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, tid ? tid : "");
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"phase\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, phase ? phase : "");
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"ok\":%s", ok ? "true" : "false");
  if (path && path[0]) {
    br_snprintf_append(payload, sizeof(payload), &used, ",\"path\":\"");
    br_json_escape_append(payload, sizeof(payload), &used, path);
    br_snprintf_append(payload, sizeof(payload), &used, "\"");
  }
  if (has_size) {
    br_snprintf_append(payload, sizeof(payload), &used, ",\"size\":%llu", size);
  }
  if (checksum && checksum[0]) {
    br_snprintf_append(payload, sizeof(payload), &used, ",\"checksum\":\"");
    br_json_escape_append(payload, sizeof(payload), &used, checksum);
    br_snprintf_append(payload, sizeof(payload), &used, "\"");
  }
  if (err && err[0]) {
    br_snprintf_append(payload, sizeof(payload), &used, ",\"error\":\"");
    br_json_escape_append(payload, sizeof(payload), &used, err);
    br_snprintf_append(payload, sizeof(payload), &used, "\"");
  }
  br_snprintf_append(payload, sizeof(payload), &used, "}");
  (void) br_usb_serial_send(&server->usb_serial, "asset/ack", payload);
}

static void br_asset_send_ack(br_server_state *server, const char *tid, const char *phase, bool ok, const char *err) {
  br_asset_send_ack_ex(server, tid, phase, ok, err, NULL, false, 0, NULL);
}

/* forward declarations — defined below alongside the appearance asset handlers */
static int br_asset_decode_b64_files(const char *dir_path);
static int br_asset_decode_b64_file(const char *b64_path, const char *outpath);
static int br_asset_file_stats_checksum(
  const char *path,
  unsigned long long *size_out,
  char checksum_hex[17]
);
static int br_asset_tree_stats(
  const char *dir_path,
  unsigned long long *file_count,
  unsigned long long *byte_count,
  bool *has_b64
);
static int br_asset_remove_tree(const char *path);
static int br_asset_mkdir_p(const char *path);
static int br_asset_ensure_parent(const char *path);
static bool br_asset_safe_path(const char *p);
static bool br_asset_is_audio_patch_path(const char *p);
static int br_asset_write_all(int fd, const char *buf, size_t len);

/* ─────────── widget OTA handlers ───────────────────────────────────────────
   Parallel to asset_* (appearance OTA), but for .clawpkg widget directories:
     widget/begin   {transferId, widgetId}     → create .incoming-widget staging
     widget/chunk   {transferId, path, data, index}  → write b64 chunk (reuses asset chunk semantics, same file format .b64)
     widget/commit  {transferId, widgetId}     → decode b64 → move to widgets/<id>/ → write .active-widget
   On commit the existing widget at widgets/<id>/ is rotated to widgets/<id>.previous/
   so a botched OTA can be rolled back. board-widget-runtime sees .active-widget
   change via inotify-equivalent poll and reloads.
   ───────────────────────────────────────────────────────────────────────────── */

static void br_widget_send_ack(br_server_state *server, const char *tid, const char *phase, bool ok, const char *msg) {
  /* Emit as {"topic":"widget-install-ack","payload":{transferId,phase,ok,msg}}
     so the desktop client's BufReader/SerialMessage parser (which requires a
     top-level `topic` field) can dispatch it into a waiter. Previously this
     was sent via br_usb_serial_send_raw as bare {"type":"widget_install_ack",...}
     which the host silently dropped as "invalid JSON" — leaving widget OTA
     fire-and-forget on the host side. Mirror the button-config-ack pattern. */
  char payload[BR_MAX_JSON]; size_t used = 0;
  br_snprintf_append(payload, sizeof(payload), &used, "{\"transferId\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, tid ? tid : "");
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"phase\":\"%s\",\"ok\":%s",
                     phase, ok ? "true" : "false");
  if (msg && msg[0]) {
    br_snprintf_append(payload, sizeof(payload), &used, ",\"msg\":\"");
    br_json_escape_append(payload, sizeof(payload), &used, msg);
    br_snprintf_append(payload, sizeof(payload), &used, "\"");
  }
  br_snprintf_append(payload, sizeof(payload), &used, "}");
  (void) br_usb_serial_send(&server->usb_serial, "widget-install-ack", payload);
}

static void br_handle_widget_install_begin(br_server_state *server, const char *payload) {
  br_json_token tokens[32]; char tid[128]; char wid[128]; char staging[BR_MAX_PATH];
  int count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) { br_widget_send_ack(server, "", "begin", false, "bad json"); return; }
  int ti = br_json_find_key(payload, tokens, count, 0, "transferId");
  int wi = br_json_find_key(payload, tokens, count, 0, "widgetId");
  tid[0] = wid[0] = '\0';
  if (ti >= 0) br_json_token_to_string(payload, &tokens[ti], tid, sizeof(tid));
  if (wi >= 0) br_json_token_to_string(payload, &tokens[wi], wid, sizeof(wid));
  if (!tid[0] || !wid[0]) {
    br_widget_send_ack(server, tid, "begin", false, "missing transferId or widgetId"); return;
  }
  /* widget id sanity: kebab-case-ish, no path traversal */
  for (const char *p = wid; *p; p++) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
      br_widget_send_ack(server, tid, "begin", false, "widgetId must be [a-z0-9_-]+"); return;
    }
  }
  snprintf(staging, sizeof(staging), "%s/.incoming-widget", server->config.root_dir);
  if (br_asset_remove_tree(staging) != 0 || br_asset_mkdir_p(staging) != 0) {
    br_widget_send_ack(server, tid, "begin", false, "cannot prepare staging"); return;
  }
  br_server_logf("widget_install_begin: %s id=%s", tid, wid);
  br_widget_send_ack(server, tid, "begin", true, "");
}

static void br_handle_widget_install_chunk(br_server_state *server, const char *payload) {
  br_json_token tokens[32]; char tid[128], rel[BR_MAX_PATH];
  static char data[90000];
  char staging[BR_MAX_PATH], target[BR_MAX_PATH];
  int index_val, flags, fd;
  size_t data_len;

  int count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) { br_widget_send_ack(server, "", "chunk", false, "bad json"); return; }

  int ti = br_json_find_key(payload, tokens, count, 0, "transferId");
  int pi = br_json_find_key(payload, tokens, count, 0, "path");
  int di = br_json_find_key(payload, tokens, count, 0, "data");
  int ii = br_json_find_key(payload, tokens, count, 0, "index");

  tid[0] = rel[0] = data[0] = '\0';
  if (ti >= 0) br_json_token_to_string(payload, &tokens[ti], tid, sizeof(tid));
  if (pi >= 0) br_json_token_to_string(payload, &tokens[pi], rel, sizeof(rel));
  if (di >= 0) br_json_token_to_string(payload, &tokens[di], data, sizeof(data));

  char idx_str[32] = "0";
  if (ii >= 0) br_json_token_to_string(payload, &tokens[ii], idx_str, sizeof(idx_str));
  index_val = atoi(idx_str);

  if (!tid[0] || !br_asset_safe_path(rel) || !data[0]) {
    br_widget_send_ack(server, tid, "chunk", false, "invalid chunk"); return;
  }

  data_len = strlen(data);
  snprintf(staging, sizeof(staging), "%s/.incoming-widget", server->config.root_dir);
  snprintf(target, sizeof(target), "%s/%s.b64", staging, rel);
  if (br_asset_ensure_parent(target) != 0) {
    br_widget_send_ack(server, tid, "chunk", false, "mkdir failed"); return;
  }
  flags = O_CREAT | O_WRONLY | (index_val == 0 ? O_TRUNC : O_APPEND);
  fd = open(target, flags, 0644);
  if (fd < 0) { br_widget_send_ack(server, tid, "chunk", false, "open failed"); return; }
  if (br_asset_write_all(fd, data, data_len) != 0 ||
      br_asset_write_all(fd, "\n", 1) != 0) {
    close(fd); br_widget_send_ack(server, tid, "chunk", false, "write failed"); return;
  }
  close(fd);
  /* skip ack for successful chunks — client streams without waiting */
}

static void br_handle_widget_install_commit(br_server_state *server, const char *payload) {
  br_json_token tokens[32]; char tid[128]; char wid[128];
  char staging[BR_MAX_PATH], widgets_root[BR_MAX_PATH];
  char target[BR_MAX_PATH], previous[BR_MAX_PATH], active_widget_path[BR_MAX_PATH];

  int count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) { br_widget_send_ack(server, "", "commit", false, "bad json"); return; }
  int ti = br_json_find_key(payload, tokens, count, 0, "transferId");
  int wi = br_json_find_key(payload, tokens, count, 0, "widgetId");
  tid[0] = wid[0] = '\0';
  if (ti >= 0) br_json_token_to_string(payload, &tokens[ti], tid, sizeof(tid));
  if (wi >= 0) br_json_token_to_string(payload, &tokens[wi], wid, sizeof(wid));
  if (!tid[0] || !wid[0]) { br_widget_send_ack(server, tid, "commit", false, "missing transferId/widgetId"); return; }

  snprintf(staging, sizeof(staging), "%s/.incoming-widget", server->config.root_dir);
  snprintf(widgets_root, sizeof(widgets_root), "%s/widgets", server->config.root_dir);
  snprintf(target, sizeof(target), "%s/%s", widgets_root, wid);
  snprintf(previous, sizeof(previous), "%s/%s.previous", widgets_root, wid);
  snprintf(active_widget_path, sizeof(active_widget_path), "%s/.active-widget", server->config.root_dir);

  if (access(staging, R_OK) != 0) {
    br_widget_send_ack(server, tid, "commit", false, "staging missing"); return;
  }

  /* Decode all .b64 files in staging to their binary form (reuse the same
     helper as appearance commit). */
  br_server_logf("widget_install_commit: decoding b64 files in staging...");
  if (br_asset_decode_b64_files(staging) != 0) {
    br_widget_send_ack(server, tid, "commit", false, "b64 decode failed"); return;
  }
  /* Ensure widgets/ exists. */
  if (br_asset_mkdir_p(widgets_root) != 0) {
    br_widget_send_ack(server, tid, "commit", false, "mkdir widgets/ failed"); return;
  }
  /* Rotate existing widgets/<id>/ → widgets/<id>.previous/ */
  (void) br_asset_remove_tree(previous);
  if (access(target, F_OK) == 0 && rename(target, previous) != 0) {
    br_widget_send_ack(server, tid, "commit", false, "rotate failed"); return;
  }
  /* Move staging → widgets/<id>/ */
  if (rename(staging, target) != 0) {
    br_widget_send_ack(server, tid, "commit", false, "activate failed"); return;
  }

  /* Activate: write widget id to .active-widget. board-widget-runtime polls
     this file and re-loads the widget. */
  if (!br_atomic_write_text(active_widget_path, wid)) {
    br_widget_send_ack(server, tid, "commit", false, "write .active-widget failed"); return;
  }
  /* Also switch screen-page to stats so the user sees the widget immediately. */
  {
    char screen_page_path[BR_MAX_PATH];
    snprintf(screen_page_path, sizeof(screen_page_path), "%s/.screen-page", server->config.root_dir);
    (void) br_atomic_write_text(screen_page_path, "stats");
  }

  br_server_logf("widget_install_commit: %s id=%s → widgets/%s, active-widget set", tid, wid, wid);
  br_widget_send_ack(server, tid, "commit", true, "");
}

static void br_handle_asset_begin(br_server_state *server, const char *payload) {
  br_json_token tokens[32]; char tid[128]; char staging[BR_MAX_PATH];
  int count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) { br_asset_send_ack(server, "", "begin", false, "bad json"); return; }
  int idx = br_json_find_key(payload, tokens, count, 0, "transferId");
  if (idx < 0 || !br_json_token_to_string(payload, &tokens[idx], tid, sizeof(tid)) || !tid[0]) {
    br_asset_send_ack(server, "", "begin", false, "missing transferId"); return;
  }
  snprintf(staging, sizeof(staging), "%s/.incoming-desktop-pet", server->config.root_dir);
  if (br_asset_remove_tree(staging) != 0 || br_asset_mkdir_p(staging) != 0) {
    br_asset_send_ack(server, tid, "begin", false, "cannot prepare staging"); return;
  }
  br_server_logf("asset_begin: %s", tid);
  br_asset_send_ack(server, tid, "begin", true, "");
}

static void br_handle_asset_stat(br_server_state *server, const char *payload) {
  br_json_token tokens[32];
  char tid[128], rel[BR_MAX_PATH], current_path[BR_MAX_PATH], checksum[17];
  unsigned long long size = 0;
  int count, ti, pi;

  count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) { br_asset_send_ack(server, "", "stat", false, "bad json"); return; }
  ti = br_json_find_key(payload, tokens, count, 0, "transferId");
  pi = br_json_find_key(payload, tokens, count, 0, "path");
  tid[0] = rel[0] = '\0';
  if (ti >= 0) br_json_token_to_string(payload, &tokens[ti], tid, sizeof(tid));
  if (pi >= 0) br_json_token_to_string(payload, &tokens[pi], rel, sizeof(rel));
  if (!tid[0]) { br_asset_send_ack(server, "", "stat", false, "missing transferId"); return; }
  if (!br_asset_safe_path(rel)) {
    br_asset_send_ack_ex(server, tid, "stat", false, "invalid path", rel, false, 0, NULL);
    return;
  }
  if (snprintf(current_path, sizeof(current_path), "%s/.desktop-pet-current/%s",
               server->config.root_dir, rel) >= (int)sizeof(current_path)) {
    br_asset_send_ack_ex(server, tid, "stat", false, "path too long", rel, false, 0, NULL);
    return;
  }
  if (access(current_path, R_OK) != 0) {
    br_asset_send_ack_ex(server, tid, "stat", true, "", rel, false, 0, NULL);
    return;
  }
  if (br_asset_file_stats_checksum(current_path, &size, checksum) != 0) {
    br_asset_send_ack_ex(server, tid, "stat", false, "file stat failed", rel, false, 0, NULL);
    return;
  }
  br_asset_send_ack_ex(server, tid, "stat", true, "", rel, true, size, checksum);
}

static void br_handle_asset_chunk(br_server_state *server, const char *payload) {
  br_json_token tokens[32]; char tid[128], rel[BR_MAX_PATH];
  static char data[90000];
  char staging[BR_MAX_PATH], target[BR_MAX_PATH];
  int index_val, flags, fd;
  size_t data_len;

  int count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) { br_asset_send_ack(server, "", "chunk", false, "bad json"); return; }

  int ti = br_json_find_key(payload, tokens, count, 0, "transferId");
  int pi = br_json_find_key(payload, tokens, count, 0, "path");
  int di = br_json_find_key(payload, tokens, count, 0, "data");
  int ii = br_json_find_key(payload, tokens, count, 0, "index");

  tid[0] = rel[0] = data[0] = '\0';
  if (ti >= 0) br_json_token_to_string(payload, &tokens[ti], tid, sizeof(tid));
  if (pi >= 0) br_json_token_to_string(payload, &tokens[pi], rel, sizeof(rel));
  if (di >= 0) br_json_token_to_string(payload, &tokens[di], data, sizeof(data));

  char idx_str[32] = "0";
  if (ii >= 0) br_json_token_to_string(payload, &tokens[ii], idx_str, sizeof(idx_str));
  index_val = atoi(idx_str);

  if (!tid[0] || !br_asset_safe_path(rel) || !data[0]) {
    br_asset_send_ack(server, tid, "chunk", false, "invalid chunk"); return;
  }

  /* Fast path: write base64 text directly to .b64 staging file.
     Each chunk's base64 is followed by a newline for line-by-line decode at commit.
     Defer decode to commit phase so main loop stays responsive. */
  data_len = strlen(data);
  snprintf(staging, sizeof(staging), "%s/.incoming-desktop-pet", server->config.root_dir);
  snprintf(target, sizeof(target), "%s/%s.b64", staging, rel);
  if (br_asset_ensure_parent(target) != 0) {
    br_asset_send_ack(server, tid, "chunk", false, "mkdir failed"); return;
  }
  flags = O_CREAT | O_WRONLY | (index_val == 0 ? O_TRUNC : O_APPEND);
  fd = open(target, flags, 0644);
  if (fd < 0) { br_asset_send_ack(server, tid, "chunk", false, "open failed"); return; }
  if (br_asset_write_all(fd, data, data_len) != 0 ||
      br_asset_write_all(fd, "\n", 1) != 0) {
    close(fd); br_asset_send_ack(server, tid, "chunk", false, "write failed"); return;
  }
  close(fd);
  /* Skip ack for successful chunks — desktop streams without waiting */
}

static bool br_asset_string_ends_with(const char *text, const char *suffix) {
  size_t text_len;
  size_t suffix_len;
  if (!text || !suffix) {
    return false;
  }
  text_len = strlen(text);
  suffix_len = strlen(suffix);
  if (text_len < suffix_len) {
    return false;
  }
  return strcmp(text + text_len - suffix_len, suffix) == 0;
}

static bool br_asset_checksum_hex_valid(const char *checksum) {
  size_t i;
  if (!checksum || strlen(checksum) != 16) {
    return false;
  }
  for (i = 0; i < 16; i += 1) {
    if (!isxdigit((unsigned char)checksum[i])) {
      return false;
    }
  }
  return true;
}

static bool br_asset_json_u64_key(
  const char *payload,
  const br_json_token *tokens,
  int count,
  const char *key,
  unsigned long long *value
) {
  int idx;
  char raw[64];
  char *end = NULL;
  unsigned long long parsed;

  if (!value) {
    return false;
  }
  idx = br_json_find_key(payload, tokens, count, 0, key);
  if (idx < 0) {
    return false;
  }
  raw[0] = '\0';
  if (tokens[idx].type == BR_JSON_STRING) {
    if (!br_json_token_to_string(payload, &tokens[idx], raw, sizeof(raw))) {
      return false;
    }
  } else if (!br_json_copy_raw(payload, &tokens[idx], raw, sizeof(raw))) {
    return false;
  }
  if (!raw[0] || raw[0] == '-') {
    return false;
  }
  errno = 0;
  parsed = strtoull(raw, &end, 10);
  if (errno != 0 || !end || *end != '\0') {
    return false;
  }
  *value = parsed;
  return true;
}

static int br_asset_decode_b64_file(const char *b64_path, const char *outpath) {
  static unsigned char decoded[65536];
  int fd_in;
  int fd_out;
  int decoded_len;
  struct stat b64_st;
  char *filebuf;
  ssize_t nread;
  size_t total_read;
  char *line_start;
  char *p;

  if (!b64_path || !outpath) {
    return -1;
  }
  fd_in = open(b64_path, O_RDONLY);
  if (fd_in < 0) {
    return -1;
  }
  if (fstat(fd_in, &b64_st) != 0 || b64_st.st_size == 0) {
    close(fd_in);
    return -1;
  }
  filebuf = (char *)malloc((size_t)b64_st.st_size + 1);
  if (!filebuf) {
    close(fd_in);
    return -1;
  }

  total_read = 0;
  while (total_read < (size_t)b64_st.st_size) {
    nread = read(fd_in, filebuf + total_read, (size_t)b64_st.st_size - total_read);
    if (nread < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(filebuf);
      close(fd_in);
      return -1;
    }
    if (nread == 0) {
      break;
    }
    total_read += (size_t)nread;
  }
  close(fd_in);
  if (total_read != (size_t)b64_st.st_size) {
    free(filebuf);
    return -1;
  }
  filebuf[total_read] = '\0';

  if (br_asset_ensure_parent(outpath) != 0) {
    free(filebuf);
    return -1;
  }
  fd_out = open(outpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd_out < 0) {
    free(filebuf);
    return -1;
  }

  line_start = filebuf;
  for (p = filebuf; *p; p++) {
    if (*p != '\n') {
      continue;
    }
    *p = '\0';
    if (p > line_start) {
      decoded_len = br_asset_base64_decode(line_start, decoded, sizeof(decoded));
      if (decoded_len < 0 ||
          br_asset_write_all(fd_out, (const char *)decoded, (size_t)decoded_len) != 0) {
        free(filebuf);
        close(fd_out);
        unlink(outpath);
        return -1;
      }
    }
    line_start = p + 1;
  }
  if (p > line_start) {
    decoded_len = br_asset_base64_decode(line_start, decoded, sizeof(decoded));
    if (decoded_len < 0 ||
        br_asset_write_all(fd_out, (const char *)decoded, (size_t)decoded_len) != 0) {
      free(filebuf);
      close(fd_out);
      unlink(outpath);
      return -1;
    }
  }

  free(filebuf);
  close(fd_out);
  unlink(b64_path);
  return 0;
}

static int br_asset_file_stats_checksum(
  const char *path,
  unsigned long long *size_out,
  char checksum_hex[17]
) {
  unsigned char buffer[8192];
  unsigned long long checksum = BR_FNV1A64_OFFSET;
  unsigned long long total = 0;
  int fd;
  ssize_t nread;

  if (!path || !checksum_hex) {
    return -1;
  }
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  while (true) {
    nread = read(fd, buffer, sizeof(buffer));
    if (nread < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      return -1;
    }
    if (nread == 0) {
      break;
    }
    checksum = br_fnv1a64_update(checksum, buffer, (size_t)nread);
    total += (unsigned long long)nread;
  }
  close(fd);
  br_fnv1a64_hex(checksum, checksum_hex, 17);
  if (size_out) {
    *size_out = total;
  }
  return 0;
}

static int br_asset_tree_stats_walk(
  const char *dir_path,
  unsigned long long *file_count,
  unsigned long long *byte_count,
  bool *has_b64
) {
  DIR *dir;
  struct dirent *entry;
  char subpath[BR_MAX_PATH];

  dir = opendir(dir_path);
  if (!dir) {
    return -1;
  }

  while ((entry = readdir(dir)) != NULL) {
    struct stat st;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, entry->d_name);
    if (lstat(subpath, &st) != 0) {
      closedir(dir);
      return -1;
    }
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
      if (br_asset_tree_stats_walk(subpath, file_count, byte_count, has_b64) != 0) {
        closedir(dir);
        return -1;
      }
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      continue;
    }
    if (br_asset_string_ends_with(entry->d_name, ".b64")) {
      if (has_b64) {
        *has_b64 = true;
      }
      continue;
    }
    if (file_count) {
      *file_count += 1;
    }
    if (byte_count) {
      *byte_count += (unsigned long long)st.st_size;
    }
  }
  closedir(dir);
  return 0;
}

static int br_asset_tree_stats(
  const char *dir_path,
  unsigned long long *file_count,
  unsigned long long *byte_count,
  bool *has_b64
) {
  if (file_count) {
    *file_count = 0;
  }
  if (byte_count) {
    *byte_count = 0;
  }
  if (has_b64) {
    *has_b64 = false;
  }
  return br_asset_tree_stats_walk(dir_path, file_count, byte_count, has_b64);
}

/* Decode all .b64 files under a directory into their final binary form.
   E.g. staging/videos/foo.mp4.b64 -> staging/videos/foo.mp4 */
static int br_asset_decode_b64_files(const char *dir_path) {
  DIR *dir;
  struct dirent *entry;
  char subpath[BR_MAX_PATH], outpath[BR_MAX_PATH];

  dir = opendir(dir_path);
  if (!dir) return -1;

  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.') continue;
    snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, entry->d_name);

    /* Recurse into subdirectories */
    struct stat st;
    if (stat(subpath, &st) == 0 && S_ISDIR(st.st_mode)) {
      if (br_asset_decode_b64_files(subpath) != 0) {
        closedir(dir);
        return -1;
      }
      continue;
    }

    size_t namelen = strlen(entry->d_name);
    if (namelen < 5 || strcmp(entry->d_name + namelen - 4, ".b64") != 0) continue;
    snprintf(outpath, sizeof(outpath), "%s/%.*s", dir_path, (int)(namelen - 4), entry->d_name);

    if (br_asset_decode_b64_file(subpath, outpath) != 0) {
      closedir(dir);
      return -1;
    }
  }
  closedir(dir);
  return 0;
}

static void br_handle_asset_file_commit(br_server_state *server, const char *payload) {
  br_json_token tokens[32];
  char tid[128], rel[BR_MAX_PATH], expected_checksum[32], actual_checksum[17];
  char staging[BR_MAX_PATH], b64_path[BR_MAX_PATH], out_path[BR_MAX_PATH];
  unsigned long long expected_size = 0;
  unsigned long long actual_size = 0;
  unsigned long long chunk_count = 0;
  int count;
  int ti, pi, ci;

  count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) {
    br_asset_send_ack(server, "", "file", false, "bad json");
    return;
  }

  ti = br_json_find_key(payload, tokens, count, 0, "transferId");
  pi = br_json_find_key(payload, tokens, count, 0, "path");
  ci = br_json_find_key(payload, tokens, count, 0, "checksum");
  tid[0] = rel[0] = expected_checksum[0] = '\0';
  if (ti >= 0) br_json_token_to_string(payload, &tokens[ti], tid, sizeof(tid));
  if (pi >= 0) br_json_token_to_string(payload, &tokens[pi], rel, sizeof(rel));
  if (ci >= 0) br_json_token_to_string(payload, &tokens[ci], expected_checksum, sizeof(expected_checksum));

  if (!tid[0]) {
    br_asset_send_ack_ex(server, "", "file", false, "missing transferId", rel, false, 0, NULL);
    return;
  }
  if (!br_asset_safe_path(rel)) {
    br_asset_send_ack_ex(server, tid, "file", false, "invalid path", rel, false, 0, NULL);
    return;
  }
  if (!br_asset_json_u64_key(payload, tokens, count, "size", &expected_size)) {
    br_asset_send_ack_ex(server, tid, "file", false, "missing size", rel, false, 0, NULL);
    return;
  }
  (void)br_asset_json_u64_key(payload, tokens, count, "chunkCount", &chunk_count);
  if (!br_asset_checksum_hex_valid(expected_checksum)) {
    br_asset_send_ack_ex(server, tid, "file", false, "invalid checksum", rel, false, 0, NULL);
    return;
  }

  if (snprintf(staging, sizeof(staging), "%s/.incoming-desktop-pet", server->config.root_dir) >= (int)sizeof(staging) ||
      snprintf(b64_path, sizeof(b64_path), "%s/%s.b64", staging, rel) >= (int)sizeof(b64_path) ||
      snprintf(out_path, sizeof(out_path), "%s/%s", staging, rel) >= (int)sizeof(out_path)) {
    br_asset_send_ack_ex(server, tid, "file", false, "path too long", rel, false, 0, NULL);
    return;
  }
  if (access(b64_path, R_OK) != 0) {
    br_asset_send_ack_ex(server, tid, "file", false, "file chunks missing", rel, false, 0, NULL);
    return;
  }
  if (br_asset_decode_b64_file(b64_path, out_path) != 0) {
    br_asset_send_ack_ex(server, tid, "file", false, "file decode failed", rel, false, 0, NULL);
    return;
  }
  if (br_asset_file_stats_checksum(out_path, &actual_size, actual_checksum) != 0) {
    br_asset_send_ack_ex(server, tid, "file", false, "file stat failed", rel, false, 0, NULL);
    return;
  }
  if (actual_size != expected_size || strcmp(actual_checksum, expected_checksum) != 0) {
    unlink(out_path);
    br_asset_send_ack_ex(
      server,
      tid,
      "file",
      false,
      "file checksum mismatch",
      rel,
      true,
      actual_size,
      actual_checksum
    );
    return;
  }

  br_server_logf(
    "asset_file: %s path=%s size=%llu checksum=%s chunks=%llu",
    tid,
    rel,
    actual_size,
    actual_checksum,
    chunk_count
  );
  br_asset_send_ack_ex(server, tid, "file", true, "", rel, true, actual_size, actual_checksum);
}

static void br_handle_asset_commit(br_server_state *server, const char *payload) {
  br_json_token tokens[32]; char tid[128];
  char staging[BR_MAX_PATH], current[BR_MAX_PATH], previous[BR_MAX_PATH];
  char clips[BR_MAX_PATH], clips_prev[BR_MAX_PATH], cur_videos[BR_MAX_PATH];
  char marker[128];
  unsigned long long expected_file_count = 0;
  unsigned long long expected_total_bytes = 0;
  unsigned long long staged_file_count = 0;
  unsigned long long staged_total_bytes = 0;
  bool has_b64 = false;
  bool has_transaction_totals = false;

  int count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) { br_asset_send_ack(server, "", "commit", false, "bad json"); return; }
  int idx = br_json_find_key(payload, tokens, count, 0, "transferId");
  tid[0] = '\0';
  if (idx >= 0) br_json_token_to_string(payload, &tokens[idx], tid, sizeof(tid));
  if (!tid[0]) { br_asset_send_ack(server, "", "commit", false, "missing transferId"); return; }
  if (br_json_find_key(payload, tokens, count, 0, "fileCount") >= 0 ||
      br_json_find_key(payload, tokens, count, 0, "totalBytes") >= 0) {
    has_transaction_totals = true;
    if (!br_asset_json_u64_key(payload, tokens, count, "fileCount", &expected_file_count) ||
        !br_asset_json_u64_key(payload, tokens, count, "totalBytes", &expected_total_bytes)) {
      br_asset_send_ack(server, tid, "commit", false, "missing transfer totals"); return;
    }
  }

  snprintf(staging, sizeof(staging), "%s/.incoming-desktop-pet", server->config.root_dir);
  snprintf(current, sizeof(current), "%s/.desktop-pet-current", server->config.root_dir);
  snprintf(previous, sizeof(previous), "%s/.desktop-pet-previous", server->config.root_dir);
  snprintf(clips, sizeof(clips), "%s/terrier-clips", server->config.root_dir);
  snprintf(clips_prev, sizeof(clips_prev), "%s/terrier-clips.previous", server->config.root_dir);
  snprintf(cur_videos, sizeof(cur_videos), "%s/videos", current);

  if (access(staging, R_OK) != 0) {
    br_asset_send_ack(server, tid, "commit", false, "staging missing"); return;
  }

  if (has_transaction_totals) {
    if (br_asset_tree_stats(staging, &staged_file_count, &staged_total_bytes, &has_b64) != 0) {
      br_asset_send_ack(server, tid, "commit", false, "staging scan failed"); return;
    }
    if (has_b64) {
      br_asset_send_ack(server, tid, "commit", false, "uncommitted file chunks"); return;
    }
    if (staged_file_count != expected_file_count || staged_total_bytes != expected_total_bytes) {
      br_asset_send_ack(server, tid, "commit", false, "staging totals mismatch"); return;
    }
  } else {
    /* Legacy compatibility: old clients sent only begin/chunk/commit. */
    br_server_logf("asset_commit: decoding b64 files in staging...");
    if (br_asset_decode_b64_files(staging) != 0) {
      br_asset_send_ack(server, tid, "commit", false, "b64 decode failed"); return;
    }
  }
  (void)br_asset_remove_tree(previous);
  if (access(current, F_OK) == 0 && rename(current, previous) != 0) {
    br_asset_send_ack(server, tid, "commit", false, "rotate failed"); return;
  }
  if (rename(staging, current) != 0) {
    br_asset_send_ack(server, tid, "commit", false, "activate failed"); return;
  }
  (void)br_asset_remove_tree(clips_prev);
  if (access(clips, F_OK) == 0) (void)rename(clips, clips_prev);
  if (symlink(cur_videos, clips) != 0) {
    br_asset_send_ack(server, tid, "commit", false, "symlink failed"); return;
  }

  /* Trigger fb-display.sh to reload clips */
  snprintf(marker, sizeof(marker), "%lld assets\n", (long long)br_now_ms());
  {
    char clips_reload[BR_MAX_PATH];
    snprintf(clips_reload, sizeof(clips_reload), "%s/.clips-reload", server->config.root_dir);
    br_atomic_write_text(clips_reload, marker);
  }
  br_atomic_write_text(server->config.screen_interrupt_path, marker);

  br_server_logf("asset_commit: %s — clips symlinked, display reloading", tid);
  br_asset_send_ack(server, tid, "commit", true, "");
}

static int br_asset_patch_audio_tree(const char *staging_root, const char *current_root, const char *dir_path) {
  DIR *dir = opendir(dir_path);
  struct dirent *entry;
  size_t root_len = strlen(staging_root);
  if (!dir) return -1;
  while ((entry = readdir(dir)) != NULL) {
    char subpath[BR_MAX_PATH], dst_path[BR_MAX_PATH];
    struct stat st;
    const char *rel;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    if (snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, entry->d_name) >= (int)sizeof(subpath)) {
      closedir(dir);
      return -1;
    }
    if (lstat(subpath, &st) != 0) {
      closedir(dir);
      return -1;
    }
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
      if (br_asset_patch_audio_tree(staging_root, current_root, subpath) != 0) {
        closedir(dir);
        return -1;
      }
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      closedir(dir);
      return -1;
    }
    if (strlen(subpath) <= root_len || subpath[root_len] != '/') {
      closedir(dir);
      return -1;
    }
    rel = subpath + root_len + 1;
    if (!br_asset_is_audio_patch_path(rel)) {
      closedir(dir);
      return -1;
    }
    if (snprintf(dst_path, sizeof(dst_path), "%s/%s", current_root, rel) >= (int)sizeof(dst_path)) {
      closedir(dir);
      return -1;
    }
    if (br_asset_copy_file_atomic(subpath, dst_path) != 0) {
      closedir(dir);
      return -1;
    }
  }
  closedir(dir);
  return 0;
}

static void br_handle_asset_patch_commit(br_server_state *server, const char *payload) {
  br_json_token tokens[32];
  char tid[128], staging[BR_MAX_PATH], current[BR_MAX_PATH], marker[128];
  unsigned long long expected_file_count = 0, expected_total_bytes = 0;
  unsigned long long staged_file_count = 0, staged_total_bytes = 0;
  bool has_b64 = false;
  int count, idx;

  count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) { br_asset_send_ack(server, "", "patch", false, "bad json"); return; }
  idx = br_json_find_key(payload, tokens, count, 0, "transferId");
  tid[0] = '\0';
  if (idx >= 0) br_json_token_to_string(payload, &tokens[idx], tid, sizeof(tid));
  if (!tid[0]) { br_asset_send_ack(server, "", "patch", false, "missing transferId"); return; }
  if (!br_asset_json_u64_key(payload, tokens, count, "fileCount", &expected_file_count) ||
      !br_asset_json_u64_key(payload, tokens, count, "totalBytes", &expected_total_bytes)) {
    br_asset_send_ack(server, tid, "patch", false, "missing patch totals"); return;
  }

  snprintf(staging, sizeof(staging), "%s/.incoming-desktop-pet", server->config.root_dir);
  snprintf(current, sizeof(current), "%s/.desktop-pet-current", server->config.root_dir);
  if (access(staging, R_OK) != 0) {
    br_asset_send_ack(server, tid, "patch", false, "staging missing"); return;
  }
  if (access(current, R_OK) != 0) {
    br_asset_send_ack(server, tid, "patch", false, "current appearance missing; full sync required"); return;
  }
  if (br_asset_tree_stats(staging, &staged_file_count, &staged_total_bytes, &has_b64) != 0) {
    br_asset_send_ack(server, tid, "patch", false, "staging scan failed"); return;
  }
  if (has_b64) {
    br_asset_send_ack(server, tid, "patch", false, "uncommitted file chunks"); return;
  }
  if (staged_file_count != expected_file_count || staged_total_bytes != expected_total_bytes) {
    br_asset_send_ack(server, tid, "patch", false, "staging totals mismatch"); return;
  }
  if (br_asset_patch_audio_tree(staging, current, staging) != 0) {
    br_asset_send_ack(server, tid, "patch", false, "audio patch failed"); return;
  }
  (void)br_asset_remove_tree(staging);

  snprintf(marker, sizeof(marker), "%lld audio-patch\n", (long long)br_now_ms());
  {
    char clips_reload[BR_MAX_PATH];
    snprintf(clips_reload, sizeof(clips_reload), "%s/.clips-reload", server->config.root_dir);
    br_atomic_write_text(clips_reload, marker);
  }
  br_atomic_write_text(server->config.screen_interrupt_path, marker);

  br_server_logf("asset_patch_commit: %s — audio cues patched", tid);
  br_asset_send_ack(server, tid, "patch", true, "");
}

/* --- Async wifi apply: worker thread polls last-attempt.json and emits acks --- */

/* Forward declarations for Task 3 helpers defined later in this TU. */
static void br_server_resolve_ap_state_dir(char *out, size_t out_size);
static bool br_server_spawn_sta_apply(br_server_state *server,
                                       const char *ssid,
                                       const char *password);

static pthread_mutex_t br_apply_wifi_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool br_apply_wifi_in_progress = false;

#define BR_APPLY_WIFI_MAX_POLLS 60   /* 60 * 500ms = 30s, leaves a few seconds buffer past the 25s sta-apply timeout */
#define BR_APPLY_WIFI_POLL_INTERVAL_US 500000

typedef struct {
  br_server_state *server;
} br_apply_wifi_ctx;

static void br_apply_wifi_send_ack(br_server_state *server,
                                    const char *stage,
                                    bool ok,
                                    const char *ip,
                                    const char *error) {
  char payload[512];
  size_t used = 0;
  payload[0] = '\0';
  br_snprintf_append(payload, sizeof(payload), &used, "{\"ok\":%s,\"stage\":\"",
                     ok ? "true" : "false");
  br_json_escape_append(payload, sizeof(payload), &used, stage ? stage : "");
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"ip\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, ip ? ip : "");
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"error\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, error ? error : "");
  br_snprintf_append(payload, sizeof(payload), &used, "\"}");
  (void) br_usb_serial_send(&server->usb_serial, "apply-wifi-ack", payload);
}

static void *br_apply_wifi_worker(void *arg) {
  br_apply_wifi_ctx *ctx = (br_apply_wifi_ctx *) arg;
  br_server_state *server = ctx->server;
  free(ctx);

  char dir[BR_MAX_PATH];
  char last_path[BR_MAX_PATH];
  br_server_resolve_ap_state_dir(dir, sizeof(dir));
  snprintf(last_path, sizeof(last_path), "%s/last-attempt.json", dir);

  /* First ack: "applying". The script writes its own pending stamp into
   * last-attempt.json before kicking off; we don't need to wait for it. */
  br_apply_wifi_send_ack(server, "applying", false, "", "");

  for (int poll = 0; poll < BR_APPLY_WIFI_MAX_POLLS; poll += 1) {
    usleep(BR_APPLY_WIFI_POLL_INTERVAL_US);

    char buffer[1024];
    if (!br_read_text_file(last_path, buffer, sizeof(buffer)) || buffer[0] == '\0') {
      continue;
    }

    char error_value[64];
    char ip_value[64];
    br_json_token tokens[64];
    int count = br_json_parse(buffer, strlen(buffer), tokens,
                              (int) (sizeof(tokens) / sizeof(tokens[0])));
    if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) {
      continue;
    }
    error_value[0] = '\0';
    ip_value[0] = '\0';
    br_server_extract_string_key(buffer, tokens, count, "error", error_value, sizeof(error_value));
    br_server_extract_string_key(buffer, tokens, count, "ip", ip_value, sizeof(ip_value));
    /* `ok` is a JSON bool; use the existing find-key + manual token dispatch. */
    int ok_idx = br_json_find_key(buffer, tokens, count, 0, "ok");
    bool ok_flag = false;
    if (ok_idx >= 0 && tokens[ok_idx].type == BR_JSON_PRIMITIVE) {
      char tmp[16];
      if (br_json_token_to_string(buffer, &tokens[ok_idx], tmp, sizeof(tmp))) {
        ok_flag = (strcmp(tmp, "true") == 0);
      }
    }

    if (strcmp(error_value, "pending") == 0) {
      /* still running; keep polling */
      continue;
    }

    /* Terminal state */
    if (ok_flag) {
      br_apply_wifi_send_ack(server, "connected", true, ip_value, "");
    } else {
      br_apply_wifi_send_ack(server, "failed", false, "", error_value);
    }
    goto done;
  }

  /* Polling exhausted without a terminal state. */
  br_apply_wifi_send_ack(server, "failed", false, "", "timeout");

done:
  pthread_mutex_lock(&br_apply_wifi_mutex);
  br_apply_wifi_in_progress = false;
  pthread_mutex_unlock(&br_apply_wifi_mutex);
  return NULL;
}

static void br_handle_usb_apply_wifi(br_server_state *server, const char *payload) {
  char ssid[128];
  char password[128];
  char mqtt_url[256];
  char mqtt_namespace[64];
  char desktop_device_id[128];

  if (!br_server_parse_network_config_json(payload,
                                           ssid, sizeof(ssid),
                                           password, sizeof(password),
                                           mqtt_url, sizeof(mqtt_url),
                                           mqtt_namespace, sizeof(mqtt_namespace),
                                           desktop_device_id, sizeof(desktop_device_id))) {
    br_apply_wifi_send_ack(server, "failed", false, "", "invalid_json");
    return;
  }
  if (!br_wifi_credential_valid(ssid, 64, false)) {
    br_apply_wifi_send_ack(server, "failed", false, "", "invalid_ssid");
    return;
  }
  if (!br_wifi_credential_valid(password, 64, true)) {
    br_apply_wifi_send_ack(server, "failed", false, "", "invalid_psk");
    return;
  }
  if (server->config.sta_apply_cmd[0] == '\0') {
    br_apply_wifi_send_ack(server, "failed", false, "", "sta_apply_unconfigured");
    return;
  }

  pthread_mutex_lock(&br_apply_wifi_mutex);
  if (br_apply_wifi_in_progress) {
    pthread_mutex_unlock(&br_apply_wifi_mutex);
    br_apply_wifi_send_ack(server, "failed", false, "", "already_in_progress");
    return;
  }
  br_apply_wifi_in_progress = true;
  pthread_mutex_unlock(&br_apply_wifi_mutex);

  /* Persist creds + spawn board-sta-apply.sh asynchronously. The worker
   * thread below will poll last-attempt.json and emit stage acks. */
  if (!br_server_spawn_sta_apply(server, ssid, password)) {
    pthread_mutex_lock(&br_apply_wifi_mutex);
    br_apply_wifi_in_progress = false;
    pthread_mutex_unlock(&br_apply_wifi_mutex);
    br_apply_wifi_send_ack(server, "failed", false, "", "spawn_failed");
    return;
  }

  br_apply_wifi_ctx *ctx = (br_apply_wifi_ctx *) calloc(1, sizeof(*ctx));
  if (!ctx) {
    pthread_mutex_lock(&br_apply_wifi_mutex);
    br_apply_wifi_in_progress = false;
    pthread_mutex_unlock(&br_apply_wifi_mutex);
    br_apply_wifi_send_ack(server, "failed", false, "", "oom");
    return;
  }
  ctx->server = server;
  pthread_t thread_id;
  if (pthread_create(&thread_id, NULL, br_apply_wifi_worker, ctx) != 0) {
    free(ctx);
    pthread_mutex_lock(&br_apply_wifi_mutex);
    br_apply_wifi_in_progress = false;
    pthread_mutex_unlock(&br_apply_wifi_mutex);
    br_apply_wifi_send_ack(server, "failed", false, "", "thread_failed");
    return;
  }
  (void) pthread_detach(thread_id);
}

/* --- USB serial message handler: maps virtual topics to full topics --- */

static void br_handle_usb_message(const char *virtual_topic, const char *payload, void *userdata) {
  br_server_state *server = (br_server_state *) userdata;
  char full_topic[BR_MAX_TOPIC];

  if (!server || !virtual_topic || !payload) {
    return;
  }

  if (strcmp(virtual_topic, "ack") == 0) {
    /* Mark first ack only to avoid log spam; do NOT reply with hello — that
     * would form an ack/hello feedback loop with the desktop client (which
     * acks every hello it receives). The 30 s hello heartbeat below already
     * lets a freshly-attached host learn boardDeviceId without a reply. */
    if (!server->usb_serial.peer_acked) {
      server->usb_serial.peer_acked = true;
      br_server_logf("USB peer acknowledged");
    }
    return;
  }

  if (strncmp(virtual_topic, "state/", 6) == 0) {
    snprintf(full_topic, sizeof(full_topic), "%s/%s/%s",
             server->config.mqtt_namespace,
             server->config.target_device_id,
             virtual_topic);
  } else if (strcmp(virtual_topic, "speech/text") == 0) {
    snprintf(full_topic, sizeof(full_topic), "%s", server->current_speech_topic);
  } else if (strcmp(virtual_topic, "control/command") == 0) {
    snprintf(full_topic, sizeof(full_topic), "%s", server->command_topic);
  } else if (strcmp(virtual_topic, "control/remote-cli-binding") == 0) {
    snprintf(full_topic, sizeof(full_topic), "%s", server->control_topic);
  } else if (strcmp(virtual_topic, "control/screen-page") == 0) {
    snprintf(full_topic, sizeof(full_topic), "%s", server->screen_page_topic);
  } else if (strcmp(virtual_topic, "control/apply-wifi") == 0) {
    br_handle_usb_apply_wifi(server, payload);
    return;
  } else if (strcmp(virtual_topic, "asset/begin") == 0) {
    br_handle_asset_begin(server, payload);
    return;
  } else if (strcmp(virtual_topic, "asset/stat") == 0) {
    br_handle_asset_stat(server, payload);
    return;
  } else if (strcmp(virtual_topic, "asset/chunk") == 0) {
    br_handle_asset_chunk(server, payload);
    return;
  } else if (strcmp(virtual_topic, "asset/file") == 0) {
    br_handle_asset_file_commit(server, payload);
    return;
  } else if (strcmp(virtual_topic, "asset/commit") == 0) {
    br_handle_asset_commit(server, payload);
    return;
  } else if (strcmp(virtual_topic, "asset/patch-commit") == 0) {
    br_handle_asset_patch_commit(server, payload);
    return;
  } else if (strcmp(virtual_topic, "widget/begin") == 0) {
    br_handle_widget_install_begin(server, payload);
    return;
  } else if (strcmp(virtual_topic, "widget/chunk") == 0) {
    br_handle_widget_install_chunk(server, payload);
    return;
  } else if (strcmp(virtual_topic, "widget/commit") == 0) {
    br_handle_widget_install_commit(server, payload);
    return;
  } else {
    br_server_logf("USB: unknown virtual topic: %s", virtual_topic);
    return;
  }

  br_handle_mqtt_publish(full_topic, payload, userdata);
}

/* --- USB touch action file polling --- */

static void br_server_poll_usb_touch(br_server_state *server) {
  struct stat st;
  char action_json[BR_MAX_JSON];

  if (server->usb_touch_action_path[0] == '\0') {
    return;
  }

  if (stat(server->usb_touch_action_path, &st) != 0) {
    return;
  }

  if (br_read_text_file(server->usb_touch_action_path, action_json, sizeof(action_json))) {
    unlink(server->usb_touch_action_path);
    if (action_json[0] != '\0') {
      br_server_publish(server, server->input_action_topic, "input/action", action_json, false);
    }
  }
}

static int br_server_open_listener(br_server_state *server) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  struct sockaddr_in address;
  if (fd < 0) {
    return -1;
  }
  (void) fcntl(fd, F_SETFD, FD_CLOEXEC);
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t) server->config.http_port);
  address.sin_addr.s_addr = strcmp(server->config.http_host, "0.0.0.0") == 0
    ? htonl(INADDR_ANY)
    : inet_addr(server->config.http_host);
  if (bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 16) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int br_send_all(int fd, const char *data, size_t length) {
  while (length > 0) {
    ssize_t written = send(fd, data, length, 0);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    data += (size_t) written;
    length -= (size_t) written;
  }
  return 0;
}

static int br_send_json_response(int fd, int status_code, const char *payload) {
  char header[512];
  size_t body_length = strlen(payload);
  const char *status_text = status_code == 200 ? "OK" :
                            status_code == 400 ? "Bad Request" :
                            status_code == 403 ? "Forbidden" :
                            status_code == 404 ? "Not Found" :
                            status_code == 405 ? "Method Not Allowed" :
                            status_code == 413 ? "Payload Too Large" :
                            status_code == 500 ? "Internal Server Error" :
                            status_code == 503 ? "Service Unavailable" : "OK";
  int header_size = snprintf(header, sizeof(header),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: application/json; charset=utf-8\r\n"
                             "Content-Length: %zu\r\n"
                             "Cache-Control: no-cache\r\n"
                             "Connection: close\r\n\r\n",
                             status_code,
                             status_text,
                             body_length);
  if (header_size < 0 || (size_t) header_size >= sizeof(header)) {
    return -1;
  }
  if (br_send_all(fd, header, (size_t) header_size) != 0) {
    return -1;
  }
  return br_send_all(fd, payload, body_length);
}

static int br_send_text_response(int fd, int status_code, const char *content_type, const char *payload) {
  char header[512];
  size_t body_length = strlen(payload);
  const char *status_text = status_code == 200 ? "OK" :
                            status_code == 404 ? "Not Found" :
                            status_code == 403 ? "Forbidden" : "OK";
  int header_size = snprintf(header, sizeof(header),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %zu\r\n"
                             "Cache-Control: no-cache\r\n"
                             "Connection: close\r\n\r\n",
                             status_code,
                             status_text,
                             content_type,
                             body_length);
  if (header_size < 0 || (size_t) header_size >= sizeof(header)) {
    return -1;
  }
  if (br_send_all(fd, header, (size_t) header_size) != 0) {
    return -1;
  }
  return br_send_all(fd, payload, body_length);
}

static int br_serve_file(int fd, const char *path) {
  FILE *file;
  struct stat st;
  char header[512];
  char buffer[4096];
  size_t read_size;
  const char *content_type;
  const char *cache_control;

  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    return br_send_text_response(fd, 404, "text/plain; charset=utf-8", "not found");
  }
  file = fopen(path, "rb");
  if (!file) {
    return br_send_text_response(fd, 404, "text/plain; charset=utf-8", "not found");
  }
  content_type = br_content_type(path);
  cache_control = br_topic_ends_with(path, ".mp4") ? "public, max-age=31536000" : "no-cache";
  int header_size = snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %lld\r\n"
                             "Cache-Control: %s\r\n"
                             "Connection: close\r\n\r\n",
                             content_type,
                             (long long) st.st_size,
                             cache_control);
  if (header_size < 0 || (size_t) header_size >= sizeof(header)) {
    fclose(file);
    return -1;
  }
  if (br_send_all(fd, header, (size_t) header_size) != 0) {
    fclose(file);
    return -1;
  }
  while ((read_size = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (br_send_all(fd, buffer, read_size) != 0) {
      fclose(file);
      return -1;
    }
  }
  fclose(file);
  return 0;
}

static void br_build_config_json(br_server_state *server, const char *request_host, char *output, size_t output_size) {
  char broker_url[256];
  char host[128];
  size_t used = 0;
  const char *effective_host = request_host && *request_host ? request_host : "127.0.0.1";
  const char *pairing_state = br_pairing_state_name(server->pairing.state);
  const char *pairing_mode = br_pairing_mode_name(server->pairing.state);

  if (server->pairing.state == BR_PAIRING_AP_FALLBACK && server->config.ap_ip[0] != '\0') {
    effective_host = server->config.ap_ip;
  }
  if (server->mqtt.host[0] != '\0' && server->mqtt.port > 0) {
    snprintf(broker_url, sizeof(broker_url), "mqtt://%s:%d", server->mqtt.host, server->mqtt.port);
  } else {
    br_mqtt_endpoint endpoint;
    if (br_parse_mqtt_url(server->config.mqtt_url, &endpoint)) {
      snprintf(broker_url, sizeof(broker_url), "mqtt://%s:%d", endpoint.host, endpoint.port);
    } else {
      snprintf(broker_url, sizeof(broker_url), "%s", server->config.mqtt_url);
    }
  }
  if (server->pairing.state == BR_PAIRING_AP_FALLBACK && server->config.ap_ip[0] != '\0') {
    br_normalize_text(server->config.ap_ip, "192.168.44.1", host, sizeof(host));
  } else if (server->config.public_host[0]) {
    br_normalize_text(server->config.public_host, "127.0.0.1", host, sizeof(host));
  } else {
    br_normalize_text(effective_host, "127.0.0.1", host, sizeof(host));
  }

  output[0] = '\0';
  br_snprintf_append(output, output_size, &used, "{");
  br_snprintf_append(output, output_size, &used, "\"websocketHost\":\"");
  br_json_escape_append(output, output_size, &used, effective_host);
  br_snprintf_append(output, output_size, &used, "\",\"websocketPort\":%d", server->config.http_port);
  br_snprintf_append(output, output_size, &used, ",\"pairingState\":\"");
  br_json_escape_append(output, output_size, &used, pairing_state);
  br_snprintf_append(output, output_size, &used, "\",\"pairingMode\":\"");
  br_json_escape_append(output, output_size, &used, pairing_mode);
  br_snprintf_append(output, output_size, &used, "\",\"discoveryUdpPort\":%d", server->config.discovery_udp_port);
  br_snprintf_append(output, output_size, &used, ",\"discoveryMdnsPort\":%d", server->config.discovery_mdns_port);
  br_snprintf_append(output, output_size, &used, ",\"apIp\":\"");
  br_json_escape_append(output, output_size, &used, server->config.ap_ip);
  br_snprintf_append(output, output_size, &used, "\",\"apSsid\":\"");
  br_json_escape_append(output, output_size, &used, server->config.ap_ssid);
  br_snprintf_append(output, output_size, &used, "\",\"apPsk\":\"");
  br_json_escape_append(output, output_size, &used, server->config.ap_psk);
  br_snprintf_append(output, output_size, &used, "\",\"pairingHint\":\"");
  br_json_escape_append(output, output_size, &used, server->pairing_message);
  br_snprintf_append(output, output_size, &used, "\"");
  br_snprintf_append(output, output_size, &used, ",\"mqttNamespace\":\"");
  br_json_escape_append(output, output_size, &used, server->config.mqtt_namespace);
  br_snprintf_append(output, output_size, &used, "\",\"localDeviceId\":\"");
  br_json_escape_append(output, output_size, &used, server->config.local_device_id);
  br_snprintf_append(output, output_size, &used, "\",\"boardDeviceId\":\"");
  br_json_escape_append(output, output_size, &used, server->config.board_device_id);
  br_snprintf_append(output, output_size, &used, "\",\"screenName\":\"");
  br_json_escape_append(output, output_size, &used, server->config.screen_name);
  br_snprintf_append(output, output_size, &used, "\",\"host\":\"");
  br_json_escape_append(output, output_size, &used, host);
  br_snprintf_append(output, output_size, &used, "\",\"brokerUrl\":\"");
  br_json_escape_append(output, output_size, &used, broker_url);
  br_snprintf_append(output, output_size, &used, "\",\"inputActionTopic\":\"");
  br_json_escape_append(output, output_size, &used, server->input_action_topic);
  br_snprintf_append(output, output_size, &used, "\",\"helloTopic\":\"");
  br_json_escape_append(output, output_size, &used, server->hello_topic);
  br_snprintf_append(output, output_size, &used, "\",\"availabilityTopic\":\"");
  br_json_escape_append(output, output_size, &used, server->availability_topic);
  br_snprintf_append(output, output_size, &used, "\",\"desktopDeviceId\":\"");
  br_json_escape_append(output, output_size, &used, server->config.target_device_id);
  br_snprintf_append(output, output_size, &used, "\",\"targetDeviceId\":\"");
  br_json_escape_append(output, output_size, &used, server->config.target_device_id);
  br_snprintf_append(output, output_size, &used, "\",\"targetSource\":\"");
  br_json_escape_append(output, output_size, &used, server->config.target_source);
  br_snprintf_append(output, output_size, &used, "\",\"sourceStateTopic\":\"");
  br_json_escape_append(output, output_size, &used,
                        server->config.target_source[0] ? server->current_state_topic : server->wildcard_state_topic);
  br_snprintf_append(output, output_size, &used, "\",\"sourceSpeechTopic\":\"");
  br_json_escape_append(output, output_size, &used, server->current_speech_topic);
  br_snprintf_append(output, output_size, &used, "\",\"uiUrl\":\"");
  if (server->config.public_url[0]) {
    br_json_escape_append(output, output_size, &used, server->config.public_url);
  } else {
    br_snprintf_append(output, output_size, &used, "http://%s:%d", host, server->config.http_port);
  }
  br_snprintf_append(output, output_size, &used, "\"}");
}

static bool br_extract_header_value(const char *request, const char *header_name, char *output, size_t output_size) {
  const char *cursor = request;
  size_t name_length = strlen(header_name);
  while ((cursor = strcasestr(cursor, header_name)) != NULL) {
    if ((cursor == request || cursor[-1] == '\n') && strncasecmp(cursor, header_name, name_length) == 0) {
      const char *colon = cursor + name_length;
      const char *line_end;
      if (*colon != ':') {
        cursor += name_length;
        continue;
      }
      colon += 1;
      while (*colon == ' ' || *colon == '\t') {
        colon += 1;
      }
      line_end = strstr(colon, "\r\n");
      if (!line_end) {
        return false;
      }
      size_t length = (size_t) (line_end - colon);
      if (length + 1 > output_size) {
        return false;
      }
      memcpy(output, colon, length);
      output[length] = '\0';
      return true;
    }
    cursor += name_length;
  }
  return false;
}

static bool br_server_is_ap_mode(const br_server_state *server) {
  return server && server->pairing.state == BR_PAIRING_AP_FALLBACK;
}

static void br_trim_ascii_inplace(char *text) {
  char *start;
  char *end;
  if (!text || text[0] == '\0') {
    return;
  }
  start = text;
  while (*start && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) {
    start += 1;
  }
  if (start != text) {
    memmove(text, start, strlen(start) + 1);
  }
  end = text + strlen(text);
  while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
    end -= 1;
  }
  *end = '\0';
}

static bool br_constant_time_equals(const char *left, const char *right) {
  size_t left_len;
  size_t right_len;
  size_t max_len;
  unsigned char diff = 0;
  if (!left || !right) {
    return false;
  }
  left_len = strlen(left);
  right_len = strlen(right);
  max_len = left_len > right_len ? left_len : right_len;
  diff |= (unsigned char) (left_len ^ right_len);
  for (size_t i = 0; i < max_len; i += 1) {
    unsigned char a = i < left_len ? (unsigned char) left[i] : 0U;
    unsigned char b = i < right_len ? (unsigned char) right[i] : 0U;
    diff |= (unsigned char) (a ^ b);
  }
  return diff == 0U;
}

static bool br_extract_admin_token_from_request(const char *request, char *token, size_t token_size) {
  char value[512];
  const char *cursor;
  if (!request || !token || token_size == 0) {
    return false;
  }
  token[0] = '\0';

  if (br_extract_header_value(request, "X-Board-Token", value, sizeof(value)) ||
      br_extract_header_value(request, "X-Admin-Token", value, sizeof(value))) {
    br_normalize_text(value, "", token, token_size);
    br_trim_ascii_inplace(token);
    return token[0] != '\0';
  }

  if (!br_extract_header_value(request, "Authorization", value, sizeof(value))) {
    return false;
  }
  cursor = value;
  while (*cursor == ' ' || *cursor == '\t') {
    cursor += 1;
  }
  if (strncasecmp(cursor, "Bearer ", 7) == 0) {
    cursor += 7;
  }
  while (*cursor == ' ' || *cursor == '\t') {
    cursor += 1;
  }
  br_normalize_text(cursor, "", token, token_size);
  br_trim_ascii_inplace(token);
  return token[0] != '\0';
}

static bool br_require_sensitive_api_auth(br_server_state *server, int fd, const char *request, const char *path) {
  char provided[256];
  if (!server || !request) {
    return br_send_json_response(fd, 500, "{\"ok\":false,\"error\":\"internal_error\"}") == 0;
  }
  if (br_server_is_ap_mode(server)) {
    return true;
  }
  if (server->config.admin_token[0] == '\0') {
    br_server_logf("sensitive API blocked in STA mode (token not configured): %s", path ? path : "(unknown)");
    return br_send_json_response(fd, 503, "{\"ok\":false,\"error\":\"admin_token_not_configured\"}") == 0;
  }
  if (!br_extract_admin_token_from_request(request, provided, sizeof(provided))) {
    return br_send_json_response(fd, 401, "{\"ok\":false,\"error\":\"unauthorized\"}") == 0;
  }
  if (!br_constant_time_equals(provided, server->config.admin_token)) {
    return br_send_json_response(fd, 401, "{\"ok\":false,\"error\":\"unauthorized\"}") == 0;
  }
  return true;
}

static bool br_read_http_request(int fd, char *request, size_t request_size, ssize_t *request_length, char **header_end, int *content_length) {
  ssize_t total = 0;
  bool header_complete = false;

  if (!request || request_size == 0 || !request_length || !header_end || !content_length) {
    return false;
  }

  request[0] = '\0';
  *request_length = 0;
  *header_end = NULL;
  *content_length = 0;

  while (total < (ssize_t) request_size - 1) {
    ssize_t read_size = recv(fd, request + total, request_size - 1 - (size_t) total, 0);
    if (read_size <= 0) {
      break;
    }
    total += read_size;
    request[total] = '\0';

    if (!header_complete) {
      char *content_length_header;
      *header_end = strstr(request, "\r\n\r\n");
      if (!*header_end) {
        continue;
      }
      header_complete = true;
      content_length_header = strcasestr(request, "Content-Length:");
      if (content_length_header) {
        *content_length = atoi(content_length_header + strlen("Content-Length:"));
      }
    }

    if (*header_end) {
      size_t header_size = (size_t) (*header_end + 4 - request);
      if (total >= (ssize_t) (header_size + (size_t) (*content_length))) {
        request[header_size + (size_t) (*content_length)] = '\0';
        *request_length = total;
        return true;
      }
    }
  }

  *request_length = total;
  if (*header_end) {
    size_t header_size = (size_t) (*header_end + 4 - request);
    if (total >= (ssize_t) header_size) {
      return total >= (ssize_t) (header_size + (size_t) (*content_length));
    }
  }
  return false;
}

static bool br_handle_input_action_post(br_server_state *server, int fd, const char *body) {
  br_input_action action;
  char error[64];
  char payload[BR_MAX_JSON];
  char response[BR_MAX_JSON];
  size_t used = 0;
  long long ts_ms = br_now_ms();

  if (!br_parse_input_action_json(body, &action, error, sizeof(error))) {
    if (strcmp(error, "invalid_json") == 0) {
      return br_send_json_response(fd, 400, "{\"ok\":false,\"error\":\"invalid_json\"}") == 0;
    }
    return br_send_json_response(fd, 400, "{\"ok\":false,\"error\":\"invalid_input_action\"}") == 0;
  }
  if (server->transport_mode == BR_TRANSPORT_MQTT && !server->mqtt.connected) {
    return br_send_json_response(fd, 503, "{\"ok\":false,\"error\":\"mqtt_not_connected\"}") == 0;
  }
  if (server->transport_mode == BR_TRANSPORT_USB && !server->usb_serial.connected) {
    return br_send_json_response(fd, 503, "{\"ok\":false,\"error\":\"usb_not_connected\"}") == 0;
  }
  if (br_build_input_action_payload(server->config.board_device_id,
                                    server->config.local_device_id,
                                    "board-runtime",
                                    &action,
                                    ts_ms,
                                    payload,
                                    sizeof(payload)) != 0) {
    return br_send_json_response(fd, 500, "{\"ok\":false,\"error\":\"publish_failed\"}") == 0;
  }
  if (br_server_publish(server, server->input_action_topic, "input/action", payload, false) != 0) {
    return br_send_json_response(fd, 500, "{\"ok\":false,\"error\":\"publish_failed\"}") == 0;
  }

  response[0] = '\0';
  br_snprintf_append(response, sizeof(response), &used, "{\"ok\":true,\"topic\":\"");
  br_json_escape_append(response, sizeof(response), &used, server->input_action_topic);
  br_snprintf_append(response, sizeof(response), &used, "\",\"action\":%s}", payload);
  return br_send_json_response(fd, 200, response) == 0;
}

static bool br_server_write_network_config_file(
  br_server_state *server,
  const char *ssid,
  const char *password,
  const char *mqtt_url,
  const char *mqtt_namespace,
  const char *desktop_device_id
) {
  char payload[BR_MAX_JSON];
  size_t used = 0;
  if (!server || !ssid || ssid[0] == '\0') {
    return false;
  }
  if (!mqtt_url || mqtt_url[0] == '\0') {
    // Fall back to whatever the device was already configured with when the
    // caller omits mqttUrl (the pairing portal intentionally does not expose
    // that field to end users).
    mqtt_url = server->config.mqtt_url;
  }
  payload[0] = '\0';
  br_snprintf_append(payload, sizeof(payload), &used, "{");
  br_snprintf_append(payload, sizeof(payload), &used, "\"ssid\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, ssid);
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"password\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, password ? password : "");
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"mqttUrl\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, mqtt_url);
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"namespace\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, mqtt_namespace ? mqtt_namespace : "");
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"desktopDeviceId\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, desktop_device_id ? desktop_device_id : "");
  br_snprintf_append(payload, sizeof(payload), &used, "\",\"tsMs\":%lld}", br_now_ms());
  return br_atomic_write_text(server->config.network_config_path, payload);
}

/* Resolves $STATE_DIR (env override of BOARD_RUNTIME_AP_STATE_DIR or the
 * default /tmp/board-runtime-ap) into the caller-owned buffer. */
static void br_server_resolve_ap_state_dir(char *out, size_t out_size) {
  const char *dir = getenv("BOARD_RUNTIME_AP_STATE_DIR");
  if (!dir || !dir[0]) {
    dir = "/tmp/board-runtime-ap";
  }
  snprintf(out, out_size, "%s", dir);
}

/* Persist `ssid`/`password` into $STATE_DIR/sta-apply.creds with 0600 perms,
 * seed $STATE_DIR/last-attempt.json with an `error:"pending"` placeholder,
 * and asynchronously fork sta_apply_cmd. Returns true iff sta_apply_cmd
 * is configured (otherwise the caller should fall back to the legacy
 * write-network-config-only path). The actual switch-over happens inside
 * board-sta-apply.sh; this helper just primes its inputs and detaches. */
static bool br_server_spawn_sta_apply(br_server_state *server,
                                       const char *ssid,
                                       const char *password) {
  if (!server || !ssid || ssid[0] == '\0') {
    return false;
  }
  if (server->config.sta_apply_cmd[0] == '\0') {
    return false;
  }

  char dir[BR_MAX_PATH];
  br_server_resolve_ap_state_dir(dir, sizeof(dir));
  (void) mkdir(dir, 0700);

  char creds_path[BR_MAX_PATH];
  snprintf(creds_path, sizeof(creds_path), "%s/sta-apply.creds", dir);
  char creds_payload[512];
  snprintf(creds_payload, sizeof(creds_payload), "SSID=%s\nPSK=%s\n",
           ssid, password ? password : "");
  if (br_atomic_write_text(creds_path, creds_payload)) {
    (void) chmod(creds_path, 0600);
  }

  char last_path[BR_MAX_PATH];
  snprintf(last_path, sizeof(last_path), "%s/last-attempt.json", dir);
  char pending[256];
  size_t pused = 0;
  pending[0] = '\0';
  br_snprintf_append(pending, sizeof(pending), &pused, "{\"ok\":false,\"ssid\":\"");
  br_json_escape_append(pending, sizeof(pending), &pused, ssid);
  br_snprintf_append(pending, sizeof(pending), &pused,
                     "\",\"error\":\"pending\",\"ip\":\"\",\"atMs\":%lld}",
                     br_now_ms());
  (void) br_atomic_write_text(last_path, pending);

  br_server_logf("STA apply command: %s", server->config.sta_apply_cmd);
  br_server_spawn_shell(server->config.sta_apply_cmd);
  return true;
}

static bool br_handle_pairing_apply_config_post(br_server_state *server, int fd, const char *body) {
  char ssid[128];
  char password[128];
  char mqtt_url[256];
  char mqtt_namespace[64];
  char desktop_device_id[128];
  br_pairing_state previous_state;
  size_t used = 0;
  char response[BR_MAX_JSON];
  char old_state[BR_MAX_TOPIC];
  char old_wildcard[BR_MAX_TOPIC];
  char old_speech[BR_MAX_TOPIC];
  if (!server || !body) {
    return br_send_json_response(fd, 400, "{\"ok\":false,\"error\":\"invalid_json\"}") == 0;
  }

  if (!br_server_parse_network_config_json(body,
                                           ssid, sizeof(ssid),
                                           password, sizeof(password),
                                           mqtt_url, sizeof(mqtt_url),
                                           mqtt_namespace, sizeof(mqtt_namespace),
                                           desktop_device_id, sizeof(desktop_device_id))) {
    return br_send_json_response(fd, 400, "{\"ok\":false,\"error\":\"invalid_network_config\"}") == 0;
  }
  if (!br_server_write_network_config_file(server, ssid, password, mqtt_url, mqtt_namespace, desktop_device_id)) {
    return br_send_json_response(fd, 500, "{\"ok\":false,\"error\":\"persist_failed\"}") == 0;
  }

  br_normalize_text(server->current_state_topic, "", old_state, sizeof(old_state));
  br_normalize_text(server->wildcard_state_topic, "", old_wildcard, sizeof(old_wildcard));
  br_normalize_text(server->current_speech_topic, "", old_speech, sizeof(old_speech));
  br_normalize_text(mqtt_url, server->config.mqtt_url, server->config.mqtt_url, sizeof(server->config.mqtt_url));
  if (mqtt_namespace[0] != '\0') {
    br_normalize_topic_part(mqtt_namespace,
                            server->config.mqtt_namespace,
                            server->config.mqtt_namespace,
                            sizeof(server->config.mqtt_namespace));
  }
  if (desktop_device_id[0] != '\0') {
    br_normalize_topic_part(desktop_device_id,
                            server->config.target_device_id,
                            server->config.target_device_id,
                            sizeof(server->config.target_device_id));
  }
  br_build_topics(server);

  // If the MQTT broker URL changed, tear down the old connection and re-init
  // so the reconnect loop in main() picks up the new host:port.
  if (server->config.mqtt_url[0] && strcmp(server->mqtt.url, server->config.mqtt_url) != 0) {
    br_server_logf("MQTT URL changed: %s -> %s, reconnecting", server->mqtt.url, server->config.mqtt_url);
    br_mqtt_client_close(&server->mqtt);
    char client_id[64];
    snprintf(client_id, sizeof(client_id), "board-c-%s-%d", server->config.local_device_id, (int) getpid());
    char will_payload[BR_MAX_JSON];
    br_publish_online_payload(server, false, will_payload, sizeof(will_payload));
    br_mqtt_client_init(&server->mqtt,
                        server->config.mqtt_url,
                        client_id,
                        server->config.mqtt_username,
                        server->config.mqtt_password,
                        server->availability_topic,
                        will_payload,
                        30,
                        br_handle_mqtt_publish,
                        server);
    server->mqtt_online = false;
  } else if (server->mqtt.connected) {
    if (old_state[0]) br_mqtt_client_unsubscribe(&server->mqtt, old_state);
    if (old_wildcard[0]) br_mqtt_client_unsubscribe(&server->mqtt, old_wildcard);
    if (old_speech[0]) br_mqtt_client_unsubscribe(&server->mqtt, old_speech);
    br_server_subscribe_topics(server);
  }

  bool delegate_to_sta_apply = (server->config.sta_apply_cmd[0] != '\0' && ssid[0] != '\0');
  if (delegate_to_sta_apply) {
    /* Prevent br_server_on_pairing_transition's set_ap_mode(false) from
     * also spawning ap_down_cmd on top of sta_apply_cmd: mark AP as
     * already-down so the transition callback's early-return path fires.
     * The actual teardown is done inside board-sta-apply.sh. */
    server->ap_mode_active = false;
  }

  previous_state = server->pairing.state;
  if (br_pairing_apply_config(&server->pairing, br_now_ms())) {
    br_server_on_pairing_transition(server, previous_state, server->pairing.state, "config_applied");
  }
  if (delegate_to_sta_apply) {
    (void) br_server_spawn_sta_apply(server, ssid, password);
  }

  response[0] = '\0';
  br_snprintf_append(response, sizeof(response), &used, "{\"ok\":true,\"pairingState\":\"");
  br_json_escape_append(response, sizeof(response), &used, br_pairing_state_name(server->pairing.state));
  br_snprintf_append(response, sizeof(response), &used, "\",\"pairingMode\":\"");
  br_json_escape_append(response, sizeof(response), &used, br_pairing_mode_name(server->pairing.state));
  br_snprintf_append(response, sizeof(response), &used, "\",\"networkConfigPath\":\"");
  br_json_escape_append(response, sizeof(response), &used, server->config.network_config_path);
  br_snprintf_append(response, sizeof(response), &used, "\"}");
  return br_send_json_response(fd, 200, response) == 0;
}

static bool br_handle_pairing_ap_mode_post(br_server_state *server, int fd, const char *body) {
  // Manual AP toggle for bench testing: POST /pairing/ap-mode {"on":true|false}
  // Uses a tolerant JSON sniff so callers can send either `on` or `enabled`.
  bool enable = false;
  bool found = false;
  if (body) {
    const char *p = body;
    while ((p = strstr(p, "\""))) {
      const char *key_start = p + 1;
      const char *key_end = strchr(key_start, '"');
      if (!key_end) break;
      if (((size_t)(key_end - key_start) == 2 && strncmp(key_start, "on", 2) == 0) ||
          ((size_t)(key_end - key_start) == 7 && strncmp(key_start, "enabled", 7) == 0)) {
        const char *colon = strchr(key_end, ':');
        if (colon) {
          const char *v = colon + 1;
          while (*v == ' ' || *v == '\t') v += 1;
          if (strncmp(v, "true", 4) == 0) { enable = true; found = true; break; }
          if (strncmp(v, "false", 5) == 0) { enable = false; found = true; break; }
          if (*v == '1') { enable = true; found = true; break; }
          if (*v == '0') { enable = false; found = true; break; }
        }
      }
      p = key_end + 1;
    }
  }
  if (!found) {
    br_send_json_response(fd, 400, "{\"ok\":false,\"error\":\"missing_on_field\"}");
    return false;
  }

  br_pairing_state previous = server->pairing.state;
  long long now_ms = br_now_ms();
  if (enable) {
    if (server->pairing.state == BR_PAIRING_STA_READY) {
      br_pairing_reset_to_waiting(&server->pairing, now_ms);
    }
    // Force-transition to AP_FALLBACK so br_server_set_ap_mode fires.
    server->pairing.state = BR_PAIRING_WAITING_CONFIG;  // ensure transition happens
    server->pairing.entered_ms = now_ms;
    server->pairing.state = BR_PAIRING_AP_FALLBACK;
    br_server_on_pairing_transition(server, previous, BR_PAIRING_AP_FALLBACK, "manual_ap_on");
  } else {
    server->pairing.state = BR_PAIRING_WAITING_CONFIG;
    server->pairing.entered_ms = now_ms;
    br_server_on_pairing_transition(server, previous, BR_PAIRING_WAITING_CONFIG, "manual_ap_off");
  }

  char response[BR_MAX_JSON];
  size_t used = 0;
  response[0] = '\0';
  br_snprintf_append(response, sizeof(response), &used, "{\"ok\":true,\"pairingState\":\"");
  br_json_escape_append(response, sizeof(response), &used, br_pairing_state_name(server->pairing.state));
  br_snprintf_append(response, sizeof(response), &used, "\",\"apActive\":%s}", server->ap_mode_active ? "true" : "false");
  return br_send_json_response(fd, 200, response) == 0;
}

static bool br_handle_pairing_reset_post(br_server_state *server, int fd) {
  br_pairing_state previous = server->pairing.state;
  (void) unlink(server->config.network_config_path);
  // Kill STA stack so wlan0 is free for AP mode.
  br_server_spawn_shell("killall wpa_supplicant udhcpc 2>/dev/null");
  if (br_pairing_reset_to_waiting(&server->pairing, br_now_ms())) {
    br_server_on_pairing_transition(server, previous, server->pairing.state, "pairing_reset");
  }
  previous = server->pairing.state;
  if (br_pairing_tick(&server->pairing, br_now_ms())) {
    br_server_on_pairing_transition(server, previous, server->pairing.state, "restart_discovery");
  }
  return br_send_json_response(fd, 200, "{\"ok\":true}") == 0;
}

static bool br_handle_http_connection(br_server_state *server, int fd) {
  char request[BR_HTTP_BUFFER];
  ssize_t read_size;
  char *header_end = NULL;
  char method[16];
  char path[BR_MAX_PATH];
  int content_length = 0;
  char request_host[128] = "";

  struct timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  if (!br_read_http_request(fd, request, sizeof(request), &read_size, &header_end, &content_length)) {
    if (read_size <= 0) {
      return false;
    }
    if (content_length > 8192) {
      br_send_json_response(fd, 413, "{\"ok\":false,\"error\":\"payload_too_large\"}");
      return false;
    }
    br_send_text_response(fd, 400, "text/plain; charset=utf-8", "bad request");
    return false;
  }

  if (sscanf(request, "%15s %1023s", method, path) != 2) {
    br_send_text_response(fd, 400, "text/plain; charset=utf-8", "bad request");
    return false;
  }
  br_extract_header_value(request, "Host", request_host, sizeof(request_host));
  char *host_colon = strchr(request_host, ':');
  if (host_colon) {
    *host_colon = '\0';
  }

  char connection_header[128];
  char upgrade_header[64];
  char websocket_key[256];
  if (br_extract_header_value(request, "Connection", connection_header, sizeof(connection_header)) &&
      br_extract_header_value(request, "Upgrade", upgrade_header, sizeof(upgrade_header)) &&
      br_extract_header_value(request, "Sec-WebSocket-Key", websocket_key, sizeof(websocket_key)) &&
      strcasecmp(upgrade_header, "websocket") == 0 &&
      strcasestr(connection_header, "Upgrade") != NULL) {
    unsigned char hash[20];
    char accept_input[512];
    char accept_key[64];
    char response[512];
    br_sha1_ctx sha1;
    snprintf(accept_input, sizeof(accept_input), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", websocket_key);
    br_sha1_init(&sha1);
    br_sha1_update(&sha1, (const unsigned char *) accept_input, strlen(accept_input));
    br_sha1_final(&sha1, hash);
    br_base64_encode(hash, sizeof(hash), accept_key, sizeof(accept_key));
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept_key);
    br_send_all(fd, response, strlen(response));
    br_set_fd_nonblocking(fd);
    for (size_t i = 0; i < BR_MAX_WS_CLIENTS; i += 1) {
      if (!server->ws_clients[i].active) {
        server->ws_clients[i].fd = fd;
        server->ws_clients[i].active = true;
        char snapshot[BR_MAX_JSON];
        if (br_build_snapshot_json(server, snapshot, sizeof(snapshot))) {
          br_ws_send_json(&server->ws_clients[i], snapshot);
        }
        return true;
      }
    }
    close(fd);
    return true;
  }

  if ((strcmp(path, "/") == 0) || (strcmp(path, "/index.html") == 0)) {
    char file_path[BR_MAX_PATH];
    // When the board is sitting in AP fallback, the only thing users can do
    // via the AP is finish provisioning, so treat `/` (and common captive
    // portal probes hitting the root) as a shortcut to the pairing portal.
    if (server->pairing.state == BR_PAIRING_AP_FALLBACK) {
      snprintf(file_path, sizeof(file_path), "%s/ui/pairing-portal.html", server->config.root_dir);
    } else {
      snprintf(file_path, sizeof(file_path), "%s/ui/windows/main/index.html", server->config.root_dir);
    }
    br_serve_file(fd, file_path);
    return false;
  }

  if (strcmp(path, "/board-runtime-config.json") == 0) {
    char json[BR_MAX_JSON];
    br_build_config_json(server, request_host, json, sizeof(json));
    br_send_json_response(fd, 200, json);
    return false;
  }

  if (strcmp(path, "/debug/state") == 0) {
    char json[BR_MAX_JSON];
    if (strcmp(method, "GET") != 0) {
      br_send_json_response(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
      return false;
    }
    br_debug_build_state_json(server->config.root_dir, json, sizeof(json));
    br_send_json_response(fd, 200, json);
    return false;
  }

  if (strcmp(path, "/debug/overlay") == 0) {
    const char *body = header_end + 4;
    bool enabled = false;
    char json[BR_MAX_JSON];
    if (strcmp(method, "POST") != 0) {
      br_send_json_response(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
      return false;
    }
    if (content_length > 1024 || !br_debug_parse_overlay_toggle_json(body, &enabled)) {
      br_send_json_response(fd, 400, "{\"ok\":false,\"error\":\"invalid_json\"}");
      return false;
    }
    if (!br_debug_set_overlay_enabled(server->config.root_dir, enabled)) {
      br_send_json_response(fd, 500, "{\"ok\":false,\"error\":\"debug_toggle_failed\"}");
      return false;
    }
    if (!enabled) {
      br_atomic_write_text(server->config.current_debug_speech_path, "");
    }
    br_debug_build_state_json(server->config.root_dir, json, sizeof(json));
    br_send_json_response(fd, 200, json);
    return false;
  }

  if (strcmp(path, "/pairing/state") == 0) {
    char json[BR_MAX_JSON];
    size_t used = 0;
    json[0] = '\0';
    br_snprintf_append(json, sizeof(json), &used, "{");
    br_snprintf_append(json, sizeof(json), &used, "\"boardDeviceId\":\"");
    br_json_escape_append(json, sizeof(json), &used, server->config.board_device_id);
    br_snprintf_append(json, sizeof(json), &used, "\",\"pairingState\":\"");
    br_json_escape_append(json, sizeof(json), &used, br_pairing_state_name(server->pairing.state));
    br_snprintf_append(json, sizeof(json), &used, "\",\"pairingMode\":\"");
    br_json_escape_append(json, sizeof(json), &used, br_pairing_mode_name(server->pairing.state));
    br_snprintf_append(json, sizeof(json), &used, "\",\"apIp\":\"");
    br_json_escape_append(json, sizeof(json), &used, server->config.ap_ip);
    br_snprintf_append(json, sizeof(json), &used, "\",\"apSsid\":\"");
    br_json_escape_append(json, sizeof(json), &used, server->config.ap_ssid);
    br_snprintf_append(json, sizeof(json), &used, "\",\"apPsk\":\"");
    br_json_escape_append(json, sizeof(json), &used, server->config.ap_psk);
    br_snprintf_append(json, sizeof(json), &used, "\",\"discoveryUdpPort\":%d", server->config.discovery_udp_port);
    br_snprintf_append(json, sizeof(json), &used, ",\"discoveryMdnsPort\":%d", server->config.discovery_mdns_port);
    br_snprintf_append(json, sizeof(json), &used, ",\"hint\":\"");
    br_json_escape_append(json, sizeof(json), &used, server->pairing_message);
    // Echo the currently-bound desktop identity + MQTT namespace so the
    // portal (and the eventual Pet Manager) can tell at a glance who this
    // board is paired with without waiting for the next MQTT handshake.
    br_snprintf_append(json, sizeof(json), &used, "\",\"desktopDeviceId\":\"");
    br_json_escape_append(json, sizeof(json), &used, server->config.target_device_id);
    br_snprintf_append(json, sizeof(json), &used, "\",\"mqttNamespace\":\"");
    br_json_escape_append(json, sizeof(json), &used, server->config.mqtt_namespace);
    br_snprintf_append(json, sizeof(json), &used, "\"");
    // Surface the most recent STA-apply result so the portal can poll this
    // endpoint post-submit and distinguish "still trying" / "success" /
    // "password wrong".  The file is produced by board-sta-apply.sh and
    // contains a fully-formed JSON object, so we splice it in directly.
    {
      char la_path[BR_MAX_PATH];
      char la_buf[768];
      const char *dir = getenv("BOARD_RUNTIME_AP_STATE_DIR");
      if (!dir || !dir[0]) { dir = "/tmp/board-runtime-ap"; }
      snprintf(la_path, sizeof(la_path), "%s/last-attempt.json", dir);
      if (br_read_text_file(la_path, la_buf, sizeof(la_buf)) && la_buf[0]) {
        size_t n = strlen(la_buf);
        while (n > 0 && (la_buf[n - 1] == '\n' || la_buf[n - 1] == '\r' || la_buf[n - 1] == ' ')) {
          la_buf[--n] = '\0';
        }
        if (n > 0) {
          br_snprintf_append(json, sizeof(json), &used, ",\"lastAttempt\":%s", la_buf);
        }
      }
    }
    br_snprintf_append(json, sizeof(json), &used, "}");
    br_send_json_response(fd, 200, json);
    return false;
  }

  if (strcmp(path, "/pairing/portal") == 0) {
    char file_path[BR_MAX_PATH];
    snprintf(file_path, sizeof(file_path), "%s/ui/pairing-portal.html", server->config.root_dir);
    br_serve_file(fd, file_path);
    return false;
  }

  // Wi-Fi scan results cached by board-ap-up.sh right before we switch wlan0
  // from STA -> AP (the radio cannot actively scan while acting as an AP, so
  // the portal shows whatever was visible at switch-over time).  If the file
  // is missing or unreadable, return an empty list so the portal degrades to
  // a plain text entry rather than erroring.
  if (strcmp(path, "/wifi/scan") == 0) {
    char json[4096];
    char file_path[BR_MAX_PATH];
    const char *dir = getenv("BOARD_RUNTIME_AP_STATE_DIR");
    if (!dir || !dir[0]) { dir = "/tmp/board-runtime-ap"; }
    snprintf(file_path, sizeof(file_path), "%s/wifi-scan.json", dir);
    if (!br_read_text_file(file_path, json, sizeof(json)) || json[0] == '\0') {
      snprintf(json, sizeof(json), "{\"networks\":[],\"updatedAt\":0}");
    }
    br_send_json_response(fd, 200, json);
    return false;
  }

  if (strcmp(path, "/pairing/apply-config") == 0) {
    const char *body = header_end + 4;
    if (strcmp(method, "POST") != 0) {
      br_send_json_response(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
      return false;
    }
    if (!br_require_sensitive_api_auth(server, fd, request, path)) {
      return false;
    }
    if (content_length > 8192) {
      br_send_json_response(fd, 413, "{\"ok\":false,\"error\":\"payload_too_large\"}");
      return false;
    }
    br_handle_pairing_apply_config_post(server, fd, body);
    return false;
  }

  if (strcmp(path, "/pairing/ap-mode") == 0) {
    const char *body = header_end + 4;
    if (strcmp(method, "POST") != 0) {
      br_send_json_response(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
      return false;
    }
    if (!br_require_sensitive_api_auth(server, fd, request, path)) {
      return false;
    }
    if (content_length > 1024) {
      br_send_json_response(fd, 413, "{\"ok\":false,\"error\":\"payload_too_large\"}");
      return false;
    }
    br_handle_pairing_ap_mode_post(server, fd, body);
    return false;
  }

  if (strcmp(path, "/pairing/reset") == 0) {
    if (strcmp(method, "POST") != 0) {
      br_send_json_response(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
      return false;
    }
    if (!br_require_sensitive_api_auth(server, fd, request, path)) {
      return false;
    }
    br_handle_pairing_reset_post(server, fd);
    return false;
  }

  if (strcmp(path, "/input/action") == 0) {
    const char *body = header_end + 4;
    size_t body_length = strlen(body);
    if (strcmp(method, "POST") != 0) {
      br_send_json_response(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
      return false;
    }
    if (content_length > 8192) {
      br_send_json_response(fd, 413, "{\"ok\":false,\"error\":\"payload_too_large\"}");
      return false;
    }
    if ((int) body_length < content_length) {
      br_send_json_response(fd, 400, "{\"ok\":false,\"error\":\"invalid_json\"}");
      return false;
    }
    br_handle_input_action_post(server, fd, body);
    return false;
  }

  if (strcmp(path, "/screen/page") == 0) {
    const char *body = header_end + 4;
    size_t body_length = strlen(body);
    if (strcmp(method, "POST") != 0) {
      br_send_json_response(fd, 405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
      return false;
    }
    if (content_length > 1024) {
      br_send_json_response(fd, 413, "{\"ok\":false,\"error\":\"payload_too_large\"}");
      return false;
    }
    if ((int) body_length < content_length) {
      br_send_json_response(fd, 400, "{\"ok\":false,\"error\":\"invalid_body\"}");
      return false;
    }
    char page[16];
    if (!br_resolve_screen_page(body, page, sizeof(page))) {
      br_send_json_response(fd, 400, "{\"ok\":false,\"error\":\"invalid_page\"}");
      return false;
    }
    br_server_apply_screen_page(server, page, "http");
    char response[64];
    snprintf(response, sizeof(response), "{\"ok\":true,\"page\":\"%s\"}", page);
    br_send_json_response(fd, 200, response);
    return false;
  }

  const char *prefixes[] = {"/ui/", "/assets/", "/src/assets/"};
  const char *bases[] = {"ui", "assets", "assets"};
  for (size_t i = 0; i < 3; i += 1) {
    size_t prefix_length = strlen(prefixes[i]);
    if (strncmp(path, prefixes[i], prefix_length) == 0) {
      char base_path[BR_MAX_PATH];
      char file_path[BR_MAX_PATH];
      snprintf(base_path, sizeof(base_path), "%s/%s", server->config.root_dir, bases[i]);
      if (!br_safe_join(base_path, path + prefix_length, file_path, sizeof(file_path))) {
        br_send_text_response(fd, 403, "text/plain; charset=utf-8", "forbidden");
        return false;
      }
      br_serve_file(fd, file_path);
      return false;
    }
  }

  br_send_text_response(fd, 404, "text/plain; charset=utf-8", "not found");
  return false;
}

static void br_server_poll_ws(br_server_state *server) {
  fd_set read_set;
  int max_fd = -1;
  struct timeval timeout;
  FD_ZERO(&read_set);
  for (size_t i = 0; i < BR_MAX_WS_CLIENTS; i += 1) {
    if (!server->ws_clients[i].active) {
      continue;
    }
    FD_SET(server->ws_clients[i].fd, &read_set);
    if (server->ws_clients[i].fd > max_fd) {
      max_fd = server->ws_clients[i].fd;
    }
  }
  if (max_fd < 0) {
    return;
  }
  timeout.tv_sec = 0;
  timeout.tv_usec = 0;
  if (select(max_fd + 1, &read_set, NULL, NULL, &timeout) <= 0) {
    return;
  }
  for (size_t i = 0; i < BR_MAX_WS_CLIENTS; i += 1) {
    char buffer[256];
    if (!server->ws_clients[i].active || !FD_ISSET(server->ws_clients[i].fd, &read_set)) {
      continue;
    }
    ssize_t read_size = recv(server->ws_clients[i].fd, buffer, sizeof(buffer), 0);
    if (read_size <= 0) {
      br_ws_close(&server->ws_clients[i]);
    }
  }
}

static void br_server_tick_session_machine(br_server_state *server) {
  br_session_resolution resolution;
  long long now = br_now_ms();
  if (!server || now < server->next_session_tick_ms) {
    return;
  }
  server->next_session_tick_ms = now + 250;
  if (br_session_machine_tick(&server->session_machine, now, &resolution)) {
    br_write_state_files_with_reason(server, resolution.state, resolution.event, resolution.reason);
    snprintf(server->last_state, sizeof(server->last_state), "%s", resolution.state);
    snprintf(server->last_reason, sizeof(server->last_reason), "%s", resolution.reason);
    server->last_state_update_ms = now;
  }
}

static void br_server_tick_speech_records(br_server_state *server) {
  char speech[2048];
  long long now = br_now_ms();
  if (!server || now < server->next_speech_tick_ms) {
    return;
  }
  server->next_speech_tick_ms = now + 250;
  if (!br_server_cleanup_speech_records(server, now)) {
    return;
  }
  if (br_server_render_speech_records(server, speech, sizeof(speech), now)) {
    br_atomic_write_text(server->config.current_speech_path, speech);
    br_write_speech_hold_until_abs(server, br_server_latest_speech_expiry_ms(server));
  } else {
    br_atomic_write_text(server->config.current_speech_path, "");
    br_write_speech_hold_until_abs(server, 0);
  }
}

static void br_server_load_config(br_server_state *server, const char *root_dir) {
  char hostname[128];
  memset(server, 0, sizeof(*server));
  server->listen_fd = -1;
  server->discovery_fd = -1;
  server->mdns_fd = -1;
  for (size_t i = 0; i < BR_MAX_WS_CLIENTS; i += 1) {
    server->ws_clients[i].fd = -1;
  }

  br_normalize_text(root_dir, ".", server->config.root_dir, sizeof(server->config.root_dir));
  snprintf(server->config.device_config_path, sizeof(server->config.device_config_path), "%s/device-config.json", server->config.root_dir);
  snprintf(server->config.network_config_path, sizeof(server->config.network_config_path), "%s/network-config.json", server->config.root_dir);
  snprintf(server->config.current_state_path, sizeof(server->config.current_state_path), "%s/.current-state", server->config.root_dir);
  snprintf(server->config.current_event_path, sizeof(server->config.current_event_path), "%s/.current-event", server->config.root_dir);
  snprintf(server->config.current_speech_path, sizeof(server->config.current_speech_path), "%s/.current-speech", server->config.root_dir);
  snprintf(server->config.current_speech_hold_until_path, sizeof(server->config.current_speech_hold_until_path), "%s/.current-speech-hold-until", server->config.root_dir);
  snprintf(server->config.current_debug_speech_path, sizeof(server->config.current_debug_speech_path), "%s/.current-debug-speech", server->config.root_dir);
  snprintf(server->config.screen_interrupt_path, sizeof(server->config.screen_interrupt_path), "%s/.screen-interrupt", server->config.root_dir);
  snprintf(server->config.screen_page_path, sizeof(server->config.screen_page_path), "%s/.screen-page", server->config.root_dir);
  snprintf(server->config.audio_bridge_config_path, sizeof(server->config.audio_bridge_config_path), "%s/.audio-bridge-config", server->config.root_dir);
  snprintf(server->config.voice_button_config_path, sizeof(server->config.voice_button_config_path), "%s/.voice-button-config", server->config.root_dir);
  snprintf(server->config.button_config_path, sizeof(server->config.button_config_path), "%s/.button-config", server->config.root_dir);
  snprintf(server->config.sound_script_path, sizeof(server->config.sound_script_path), "%s/board-sound.sh", server->config.root_dir);

  br_normalize_text(getenv("BOARD_RUNTIME_HOST"), "0.0.0.0", server->config.http_host, sizeof(server->config.http_host));
  server->config.http_port = getenv("BOARD_RUNTIME_PORT") ? atoi(getenv("BOARD_RUNTIME_PORT")) : 80;
  if (server->config.http_port <= 0 || server->config.http_port > 65535) {
    server->config.http_port = 80;
  }
  br_normalize_text(getenv("PET_CLAW_MQTT_URL") ? getenv("PET_CLAW_MQTT_URL") : getenv("MQTT_URL"),
                    "mqtt://broker.openclaw.example:1883",
                    server->config.mqtt_url,
                    sizeof(server->config.mqtt_url));
  br_normalize_text(getenv("PET_CLAW_MQTT_USERNAME") ? getenv("PET_CLAW_MQTT_USERNAME") : getenv("MQTT_USERNAME"),
                    "device",
                    server->config.mqtt_username,
                    sizeof(server->config.mqtt_username));
  br_normalize_text(getenv("PET_CLAW_MQTT_PASSWORD") ? getenv("PET_CLAW_MQTT_PASSWORD") : getenv("MQTT_PASSWORD"),
                    "",
                    server->config.mqtt_password,
                    sizeof(server->config.mqtt_password));
  br_normalize_text(getenv("BOARD_RUNTIME_ADMIN_TOKEN") ? getenv("BOARD_RUNTIME_ADMIN_TOKEN") : getenv("PET_CLAW_ADMIN_TOKEN"),
                    "",
                    server->config.admin_token,
                    sizeof(server->config.admin_token));
  br_normalize_topic_part(getenv("PET_CLAW_MQTT_NAMESPACE") ? getenv("PET_CLAW_MQTT_NAMESPACE") : getenv("STATUS_NAMESPACE"),
                          "desk",
                          server->config.mqtt_namespace,
                          sizeof(server->config.mqtt_namespace));
  if (!br_normalize_topic_part(getenv("PET_CLAW_DEVICE_ID"), "", server->config.local_device_id, sizeof(server->config.local_device_id)) ||
      server->config.local_device_id[0] == '\0') {
    if (!br_read_device_id_json(server->config.device_config_path, server->config.local_device_id, sizeof(server->config.local_device_id))) {
      if (br_get_hostname_text(hostname, sizeof(hostname))) {
        br_normalize_topic_part(hostname, "linux-pet-01", server->config.local_device_id, sizeof(server->config.local_device_id));
      } else {
        br_normalize_text("linux-pet-01", "linux-pet-01", server->config.local_device_id, sizeof(server->config.local_device_id));
      }
    }
  }
  br_normalize_text(server->config.local_device_id, "linux-pet-01", server->config.board_device_id, sizeof(server->config.board_device_id));
  br_normalize_text(getenv("PET_SCREEN_NAME"), "OpenClaw Board Runtime", server->config.screen_name, sizeof(server->config.screen_name));
  br_normalize_text(getenv("PET_SCREEN_MODEL"), "zqboard-t113-board-runtime", server->config.screen_model, sizeof(server->config.screen_model));
  br_normalize_text(getenv("PET_SCREEN_FW"), "0.1.0", server->config.screen_fw, sizeof(server->config.screen_fw));
  br_normalize_topic_part(getenv("PET_CLAW_TARGET_DEVICE_ID"), server->config.local_device_id, server->config.target_device_id, sizeof(server->config.target_device_id));
  br_normalize_topic_part(getenv("PET_CLAW_TARGET_SOURCE"), "", server->config.target_source, sizeof(server->config.target_source));
  br_normalize_text(getenv("BOARD_RUNTIME_PUBLIC_HOST"), "", server->config.public_host, sizeof(server->config.public_host));
  if (server->config.public_host[0] == '\0') {
    br_get_first_lan_ipv4(server->config.public_host, sizeof(server->config.public_host));
  }
  br_normalize_text(getenv("BOARD_RUNTIME_PUBLIC_URL"), "", server->config.public_url, sizeof(server->config.public_url));
  br_normalize_text(getenv("BOARD_RUNTIME_AP_IP"), "192.168.44.1", server->config.ap_ip, sizeof(server->config.ap_ip));
  server->config.discovery_udp_port = getenv("BOARD_RUNTIME_DISCOVERY_UDP_PORT")
    ? atoi(getenv("BOARD_RUNTIME_DISCOVERY_UDP_PORT"))
    : 19890;
  if (server->config.discovery_udp_port <= 0 || server->config.discovery_udp_port > 65535) {
    server->config.discovery_udp_port = 19890;
  }
  server->config.discovery_mdns_port = getenv("BOARD_RUNTIME_DISCOVERY_MDNS_PORT")
    ? atoi(getenv("BOARD_RUNTIME_DISCOVERY_MDNS_PORT"))
    : 5353;
  if (server->config.discovery_mdns_port <= 0 || server->config.discovery_mdns_port > 65535) {
    server->config.discovery_mdns_port = 5353;
  }
  // Historically defaulted to 45s auto-fallback.  Real field installs run
  // over STA exclusively, so the automatic AP fallback is only useful for
  // bench testing and must be opt-in (set BOARD_RUNTIME_DISCOVERY_TIMEOUT_MS
  // to a positive value to re-enable it).  Zero / negative means never
  // auto-fallback; AP mode can still be toggled explicitly via the
  // /pairing/ap-mode HTTP endpoint.
  server->config.discovery_timeout_ms = getenv("BOARD_RUNTIME_DISCOVERY_TIMEOUT_MS")
    ? atoi(getenv("BOARD_RUNTIME_DISCOVERY_TIMEOUT_MS"))
    : 0;
  // Guard against very short manual values that would thrash AP mode; snap
  // them back to the safe 45s default.  Zero / negative keep the auto
  // fallback disabled entirely.
  if (server->config.discovery_timeout_ms > 0 &&
      server->config.discovery_timeout_ms < 5000) {
    server->config.discovery_timeout_ms = 45000;
  }
  server->config.discovery_announce_interval_ms = getenv("BOARD_RUNTIME_DISCOVERY_ANNOUNCE_MS")
    ? atoi(getenv("BOARD_RUNTIME_DISCOVERY_ANNOUNCE_MS"))
    : 3000;
  if (server->config.discovery_announce_interval_ms < 500) {
    server->config.discovery_announce_interval_ms = 3000;
  }
  br_normalize_text(getenv("BOARD_RUNTIME_AP_UP_CMD"), "", server->config.ap_up_cmd, sizeof(server->config.ap_up_cmd));
  br_normalize_text(getenv("BOARD_RUNTIME_AP_DOWN_CMD"), "", server->config.ap_down_cmd, sizeof(server->config.ap_down_cmd));
  br_normalize_text(getenv("BOARD_RUNTIME_STA_APPLY_CMD"), "", server->config.sta_apply_cmd, sizeof(server->config.sta_apply_cmd));
  br_normalize_text(getenv("BOARD_RUNTIME_AP_SSID"), "claw-pet", server->config.ap_ssid, sizeof(server->config.ap_ssid));
  br_normalize_text(getenv("BOARD_RUNTIME_AP_PSK"), "88888888", server->config.ap_psk, sizeof(server->config.ap_psk));
  {
    long long done_hold_ms = getenv("BOARD_SESSION_DONE_HOLD_MS")
      ? atoll(getenv("BOARD_SESSION_DONE_HOLD_MS"))
      : 3000;
    long long stale_ms = getenv("BOARD_SESSION_STALE_MS")
      ? atoll(getenv("BOARD_SESSION_STALE_MS"))
      : 300000;
    br_session_machine_init(&server->session_machine, done_hold_ms, stale_ms);
  }
  server->next_session_tick_ms = br_now_ms() + 250;
  server->next_speech_tick_ms = br_now_ms() + 250;
  server->use_legacy_active_topic = getenv("BOARD_ACCEPT_LEGACY_ACTIVE") &&
    strcmp(getenv("BOARD_ACCEPT_LEGACY_ACTIVE"), "1") == 0;
  server->pairing_message[0] = '\0';
  server->last_discovery_peer[0] = '\0';
  server->last_discovery_announce_ms = 0;

  br_build_topics(server);
}

static void br_self_check_begin(br_self_check_report *report, const br_server_state *server) {
  if (!report || !server) {
    return;
  }
  if (!report->json) {
    return;
  }
  report->first = true;
  printf("{\"boardDeviceId\":\"%s\",\"rootDir\":\"%s\",\"checks\":[",
         server->config.board_device_id,
         server->config.root_dir);
}

static void br_self_check_emit(
  br_self_check_report *report,
  const char *id,
  bool required,
  bool passed,
  const char *detail
) {
  const char *status = passed ? "pass" : (required ? "fail" : "warn");
  const char *note = detail ? detail : "";

  if (!report || !id) {
    return;
  }

  report->total += 1;
  if (passed) {
    report->passed += 1;
  } else if (required) {
    report->failed += 1;
  } else {
    report->warnings += 1;
  }

  if (report->json) {
    char escaped[1024];
    size_t used = 0;
    escaped[0] = '\0';
    br_json_escape_append(escaped, sizeof(escaped), &used, note);
    if (!report->first) {
      printf(",");
    }
    printf("{\"id\":\"%s\",\"required\":%s,\"status\":\"%s\",\"detail\":\"%s\"}",
           id,
           required ? "true" : "false",
           status,
           escaped);
    report->first = false;
    return;
  }

  if (passed) {
    printf("[self-check] [OK]   %s: %s\n", id, note);
  } else if (required) {
    printf("[self-check] [FAIL] %s: %s\n", id, note);
  } else {
    printf("[self-check] [WARN] %s: %s\n", id, note);
  }
}

static bool br_self_check_regular_file(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool br_self_check_directory(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool br_self_check_probe_runtime_contract(const char *path, char *detail, size_t detail_size) {
  char previous[BR_MAX_JSON];
  char verify[BR_MAX_TEXT];
  const char *probe = "__board_runtime_self_check__";
  bool had_previous = br_read_text_file(path, previous, sizeof(previous));
  bool ok = true;

  if (!br_atomic_write_text(path, probe)) {
    snprintf(detail, detail_size, "write failed (%s)", path);
    return false;
  }
  if (!br_read_text_file(path, verify, sizeof(verify)) || strcmp(verify, probe) != 0) {
    ok = false;
    snprintf(detail, detail_size, "readback mismatch (%s)", path);
  }

  if (had_previous) {
    if (!br_atomic_write_text(path, previous)) {
      ok = false;
      snprintf(detail, detail_size, "restore failed (%s)", path);
    }
  } else if (unlink(path) != 0 && errno != ENOENT) {
    ok = false;
    snprintf(detail, detail_size, "cleanup failed (%s)", path);
  }

  if (ok) {
    snprintf(detail, detail_size, "read/write ok (%s)", path);
  }
  return ok;
}

static bool br_self_check_find_executable(
  const br_server_state *server,
  const char *name,
  char *resolved,
  size_t resolved_size
) {
  char direct[BR_MAX_PATH];
  char build[BR_MAX_PATH];
  snprintf(direct, sizeof(direct), "%s/%s", server->config.root_dir, name);
  if (access(direct, X_OK) == 0 && br_self_check_regular_file(direct)) {
    return br_normalize_text(direct, "", resolved, resolved_size);
  }
  snprintf(build, sizeof(build), "%s/build/%s", server->config.root_dir, name);
  if (access(build, X_OK) == 0 && br_self_check_regular_file(build)) {
    return br_normalize_text(build, "", resolved, resolved_size);
  }
  return false;
}

static bool br_server_run_self_check(br_server_state *server, bool json_output) {
  br_self_check_report report;
  char detail[256];
  char path[BR_MAX_PATH];
  char exec_path[BR_MAX_PATH];
  br_mqtt_endpoint endpoint;
  char ipv4[64];
  int probe_fd = -1;

  if (!server) {
    return false;
  }

  memset(&report, 0, sizeof(report));
  report.json = json_output;
  br_self_check_begin(&report, server);

  snprintf(detail, sizeof(detail), "root=%s", server->config.root_dir);
  br_self_check_emit(&report, "runtime_root", true, br_self_check_directory(server->config.root_dir), detail);

  snprintf(path, sizeof(path), "%s/ui/windows/main/index.html", server->config.root_dir);
  snprintf(detail, sizeof(detail), "%s", path);
  br_self_check_emit(&report, "ui_entry", true, br_self_check_regular_file(path), detail);

  snprintf(path, sizeof(path), "%s/assets/pets/terrier/0-idle.jpeg", server->config.root_dir);
  snprintf(detail, sizeof(detail), "%s", path);
  br_self_check_emit(&report, "base_asset", true, br_self_check_regular_file(path), detail);

  if (br_self_check_find_executable(server, "board-touch-input", exec_path, sizeof(exec_path))) {
    snprintf(detail, sizeof(detail), "%s", exec_path);
    br_self_check_emit(&report, "binary_touch_input", true, true, detail);
  } else {
    br_self_check_emit(&report, "binary_touch_input", true, false, "missing executable board-touch-input");
  }

  if (br_self_check_find_executable(server, "fb-speech-overlay", exec_path, sizeof(exec_path))) {
    snprintf(detail, sizeof(detail), "%s", exec_path);
    br_self_check_emit(&report, "binary_fb_speech_overlay", true, true, detail);
  } else {
    br_self_check_emit(&report, "binary_fb_speech_overlay", true, false, "missing executable fb-speech-overlay");
  }

  snprintf(detail, sizeof(detail), "boardDeviceId=%s", server->config.board_device_id);
  br_self_check_emit(&report, "board_device_id", true, server->config.board_device_id[0] != '\0', detail);

  if (br_parse_mqtt_url(server->config.mqtt_url, &endpoint)) {
    snprintf(detail, sizeof(detail), "mqtt://%s:%d", endpoint.host, endpoint.port);
    br_self_check_emit(&report, "mqtt_url", true, true, detail);
  } else {
    snprintf(detail, sizeof(detail), "invalid mqtt url: %s", server->config.mqtt_url);
    br_self_check_emit(&report, "mqtt_url", true, false, detail);
  }

  snprintf(detail, sizeof(detail), "namespace=%s", server->config.mqtt_namespace);
  br_self_check_emit(&report, "mqtt_namespace", true, server->config.mqtt_namespace[0] != '\0', detail);

  if (server->pairing.network_config_valid) {
    snprintf(detail, sizeof(detail), "valid %s", server->config.network_config_path);
    br_self_check_emit(&report, "network_config", false, true, detail);
  } else {
    snprintf(detail, sizeof(detail), "missing/invalid %s (will enter pairing mode)", server->config.network_config_path);
    br_self_check_emit(&report, "network_config", false, false, detail);
  }

  br_self_check_emit(&report,
                     "contract_current_state",
                     true,
                     br_self_check_probe_runtime_contract(server->config.current_state_path, detail, sizeof(detail)),
                     detail);
  br_self_check_emit(&report,
                     "contract_current_event",
                     true,
                     br_self_check_probe_runtime_contract(server->config.current_event_path, detail, sizeof(detail)),
                     detail);
  br_self_check_emit(&report,
                     "contract_current_speech",
                     true,
                     br_self_check_probe_runtime_contract(server->config.current_speech_path, detail, sizeof(detail)),
                     detail);

  probe_fd = br_server_open_listener(server);
  if (probe_fd >= 0) {
    close(probe_fd);
    snprintf(detail, sizeof(detail), "bind ok %s:%d", server->config.http_host, server->config.http_port);
    br_self_check_emit(&report, "http_bind", true, true, detail);
  } else {
    snprintf(detail, sizeof(detail), "bind failed %s:%d", server->config.http_host, server->config.http_port);
    br_self_check_emit(&report, "http_bind", true, false, detail);
  }

  if (br_get_first_lan_ipv4(ipv4, sizeof(ipv4))) {
    snprintf(detail, sizeof(detail), "lan ipv4=%s", ipv4);
    br_self_check_emit(&report, "lan_ipv4", false, true, detail);
  } else {
    br_self_check_emit(&report, "lan_ipv4", false, false, "no non-loopback ipv4");
  }

  const char *fb_dev = getenv("PET_CLAW_FB_DEV");
  if (!fb_dev || !fb_dev[0] || strcmp(fb_dev, "auto") == 0) {
    fb_dev = "/dev/fb0";
  }
  if (access(fb_dev, R_OK | W_OK) == 0) {
    snprintf(detail, sizeof(detail), "%s rw ok", fb_dev);
    br_self_check_emit(&report, "framebuffer_device", false, true, detail);
  } else {
    snprintf(detail, sizeof(detail), "%s not accessible", fb_dev);
    br_self_check_emit(&report, "framebuffer_device", false, false, detail);
  }

  if (report.json) {
    printf("],\"summary\":{\"total\":%d,\"passed\":%d,\"failed\":%d,\"warnings\":%d},\"ok\":%s}\n",
           report.total,
           report.passed,
           report.failed,
           report.warnings,
           report.failed == 0 ? "true" : "false");
  } else {
    printf("[self-check] summary: total=%d passed=%d failed=%d warnings=%d\n",
           report.total,
           report.passed,
           report.failed,
           report.warnings);
  }

  return report.failed == 0;
}

static void br_server_publish_offline(br_server_state *server) {
  char payload[BR_MAX_JSON];
  br_publish_online_payload(server, false, payload, sizeof(payload));
  br_server_publish(server, server->availability_topic, "availability", payload, true);
}

int main(int argc, char **argv) {
  br_server_state server;
  char client_id[128];
  char will_payload[BR_MAX_JSON];
  char root_dir[BR_MAX_PATH];
  char network_detail[256];
  long long last_usb_hotplug_probe_ms = 0;
  bool network_config_valid = false;
  bool run_self_check = false;
  bool self_check_json = false;
  bool self_check_strict = false;

  br_normalize_text(".", ".", root_dir, sizeof(root_dir));
  for (int i = 1; i < argc; i += 1) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      printf("usage: %s [runtime-root] [--self-check] [--self-check-json]\n", argv[0]);
      return 0;
    }
    if (strcmp(argv[i], "--self-check") == 0) {
      run_self_check = true;
      continue;
    }
    if (strcmp(argv[i], "--self-check-json") == 0) {
      run_self_check = true;
      self_check_json = true;
      continue;
    }
    if (argv[i][0] == '-') {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 2;
    }
    br_normalize_text(argv[i], ".", root_dir, sizeof(root_dir));
  }
  if (getenv("BOARD_RUNTIME_SELF_CHECK_STRICT") &&
      strcmp(getenv("BOARD_RUNTIME_SELF_CHECK_STRICT"), "1") == 0) {
    self_check_strict = true;
  }

  br_server_load_config(&server, root_dir);
  network_config_valid = br_server_load_network_config(&server, network_detail, sizeof(network_detail));
  br_build_topics(&server);
  br_pairing_init(&server.pairing, network_config_valid, server.config.discovery_timeout_ms, br_now_ms());

  if (run_self_check) {
    return br_server_run_self_check(&server, self_check_json) ? 0 : 2;
  }

  if (network_config_valid) {
    br_server_logf("network config: valid (%s)", network_detail);
  } else {
    br_server_logf("network config: missing/invalid (%s)", network_detail);
  }

  if (!br_server_run_self_check(&server, false)) {
    br_server_log("startup self-check has failures");
    if (self_check_strict) {
      br_server_log("strict mode enabled, aborting startup");
      return 1;
    }
  }

  /* Detect USB gadget connection BEFORE pairing tick, so USB mode can
   * skip AP fallback entirely. */
  bool force_usb_transport = false;
  bool mqtt_disabled = false;
  {
    const char *env_transport = getenv("BOARD_TRANSPORT");
    const char *env_mqtt_disabled = getenv("BOARD_RUNTIME_DISABLE_MQTT");
    force_usb_transport = env_transport && strcmp(env_transport, "usb") == 0;
    mqtt_disabled = env_mqtt_disabled &&
                    (strcmp(env_mqtt_disabled, "1") == 0 ||
                     strcmp(env_mqtt_disabled, "true") == 0 ||
                     strcmp(env_mqtt_disabled, "yes") == 0);
    if (force_usb_transport || mqtt_disabled) {
      server.transport_mode = BR_TRANSPORT_USB;
    } else if (env_transport && strcmp(env_transport, "mqtt") == 0) {
      server.transport_mode = BR_TRANSPORT_MQTT;
    } else {
      server.transport_mode = br_usb_gadget_connected() ? BR_TRANSPORT_USB : BR_TRANSPORT_MQTT;
    }
  }

  if (server.transport_mode == BR_TRANSPORT_USB) {
    const char *usb_dev = getenv("BOARD_USB_SERIAL_PATH");
    if (!usb_dev || !usb_dev[0]) {
      usb_dev = "/dev/ttyGS0";
    }
    br_server_logf("transport: USB serial (%s)", usb_dev);
    if (br_usb_serial_open(&server.usb_serial, usb_dev, 921600,
                           br_handle_usb_message, &server) != 0) {
      if (force_usb_transport || mqtt_disabled) {
        br_server_logf("USB serial open failed; MQTT fallback disabled");
        return 1;
      } else {
        br_server_logf("USB serial open failed, falling back to MQTT");
        server.transport_mode = BR_TRANSPORT_MQTT;
      }
    } else {
      /* In USB mode, set pairing directly to STA_READY (no WiFi needed) */
      server.pairing.network_config_valid = true;
      server.pairing.state = BR_PAIRING_STA_READY;
      snprintf(server.last_state, sizeof(server.last_state), "idle");
      server.last_reason[0] = '\0';
      server.last_speech_rewrite_ms = br_now_ms();
      /* Set up USB touch action file path */
      snprintf(server.usb_touch_action_path, sizeof(server.usb_touch_action_path),
               "%s/.usb-touch-action", server.config.root_dir);
    }
  }

  br_server_apply_pairing_runtime_state(&server, server.pairing.state);
  if (!server.ap_mode_active) {
    snprintf(server.last_state, sizeof(server.last_state), "idle");
    server.last_reason[0] = '\0';
    server.last_speech_rewrite_ms = br_now_ms();
  }
  /* Only run pairing tick (which may trigger AP fallback) in MQTT mode */
  if (server.transport_mode != BR_TRANSPORT_USB && br_pairing_is_waiting(server.pairing.state)) {
    br_pairing_state previous = server.pairing.state;
    if (br_pairing_tick(&server.pairing, br_now_ms())) {
      br_server_on_pairing_transition(&server, previous, server.pairing.state, "startup_discovery");
    }
  }

  if (server.transport_mode == BR_TRANSPORT_MQTT) {
    br_server_logf("transport: MQTT (%s)", server.config.mqtt_url);
    snprintf(client_id, sizeof(client_id), "board-c-%s-%d", server.config.local_device_id, (int) getpid());
    br_publish_online_payload(&server, false, will_payload, sizeof(will_payload));
    br_mqtt_client_init(&server.mqtt,
                        server.config.mqtt_url,
                        client_id,
                        server.config.mqtt_username,
                        server.config.mqtt_password,
                        server.availability_topic,
                        will_payload,
                        30,
                        br_handle_mqtt_publish,
                        &server);
  }

  server.listen_fd = br_server_open_listener(&server);
  if (server.listen_fd < 0) {
    fprintf(stderr, "failed to listen on %s:%d\n", server.config.http_host, server.config.http_port);
    return 1;
  }
  br_server_open_discovery_channels(&server);

  signal(SIGINT, br_server_request_shutdown);
  signal(SIGTERM, br_server_request_shutdown);
  g_server = &server;

  {
    long long tokens_per_lunch = 350000LL;
    int tz_offset_sec = 28800;
    const char *env_tpc = getenv("PET_CLAW_STATS_TOKENS_PER_LUNCH");
    if (!env_tpc || !*env_tpc) {
      env_tpc = getenv("PET_CLAW_STATS_TOKENS_PER_COFFEE");
    }
    if (env_tpc && *env_tpc) {
      long long v = strtoll(env_tpc, NULL, 10);
      if (v > 0) tokens_per_lunch = v;
    }
    const char *env_tz = getenv("PET_CLAW_STATS_TZ_OFFSET_SEC");
    if (env_tz && *env_tz) {
      tz_offset_sec = (int) strtol(env_tz, NULL, 10);
    }
    if (runtime_stats_init(server.config.root_dir, tokens_per_lunch, tz_offset_sec, br_now_ms()) == 0) {
      runtime_stats_flush();
      br_server_logf("runtime_stats: ready (tokens_per_lunch=%lld, tz_offset_sec=%d)",
                     tokens_per_lunch, tz_offset_sec);
    } else {
      br_server_logf("runtime_stats: init failed (root=%s)", server.config.root_dir);
    }
  }

  /* Initialize .screen-page to the pet home screen when absent or invalid. */
  {
    char existing[16] = "";
    if (!br_read_text_file(server.config.screen_page_path, existing, sizeof(existing)) ||
        (strncmp(existing, "main", 4) != 0 && strncmp(existing, "stats", 5) != 0)) {
      br_atomic_write_text(server.config.screen_page_path, br_screen_page_default_page());
    }
  }

  br_server_logf("HTTP/WebSocket listening: http://%s:%d", server.config.http_host, server.config.http_port);
  br_server_logf("local device: %s", server.config.local_device_id);
  br_server_logf("board device: %s", server.config.board_device_id);
  br_server_logf("target: %s %s",
                 server.config.target_device_id,
                 server.config.target_source[0] ? server.config.target_source : "state/+");

  while (!server.shutdown_requested) {
    fd_set read_set;
    int max_fd;
    struct timeval timeout;
    long long now_ms;

    if (server.transport_mode == BR_TRANSPORT_USB) {
      /* USB mode: poll serial instead of MQTT */
      if (server.usb_serial.connected) {
        if (br_usb_serial_poll(&server.usb_serial) < 0) {
          br_server_logf("USB serial disconnected");
          break;  /* Exit for procd respawn -> will re-detect mode */
        }
        /* Send hello every 3s until peer ack, then every 30s as heartbeat
           so a newly-connected host can discover the device without restart */
        now_ms = br_now_ms();
        {
          long long hello_interval = server.usb_serial.peer_acked ? 30000 : 3000;
          if ((now_ms - server.usb_serial.last_hello_ms) >= hello_interval) {
            br_usb_serial_send_hello(&server.usb_serial, server.config.board_device_id);
            server.usb_serial.last_hello_ms = now_ms;
          }
        }
        /* Poll for touch action files from board-touch-input */
        if ((now_ms - server.last_usb_touch_check_ms) >= 100) {
          br_server_poll_usb_touch(&server);
          server.last_usb_touch_check_ms = now_ms;
        }
      } else {
        br_server_logf("USB serial not connected, exiting");
        break;
      }
    } else {
      /* MQTT mode: existing logic */
      now_ms = br_now_ms();
      if ((now_ms - last_usb_hotplug_probe_ms) >= 1000) {
        last_usb_hotplug_probe_ms = now_ms;
        if (br_usb_gadget_connected()) {
          const char *usb_dev = getenv("BOARD_USB_SERIAL_PATH");
          if (!usb_dev || !usb_dev[0]) {
            usb_dev = "/dev/ttyGS0";
          }
          br_server_logf("USB hotplug detected in MQTT mode, trying switch (%s)", usb_dev);
          if (br_usb_serial_open(&server.usb_serial, usb_dev, 921600,
                                 br_handle_usb_message, &server) == 0) {
            if (server.mqtt.connected) {
              br_server_publish_offline(&server);
            }
            br_mqtt_client_close(&server.mqtt);
            server.mqtt_online = false;
            server.transport_mode = BR_TRANSPORT_USB;
            server.pairing.network_config_valid = true;
            snprintf(server.usb_touch_action_path, sizeof(server.usb_touch_action_path),
                     "%s/.usb-touch-action", server.config.root_dir);
            if (server.pairing.state != BR_PAIRING_STA_READY) {
              br_pairing_state previous = server.pairing.state;
              server.pairing.state = BR_PAIRING_STA_READY;
              br_server_on_pairing_transition(&server,
                                              previous,
                                              server.pairing.state,
                                              "usb_hotplug_switch");
            } else {
              br_server_apply_pairing_runtime_state(&server, server.pairing.state);
            }
            br_server_logf("switched transport: MQTT -> USB serial (%s)", usb_dev);
            continue;
          } else {
            br_server_logf("USB hotplug serial open failed (%s), keep MQTT", usb_dev);
          }
        }
      }

      if (!server.mqtt.connected && server.pairing.network_config_valid) {
        bool had_connection = server.mqtt_online;
        if (br_mqtt_client_ensure_connected(&server.mqtt, 3000) == 0) {
          server.mqtt_online = true;
          br_server_logf("MQTT connected: %s", server.config.mqtt_url);
          br_server_subscribe_topics(&server);
          br_server_publish_presence(&server, true);
        } else if (had_connection) {
          server.mqtt_online = false;
        }
      }

      if (server.mqtt.connected) {
        br_mqtt_client_poll(&server.mqtt, 0);
      }

      br_server_poll_discovery(&server);
      now_ms = br_now_ms();
      br_pairing_state previous = server.pairing.state;
      if (br_pairing_tick(&server.pairing, now_ms)) {
        br_server_on_pairing_transition(&server,
                                        previous,
                                        server.pairing.state,
                                        "pairing_tick");
      }
      br_server_discovery_announce(&server, now_ms);

      if ((now_ms - server.last_speech_rewrite_ms) >= 8000) {
        if (br_pairing_is_waiting(server.pairing.state) && server.pairing_message[0]) {
          br_atomic_write_text(server.config.current_speech_path, server.pairing_message);
        }
        server.last_speech_rewrite_ms = now_ms;
      }
    }

    /* Shared logic for both modes */
    br_server_tick_session_machine(&server);
    br_server_tick_speech_records(&server);

    now_ms = br_now_ms();
    if ((now_ms - server.last_stats_flush_ms) >= 5000) {
      runtime_stats_flush();
      server.last_stats_flush_ms = now_ms;
    }

    FD_ZERO(&read_set);
    FD_SET(server.listen_fd, &read_set);
    max_fd = server.listen_fd;
    /* USB serial: do NOT add fd to select — CDC-ACM gadget returns read()==0
       when host is connected but idle, which makes select() report readable
       continuously, causing a busy-loop. Instead, rely on timeout-based polling. */
    if (server.transport_mode == BR_TRANSPORT_USB && server.usb_serial.connected) {
      timeout.tv_sec = 0;
      timeout.tv_usec = 10000; /* 10ms in USB mode for fast polling */
    } else {
      timeout.tv_sec = 0;
      timeout.tv_usec = 200000; /* 200ms in MQTT mode */
    }
    if (select(max_fd + 1, &read_set, NULL, NULL, &timeout) > 0 && FD_ISSET(server.listen_fd, &read_set)) {
      int client_fd = accept(server.listen_fd, NULL, NULL);
      if (client_fd >= 0) {
        (void) fcntl(client_fd, F_SETFD, FD_CLOEXEC);
        bool keep_open = br_handle_http_connection(&server, client_fd);
        if (!keep_open) {
          close(client_fd);
        }
      }
    }

    br_server_poll_ws(&server);
  }

  runtime_stats_shutdown();

  if (server.transport_mode == BR_TRANSPORT_USB) {
    if (server.usb_serial.connected) {
      br_server_publish_offline(&server);
      br_usb_serial_close(&server.usb_serial);
    }
  } else {
    if (server.mqtt.connected) {
      br_server_publish_offline(&server);
    }
    br_server_set_ap_mode(&server, false);
    br_mqtt_client_close(&server.mqtt);
  }
  br_server_close_discovery_channels(&server);
  if (server.listen_fd >= 0) {
    close(server.listen_fd);
  }
  for (size_t i = 0; i < BR_MAX_WS_CLIENTS; i += 1) {
    br_ws_close(&server.ws_clients[i]);
  }
  br_server_log("shutdown");
  return 0;
}
