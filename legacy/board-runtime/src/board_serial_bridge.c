/*
 * USB serial bridge for board-server runtime sync.
 * Appearance asset activation updates the active clip directory, then emits
 * .clips-reload, .welcome-trigger, and .screen-interrupt so the board reloads
 * assets and replays welcome once before resuming the current session state.
 */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "runtime_common.h"
#include "runtime_json.h"
#include "runtime_protocol.h"
#include "runtime_session_state.h"

#define BR_SERIAL_MAX_SPEECH_RECORDS 4
#define BR_SERIAL_SPEECH_HOLD_MS 30000LL

typedef struct {
  bool active;
  char key[256];
  char source[128];
  char title[256];
  char body[768];
  char text[1024];
  long long updated_at_ms;
  long long expires_at_ms;
} br_serial_speech_record;

typedef struct {
  char root_dir[BR_MAX_PATH];
  char serial_port[BR_MAX_PATH];
  char board_device_id[128];
  char local_device_id[128];
  char namespace_id[64];
  char desktop_device_id[128];
  char current_state_path[BR_MAX_PATH];
  char current_event_path[BR_MAX_PATH];
  char current_speech_path[BR_MAX_PATH];
  char speech_hold_until_path[BR_MAX_PATH];
  char screen_interrupt_path[BR_MAX_PATH];
  char welcome_trigger_path[BR_MAX_PATH];
  char clips_reload_path[BR_MAX_PATH];
  char debug_session_path[BR_MAX_PATH];
  int fd;
  br_session_machine session_machine;
  br_serial_speech_record speech_records[BR_SERIAL_MAX_SPEECH_RECORDS];
  long long last_hello_ms;
  long long last_availability_ms;
} br_serial_bridge;

static volatile sig_atomic_t g_shutdown_requested = 0;

static void br_serial_signal(int signum) {
  (void) signum;
  g_shutdown_requested = 1;
}

static int br_serial_write_all(int fd, const char *text, size_t length) {
  while (length > 0) {
    ssize_t written = write(fd, text, length);
    if (written < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    text += written;
    length -= (size_t) written;
  }
  return 0;
}

static void br_serial_send_line(int fd, const char *line) {
  if (fd < 0 || !line) return;
  (void) br_serial_write_all(fd, line, strlen(line));
  (void) br_serial_write_all(fd, "\n", 1);
}

static void br_serial_send_asset_ack(
  br_serial_bridge *bridge,
  const char *transfer_id,
  const char *phase,
  bool ok,
  const char *error
) {
  char json[BR_MAX_JSON];
  size_t used = 0;
  if (!bridge) return;
  json[0] = '\0';
  br_snprintf_append(json, sizeof(json), &used, "{\"v\":1,\"type\":\"asset_ack\",\"transferId\":\"");
  br_json_escape_append(json, sizeof(json), &used, transfer_id ? transfer_id : "");
  br_snprintf_append(json, sizeof(json), &used, "\",\"phase\":\"");
  br_json_escape_append(json, sizeof(json), &used, phase ? phase : "");
  br_snprintf_append(json, sizeof(json), &used, "\",\"ok\":%s", ok ? "true" : "false");
  if (error && error[0]) {
    br_snprintf_append(json, sizeof(json), &used, ",\"error\":\"");
    br_json_escape_append(json, sizeof(json), &used, error);
    br_snprintf_append(json, sizeof(json), &used, "\"");
  }
  br_snprintf_append(json, sizeof(json), &used, "}");
  br_serial_send_line(bridge->fd, json);
}

static void br_serial_send_payload_ack(
  br_serial_bridge *bridge,
  bool ok,
  const char *error
) {
  char json[256];
  size_t used = 0;
  if (!bridge) return;
  json[0] = '\0';
  br_snprintf_append(json, sizeof(json), &used,
    "{\"v\":1,\"type\":\"payload_ack\",\"ok\":%s",
    ok ? "true" : "false");
  if (!ok && error && *error) {
    br_snprintf_append(json, sizeof(json), &used, ",\"error\":\"");
    br_json_escape_append(json, sizeof(json), &used, error);
    br_snprintf_append(json, sizeof(json), &used, "\"");
  }
  br_snprintf_append(json, sizeof(json), &used, "}");
  br_serial_send_line(bridge->fd, json);
}

static bool br_serial_get_json_string(
  const char *json,
  const br_json_token *tokens,
  int count,
  const char *key,
  char *output,
  size_t output_size
) {
  int index = br_json_find_key(json, tokens, count, 0, key);
  if (index < 0) {
    if (output_size > 0) output[0] = '\0';
    return false;
  }
  return br_json_token_to_string(json, &tokens[index], output, output_size);
}

static int br_serial_get_json_int(
  const char *json,
  const br_json_token *tokens,
  int count,
  const char *key,
  int fallback
) {
  int index = br_json_find_key(json, tokens, count, 0, key);
  char text[64];
  if (index < 0) return fallback;
  if (!br_json_token_to_string(json, &tokens[index], text, sizeof(text)) &&
      !br_json_copy_raw(json, &tokens[index], text, sizeof(text))) {
    return fallback;
  }
  return atoi(text);
}

static bool br_serial_safe_asset_path(const char *relative) {
  const char *p;
  if (!relative || !relative[0]) return false;
  if (relative[0] == '/' || strstr(relative, "..") || strchr(relative, '\\')) return false;
  for (p = relative; *p; p += 1) {
    char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' || ch == '/') {
      continue;
    }
    return false;
  }
  return true;
}

static int br_serial_base64_value(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  if (ch == '=') return -2;
  return -1;
}

static int br_serial_base64_decode(const char *input, unsigned char *output, size_t output_size) {
  int value = 0;
  int value_bits = -8;
  size_t used = 0;
  for (; input && *input; input += 1) {
    int decoded = br_serial_base64_value(*input);
    if (decoded == -2) break;
    if (decoded < 0) return -1;
    value = (value << 6) | decoded;
    value_bits += 6;
    if (value_bits >= 0) {
      if (used >= output_size) return -1;
      output[used++] = (unsigned char) ((value >> value_bits) & 0xff);
      value_bits -= 8;
    }
  }
  return (int) used;
}

static int br_serial_remove_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return errno == ENOENT ? 0 : -1;
  }
  if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    if (!dir) return -1;
    while ((entry = readdir(dir)) != NULL) {
      char child[BR_MAX_PATH];
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
      snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
      if (br_serial_remove_tree(child) != 0) {
        closedir(dir);
        return -1;
      }
    }
    closedir(dir);
    return rmdir(path);
  }
  return unlink(path);
}

static int br_serial_mkdir_p(const char *path) {
  char temp[BR_MAX_PATH];
  char *p;
  br_normalize_text(path, "", temp, sizeof(temp));
  if (!temp[0]) return -1;
  for (p = temp + 1; *p; p += 1) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(temp, 0755) != 0 && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if (mkdir(temp, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
}

static int br_serial_ensure_parent_dir(const char *path) {
  char dir[BR_MAX_PATH];
  char *slash;
  br_normalize_text(path, "", dir, sizeof(dir));
  slash = strrchr(dir, '/');
  if (!slash) return 0;
  *slash = '\0';
  return br_serial_mkdir_p(dir);
}

static speed_t br_serial_baud(int baud) {
  switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return B115200;
  }
}

static int br_serial_open_port(const char *path, int baud) {
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  struct termios tty;
  speed_t speed = br_serial_baud(baud);
  if (fd < 0) return -1;
  if (tcgetattr(fd, &tty) != 0) {
    close(fd);
    return -1;
  }
  cfmakeraw(&tty);
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 1;
  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    close(fd);
    return -1;
  }
  tcflush(fd, TCIOFLUSH);
  return fd;
}

static bool br_serial_extract_payload(
  const char *line,
  char *topic,
  size_t topic_size,
  char *payload,
  size_t payload_size,
  char *type,
  size_t type_size
) {
  br_json_token tokens[512];
  int count = br_json_parse(line, strlen(line), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  int index;
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) return false;

  type[0] = '\0';
  index = br_json_find_key(line, tokens, count, 0, "type");
  if (index >= 0) {
    (void) br_json_token_to_string(line, &tokens[index], type, type_size);
  }

  topic[0] = '\0';
  index = br_json_find_key(line, tokens, count, 0, "topic");
  if (index >= 0) {
    (void) br_json_token_to_string(line, &tokens[index], topic, topic_size);
  }

  payload[0] = '\0';
  index = br_json_find_key(line, tokens, count, 0, "payload");
  if (index >= 0) {
    (void) br_json_copy_raw(line, &tokens[index], payload, payload_size);
  }
  return true;
}

static void br_serial_write_session_debug(
  br_serial_bridge *bridge,
  const br_session_resolution *resolution
) {
  char json[BR_MAX_JSON];
  if (!bridge || !resolution) return;
  br_session_machine_debug_json(&bridge->session_machine, resolution, json, sizeof(json));
  (void) br_atomic_write_text(bridge->debug_session_path, json);
}

static void br_serial_speech_key(const br_speech_update *update, char *output, size_t output_size) {
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

static br_serial_speech_record *br_serial_speech_slot(br_serial_bridge *bridge, const char *key) {
  int empty_index = -1;
  int oldest_index = 0;
  for (int i = 0; i < BR_SERIAL_MAX_SPEECH_RECORDS; i += 1) {
    br_serial_speech_record *record = &bridge->speech_records[i];
    if (record->active && strcmp(record->key, key) == 0) return record;
    if (!record->active && empty_index < 0) empty_index = i;
    if (record->updated_at_ms < bridge->speech_records[oldest_index].updated_at_ms) oldest_index = i;
  }
  return empty_index >= 0 ? &bridge->speech_records[empty_index] : &bridge->speech_records[oldest_index];
}

static void br_serial_speech_normalize_body_for_match(
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

static br_serial_speech_record *br_serial_duplicate_speech_slot(
  br_serial_bridge *bridge,
  const br_speech_update *update,
  const char *key
) {
  const char *source;
  const char *body;
  char match_body[1024];
  if (!bridge || !update) return NULL;
  source = update->source[0] ? update->source : "unknown";
  body = update->body[0] ? update->body : update->text;
  if (!body[0]) return NULL;
  br_serial_speech_normalize_body_for_match(body, match_body, sizeof(match_body));
  for (int i = 0; i < BR_SERIAL_MAX_SPEECH_RECORDS; i += 1) {
    br_serial_speech_record *record = &bridge->speech_records[i];
    const char *record_body;
    char record_match_body[1024];
    if (!record->active) continue;
    if (key && record->key[0] && strcmp(record->key, key) == 0) return record;
    if (record->source[0] && strcmp(record->source, source) != 0) continue;
    record_body = record->body[0] ? record->body : record->text;
    if (record_body[0] && strcmp(record_body, body) == 0) return record;
    br_serial_speech_normalize_body_for_match(record_body, record_match_body, sizeof(record_match_body));
    if (match_body[0] && record_match_body[0] && strcmp(record_match_body, match_body) == 0) return record;
  }
  return NULL;
}

static bool br_serial_cleanup_speech(br_serial_bridge *bridge, long long now_ms) {
  bool changed = false;
  if (!bridge) return false;
  for (int i = 0; i < BR_SERIAL_MAX_SPEECH_RECORDS; i += 1) {
    br_serial_speech_record *record = &bridge->speech_records[i];
    if (record->active && record->expires_at_ms > 0 && now_ms >= record->expires_at_ms) {
      memset(record, 0, sizeof(*record));
      changed = true;
    }
  }
  return changed;
}

static long long br_serial_latest_speech_expiry_ms(br_serial_bridge *bridge) {
  long long latest = 0;
  if (!bridge) return 0;
  for (int i = 0; i < BR_SERIAL_MAX_SPEECH_RECORDS; i += 1) {
    br_serial_speech_record *record = &bridge->speech_records[i];
    if (record->active && record->expires_at_ms > latest) {
      latest = record->expires_at_ms;
    }
  }
  return latest;
}

static void br_serial_write_speech_hold_until_abs(br_serial_bridge *bridge, long long until_ms) {
  char hold_until[64];
  if (!bridge) return;
  if (until_ms <= 0) {
    (void) br_atomic_write_text(bridge->speech_hold_until_path, "");
    return;
  }
  snprintf(hold_until, sizeof(hold_until), "%lld\n", until_ms);
  (void) br_atomic_write_text(bridge->speech_hold_until_path, hold_until);
}

static bool br_serial_render_speech(br_serial_bridge *bridge, char *output, size_t output_size, long long now_ms) {
  bool used_indices[BR_SERIAL_MAX_SPEECH_RECORDS] = {0};
  size_t used = 0;
  int appended = 0;
  output[0] = '\0';
  br_serial_cleanup_speech(bridge, now_ms);
  for (;;) {
    int best = -1;
    for (int i = 0; i < BR_SERIAL_MAX_SPEECH_RECORDS; i += 1) {
      br_serial_speech_record *record = &bridge->speech_records[i];
      if (used_indices[i] || !record->active) continue;
      if (best < 0 || record->updated_at_ms > bridge->speech_records[best].updated_at_ms) best = i;
    }
    if (best < 0) break;
    used_indices[best] = true;
    br_serial_speech_record *record = &bridge->speech_records[best];
    char line[512];
    const char *title = record->title[0] ? record->title : (record->source[0] ? record->source : "session");
    const char *body = record->body[0] ? record->body : record->text;
    if (record->title[0] && body[0] && strcmp(record->title, body) != 0) {
      snprintf(line, sizeof(line), "%s: %s", title, body);
    } else {
      snprintf(line, sizeof(line), "%s", body[0] ? body : title);
    }
    for (char *cursor = line; *cursor; cursor += 1) {
      if (*cursor == '\r' || *cursor == '\n') *cursor = ' ';
    }
    if (line[0]) {
      if (used > 0) br_snprintf_append(output, output_size, &used, "\n");
      br_snprintf_append(output, output_size, &used, "%s", line);
      appended += 1;
    }
  }
  return appended > 0 && output[0] != '\0';
}

static bool br_serial_apply_speech(
  br_serial_bridge *bridge,
  const br_speech_update *update,
  char *rendered,
  size_t rendered_size
) {
  char key[256];
  long long now = br_now_ms();
  br_serial_speech_record *record;
  if (!bridge || !update || !update->text[0]) return false;
  br_serial_speech_key(update, key, sizeof(key));
  record = br_serial_duplicate_speech_slot(bridge, update, key);
  if (!record) {
    record = br_serial_speech_slot(bridge, key);
  }
  memset(record, 0, sizeof(*record));
  record->active = true;
  snprintf(record->key, sizeof(record->key), "%s", key);
  snprintf(record->source, sizeof(record->source), "%s", update->source[0] ? update->source : "unknown");
  snprintf(record->title, sizeof(record->title), "%s", update->title);
  snprintf(record->body, sizeof(record->body), "%s", update->body);
  snprintf(record->text, sizeof(record->text), "%s", update->text);
  record->updated_at_ms = update->has_payload_ts_ms && update->payload_ts_ms > 0 ? update->payload_ts_ms : now;
  record->expires_at_ms = update->has_expires_at_ms && update->expires_at_ms > now
    ? update->expires_at_ms
    : now + BR_SERIAL_SPEECH_HOLD_MS;
  return br_serial_render_speech(bridge, rendered, rendered_size, now);
}

static void br_serial_handle_publish(br_serial_bridge *bridge, const char *topic, const char *payload) {
  br_bridge_state_update update;
  if (!bridge || !topic || !payload) return;

  if (strstr(topic, "/speech/text")) {
    br_speech_update speech_update;
    char speech[2048];
    if (br_parse_speech_update(payload, &speech_update) &&
        br_serial_apply_speech(bridge, &speech_update, speech, sizeof(speech))) {
      char marker[128];
      (void) br_atomic_write_text(bridge->current_speech_path, speech);
      br_serial_write_speech_hold_until_abs(bridge, br_serial_latest_speech_expiry_ms(bridge));
      snprintf(marker, sizeof(marker), "%lld speech\n", br_now_ms());
      (void) br_atomic_write_text(bridge->screen_interrupt_path, marker);
    }
    return;
  }

  if (strstr(topic, "/state/") && br_bridge_state_from_message(topic, payload, &update)) {
    br_session_resolution resolution;
    if (update.should_write &&
        br_session_machine_apply(&bridge->session_machine, &update, br_now_ms(), &resolution)) {
      (void) br_atomic_write_text(bridge->current_state_path, resolution.state);
      (void) br_atomic_write_text(bridge->current_event_path, resolution.event);
      br_serial_write_session_debug(bridge, &resolution);
      if (resolution.should_interrupt) {
        char marker[128];
        snprintf(marker, sizeof(marker), "%lld %s\n", br_now_ms(), resolution.state);
        (void) br_atomic_write_text(bridge->screen_interrupt_path, marker);
      }
    }
  }
}

static void br_serial_tick_speech(br_serial_bridge *bridge) {
  char speech[2048];
  long long now = br_now_ms();
  if (!bridge || !br_serial_cleanup_speech(bridge, now)) return;
  if (br_serial_render_speech(bridge, speech, sizeof(speech), now)) {
    (void) br_atomic_write_text(bridge->current_speech_path, speech);
    br_serial_write_speech_hold_until_abs(bridge, br_serial_latest_speech_expiry_ms(bridge));
  } else {
    (void) br_atomic_write_text(bridge->current_speech_path, "");
    br_serial_write_speech_hold_until_abs(bridge, 0);
  }
}

static void br_serial_handle_asset_begin(
  br_serial_bridge *bridge,
  const char *line,
  const br_json_token *tokens,
  int count
) {
  char transfer_id[128];
  char staging[BR_MAX_PATH];
  br_serial_get_json_string(line, tokens, count, "transferId", transfer_id, sizeof(transfer_id));
  snprintf(staging, sizeof(staging), "%s/.incoming-desktop-pet", bridge->root_dir);
  if (!transfer_id[0]) {
    br_serial_send_asset_ack(bridge, "", "begin", false, "missing transferId");
    return;
  }
  if (br_serial_remove_tree(staging) != 0 || br_serial_mkdir_p(staging) != 0) {
    br_serial_send_asset_ack(bridge, transfer_id, "begin", false, "cannot prepare staging directory");
    return;
  }
  br_serial_send_asset_ack(bridge, transfer_id, "begin", true, "");
}

static void br_serial_handle_asset_chunk(
  br_serial_bridge *bridge,
  const char *line,
  const br_json_token *tokens,
  int count
) {
  char transfer_id[128];
  char relative[BR_MAX_PATH];
  char data[6144];
  char staging[BR_MAX_PATH];
  char target[BR_MAX_PATH];
  unsigned char decoded[4096];
  int decoded_length;
  int index;
  int flags;
  int fd;

  br_serial_get_json_string(line, tokens, count, "transferId", transfer_id, sizeof(transfer_id));
  br_serial_get_json_string(line, tokens, count, "path", relative, sizeof(relative));
  br_serial_get_json_string(line, tokens, count, "data", data, sizeof(data));
  index = br_serial_get_json_int(line, tokens, count, "index", 0);

  if (!transfer_id[0] || !br_serial_safe_asset_path(relative) || !data[0]) {
    br_serial_send_asset_ack(bridge, transfer_id, "chunk", false, "invalid asset chunk");
    return;
  }
  decoded_length = br_serial_base64_decode(data, decoded, sizeof(decoded));
  if (decoded_length < 0) {
    br_serial_send_asset_ack(bridge, transfer_id, "chunk", false, "invalid base64 chunk");
    return;
  }

  snprintf(staging, sizeof(staging), "%s/.incoming-desktop-pet", bridge->root_dir);
  snprintf(target, sizeof(target), "%s/%s", staging, relative);
  if (br_serial_ensure_parent_dir(target) != 0) {
    br_serial_send_asset_ack(bridge, transfer_id, "chunk", false, "cannot create asset directory");
    return;
  }

  flags = O_CREAT | O_WRONLY;
  flags |= index == 0 ? O_TRUNC : O_APPEND;
  fd = open(target, flags, 0644);
  if (fd < 0) {
    br_serial_send_asset_ack(bridge, transfer_id, "chunk", false, "cannot open asset file");
    return;
  }
  if (br_serial_write_all(fd, (const char *) decoded, (size_t) decoded_length) != 0) {
    close(fd);
    br_serial_send_asset_ack(bridge, transfer_id, "chunk", false, "cannot write asset file");
    return;
  }
  close(fd);
  br_serial_send_asset_ack(bridge, transfer_id, "chunk", true, "");
}

static void br_serial_handle_asset_commit(
  br_serial_bridge *bridge,
  const char *line,
  const br_json_token *tokens,
  int count
) {
  char transfer_id[128];
  char marker[128];
  char staging[BR_MAX_PATH];
  char current[BR_MAX_PATH];
  char previous[BR_MAX_PATH];
  char clips[BR_MAX_PATH];
  char clips_previous[BR_MAX_PATH];
  char current_videos[BR_MAX_PATH];

  br_serial_get_json_string(line, tokens, count, "transferId", transfer_id, sizeof(transfer_id));
  if (!transfer_id[0]) {
    br_serial_send_asset_ack(bridge, "", "commit", false, "missing transferId");
    return;
  }

  snprintf(staging, sizeof(staging), "%s/.incoming-desktop-pet", bridge->root_dir);
  snprintf(current, sizeof(current), "%s/.desktop-pet-current", bridge->root_dir);
  snprintf(previous, sizeof(previous), "%s/.desktop-pet-previous", bridge->root_dir);
  snprintf(clips, sizeof(clips), "%s/terrier-clips", bridge->root_dir);
  snprintf(clips_previous, sizeof(clips_previous), "%s/terrier-clips.previous", bridge->root_dir);
  snprintf(current_videos, sizeof(current_videos), "%s/videos", current);

  if (access(staging, R_OK) != 0) {
    br_serial_send_asset_ack(bridge, transfer_id, "commit", false, "staging directory missing");
    return;
  }

  (void) br_serial_remove_tree(previous);
  if (access(current, F_OK) == 0 && rename(current, previous) != 0) {
    br_serial_send_asset_ack(bridge, transfer_id, "commit", false, "cannot rotate current assets");
    return;
  }
  if (rename(staging, current) != 0) {
    br_serial_send_asset_ack(bridge, transfer_id, "commit", false, "cannot activate assets");
    return;
  }

  (void) br_serial_remove_tree(clips_previous);
  if (access(clips, F_OK) == 0) {
    (void) rename(clips, clips_previous);
  }
  if (symlink(current_videos, clips) != 0) {
    br_serial_send_asset_ack(bridge, transfer_id, "commit", false, "cannot point clip directory at assets");
    return;
  }

  snprintf(marker, sizeof(marker), "%lld assets\n", br_now_ms());
  (void) br_atomic_write_text(bridge->clips_reload_path, marker);
  (void) br_atomic_write_text(bridge->welcome_trigger_path, marker);
  (void) br_atomic_write_text(bridge->screen_interrupt_path, marker);
  br_serial_send_asset_ack(bridge, transfer_id, "commit", true, "");
}

static void br_serial_send_hello(br_serial_bridge *bridge) {
  char json[BR_MAX_JSON];
  char ts[64];
  size_t used = 0;
  if (!bridge) return;
  br_iso8601_now(ts, sizeof(ts));
  json[0] = '\0';
  br_snprintf_append(json, sizeof(json), &used, "{\"v\":1,\"type\":\"hello_ack\",\"transport\":\"usb-serial\",\"boardDeviceId\":\"");
  br_json_escape_append(json, sizeof(json), &used, bridge->board_device_id);
  br_snprintf_append(json, sizeof(json), &used, "\",\"localDeviceId\":\"");
  br_json_escape_append(json, sizeof(json), &used, bridge->local_device_id);
  br_snprintf_append(json, sizeof(json), &used, "\",\"namespace\":\"");
  br_json_escape_append(json, sizeof(json), &used, bridge->namespace_id);
  br_snprintf_append(json, sizeof(json), &used, "\",\"desktopDeviceId\":\"");
  br_json_escape_append(json, sizeof(json), &used, bridge->desktop_device_id);
  br_snprintf_append(json, sizeof(json), &used, "\",\"fw\":\"0.1.0\",\"ts\":\"");
  br_json_escape_append(json, sizeof(json), &used, ts);
  br_snprintf_append(json, sizeof(json), &used, "\",\"tsMs\":%lld}", br_now_ms());
  br_serial_send_line(bridge->fd, json);
}

static void br_serial_send_availability(br_serial_bridge *bridge, bool online) {
  char json[BR_MAX_JSON];
  char ts[64];
  size_t used = 0;
  if (!bridge) return;
  br_iso8601_now(ts, sizeof(ts));
  json[0] = '\0';
  br_snprintf_append(json, sizeof(json), &used, "{\"v\":1,\"type\":\"availability\",\"transport\":\"usb-serial\",\"boardDeviceId\":\"");
  br_json_escape_append(json, sizeof(json), &used, bridge->board_device_id);
  br_snprintf_append(json, sizeof(json), &used, "\",\"online\":%s,\"ts\":\"", online ? "true" : "false");
  br_json_escape_append(json, sizeof(json), &used, ts);
  br_snprintf_append(json, sizeof(json), &used, "\",\"tsMs\":%lld}", br_now_ms());
  br_serial_send_line(bridge->fd, json);
}

static void br_serial_handle_line(br_serial_bridge *bridge, const char *line) {
  char topic[BR_MAX_TOPIC];
  char payload[BR_MAX_JSON];
  char type[64];
  br_json_token tokens[512];
  int count;
  int index;

  if (!bridge || !line || line[0] == '\0') return;
  if (!br_serial_extract_payload(line, topic, sizeof(topic), payload, sizeof(payload), type, sizeof(type))) {
    return;
  }
  count = br_json_parse(line, strlen(line), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) {
    return;
  }

  if (strcmp(type, "hello") == 0 || strcmp(type, "bind") == 0) {
    index = br_json_find_key(line, tokens, count, 0, "desktopDeviceId");
    if (index >= 0) {
      (void) br_json_token_to_string(line, &tokens[index], bridge->desktop_device_id, sizeof(bridge->desktop_device_id));
    }
    index = br_json_find_key(line, tokens, count, 0, "namespace");
    if (index >= 0) {
      (void) br_json_token_to_string(line, &tokens[index], bridge->namespace_id, sizeof(bridge->namespace_id));
    }
    br_serial_send_hello(bridge);
    br_serial_send_availability(bridge, true);
    return;
  }

  if (strcmp(type, "asset_begin") == 0) {
    br_serial_handle_asset_begin(bridge, line, tokens, count);
    return;
  }

  if (strcmp(type, "asset_chunk") == 0) {
    br_serial_handle_asset_chunk(bridge, line, tokens, count);
    return;
  }

  if (strcmp(type, "asset_commit") == 0) {
    br_serial_handle_asset_commit(bridge, line, tokens, count);
    return;
  }

  if (strcmp(type, "payload_write") == 0) {
    int path_idx = br_json_find_key(line, tokens, count, 0, "path");
    int content_idx = br_json_find_key(line, tokens, count, 0, "content");
    if (path_idx < 0 || content_idx < 0) {
      br_serial_send_payload_ack(bridge, false, "missing path or content");
      return;
    }
    char path_arg[64];
    char content[2048];
    if (!br_json_token_to_string(line, &tokens[path_idx], path_arg, sizeof(path_arg))) {
      br_serial_send_payload_ack(bridge, false, "invalid path");
      return;
    }
    if (!br_json_token_to_string(line, &tokens[content_idx], content, sizeof(content))) {
      br_serial_send_payload_ack(bridge, false, "invalid content");
      return;
    }
    const char *err = NULL;
    if (!br_apply_payload_write(bridge->root_dir, path_arg, content, &err)) {
      br_serial_send_payload_ack(bridge, false, err ? err : "write failed");
      return;
    }
    br_serial_send_payload_ack(bridge, true, "");
    return;
  }

  if (strcmp(type, "publish") == 0 && topic[0] && payload[0]) {
    br_serial_handle_publish(bridge, topic, payload);
  }
}

static void br_serial_load_config(br_serial_bridge *bridge, const char *root_dir) {
  char hostname[128];
  memset(bridge, 0, sizeof(*bridge));
  bridge->fd = -1;
  br_normalize_text(root_dir, ".", bridge->root_dir, sizeof(bridge->root_dir));
  br_normalize_text(getenv("BOARD_SERIAL_PORT"), "/dev/ttyGS0", bridge->serial_port, sizeof(bridge->serial_port));
  br_normalize_topic_part(getenv("PET_CLAW_MQTT_NAMESPACE"), "desk", bridge->namespace_id, sizeof(bridge->namespace_id));
  br_normalize_topic_part(getenv("PET_CLAW_TARGET_DEVICE_ID"), "", bridge->desktop_device_id, sizeof(bridge->desktop_device_id));

  {
    char device_config_path[BR_MAX_PATH];
    snprintf(device_config_path, sizeof(device_config_path), "%s/device-config.json", bridge->root_dir);
    if (!br_read_device_id_json(device_config_path, bridge->local_device_id, sizeof(bridge->local_device_id))) {
      if (br_get_hostname_text(hostname, sizeof(hostname))) {
        br_normalize_topic_part(hostname, "linux-pet-01", bridge->local_device_id, sizeof(bridge->local_device_id));
      } else {
        br_normalize_topic_part("linux-pet-01", "linux-pet-01", bridge->local_device_id, sizeof(bridge->local_device_id));
      }
    }
  }
  br_normalize_text(bridge->local_device_id, "linux-pet-01", bridge->board_device_id, sizeof(bridge->board_device_id));

  snprintf(bridge->current_state_path, sizeof(bridge->current_state_path), "%s/.current-state", bridge->root_dir);
  snprintf(bridge->current_event_path, sizeof(bridge->current_event_path), "%s/.current-event", bridge->root_dir);
  snprintf(bridge->current_speech_path, sizeof(bridge->current_speech_path), "%s/.current-speech", bridge->root_dir);
  snprintf(bridge->speech_hold_until_path, sizeof(bridge->speech_hold_until_path), "%s/.current-speech-hold-until", bridge->root_dir);
  snprintf(bridge->screen_interrupt_path, sizeof(bridge->screen_interrupt_path), "%s/.screen-interrupt", bridge->root_dir);
  snprintf(bridge->welcome_trigger_path, sizeof(bridge->welcome_trigger_path), "%s/.welcome-trigger", bridge->root_dir);
  snprintf(bridge->clips_reload_path, sizeof(bridge->clips_reload_path), "%s/.clips-reload", bridge->root_dir);
  snprintf(bridge->debug_session_path, sizeof(bridge->debug_session_path), "%s/.debug-session-state.json", bridge->root_dir);
  br_session_machine_init(&bridge->session_machine, 3000, 300000);
}

int main(int argc, char **argv) {
  br_serial_bridge bridge;
  char root_dir[BR_MAX_PATH];
  int baud = getenv("BOARD_SERIAL_BAUD") ? atoi(getenv("BOARD_SERIAL_BAUD")) : 115200;
  char line[BR_MAX_JSON];
  size_t line_used = 0;

  br_normalize_text(argc > 1 ? argv[1] : ".", ".", root_dir, sizeof(root_dir));
  br_serial_load_config(&bridge, root_dir);

  signal(SIGINT, br_serial_signal);
  signal(SIGTERM, br_serial_signal);

  while (!g_shutdown_requested) {
    if (bridge.fd < 0) {
      bridge.fd = br_serial_open_port(bridge.serial_port, baud);
      if (bridge.fd < 0) {
        fprintf(stderr, "[board-serial-bridge] open %s failed: %s\n", bridge.serial_port, strerror(errno));
        br_sleep_ms(1000);
        continue;
      }
      fprintf(stderr, "[board-serial-bridge] listening on %s @ %d\n", bridge.serial_port, baud);
      br_serial_send_hello(&bridge);
      br_serial_send_availability(&bridge, true);
      bridge.last_hello_ms = br_now_ms();
      bridge.last_availability_ms = br_now_ms();
      line_used = 0;
    }

    fd_set read_set;
    struct timeval timeout;
    FD_ZERO(&read_set);
    FD_SET(bridge.fd, &read_set);
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    if (select(bridge.fd + 1, &read_set, NULL, NULL, &timeout) > 0 && FD_ISSET(bridge.fd, &read_set)) {
      char buffer[512];
      ssize_t n = read(bridge.fd, buffer, sizeof(buffer));
      if (n > 0) {
        for (ssize_t i = 0; i < n; i += 1) {
          char ch = buffer[i];
          if (ch == '\r') continue;
          if (ch == '\n') {
            line[line_used] = '\0';
            br_serial_handle_line(&bridge, line);
            line_used = 0;
          } else if (line_used + 1 < sizeof(line)) {
            line[line_used++] = ch;
          } else {
            line_used = 0;
          }
        }
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close(bridge.fd);
        bridge.fd = -1;
      }
    }

    br_session_resolution resolution;
    if (br_session_machine_tick(&bridge.session_machine, br_now_ms(), &resolution)) {
      (void) br_atomic_write_text(bridge.current_state_path, resolution.state);
      (void) br_atomic_write_text(bridge.current_event_path, resolution.event);
      br_serial_write_session_debug(&bridge, &resolution);
    }
    br_serial_tick_speech(&bridge);

    if (bridge.fd >= 0 && br_now_ms() - bridge.last_availability_ms >= 3000) {
      br_serial_send_availability(&bridge, true);
      bridge.last_availability_ms = br_now_ms();
    }
  }

  if (bridge.fd >= 0) {
    br_serial_send_availability(&bridge, false);
    close(bridge.fd);
  }
  return 0;
}
