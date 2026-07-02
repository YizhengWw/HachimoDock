/*
 * board_asset_ota.c — appearance (asset) and widget OTA transfer.
 *
 * Extracted from board_server.c: the desktop client streams a directory of
 * files as newline-terminated base64 chunks, the board stages them, and commit
 * atomically rotates the previous install away and activates the staged tree.
 * Both channels share the transfer mechanics (see br_ota_ helpers); they keep
 * separate ack envelopes because they speak different topics/fields.
 *
 * Depends only on board_server_state.h (br_server_state + br_server_logf) and
 * the runtime_ common library — no other board_server.c internals.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_asset_ota.h"
#include "runtime_common.h"
#include "runtime_json.h"
#include "runtime_usb_serial.h"

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

int br_asset_write_all(int fd, const char *data, size_t len) {
  while (len > 0) {
    ssize_t w = write(fd, data, len);
    if (w < 0) { if (errno == EINTR) continue; return -1; }
    data += w; len -= (size_t)w;
  }
  return 0;
}

int br_asset_remove_tree(const char *path) {
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

int br_asset_mkdir_p(const char *path) {
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

bool br_asset_is_audio_patch_path(const char *rel) {
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
int br_asset_decode_b64_file(const char *b64_path, const char *outpath);
int br_asset_file_stats_checksum(
  const char *path,
  unsigned long long *size_out,
  char checksum_hex[17]
);
int br_asset_tree_stats(
  const char *dir_path,
  unsigned long long *file_count,
  unsigned long long *byte_count,
  bool *has_b64
);
int br_asset_remove_tree(const char *path);
int br_asset_mkdir_p(const char *path);
static int br_asset_ensure_parent(const char *path);
static bool br_asset_safe_path(const char *p);
bool br_asset_is_audio_patch_path(const char *p);
int br_asset_write_all(int fd, const char *buf, size_t len);

/* ─── shared OTA transfer helpers ───
   The appearance (asset) and widget OTA channels stream files the
   same way: begin prepares a clean staging dir, each chunk appends newline-
   terminated base64 into <staging>/<path>.b64, and commit rotates the previous
   install away before activating the staged tree. These helpers hold that
   shared mechanic once. The handlers keep their own JSON envelopes and ack
   functions because the two channels speak different ack topics/fields.
   Helpers return NULL on success or a short error string for the caller's ack. */

const char *br_ota_prepare_staging(br_server_state *server, const char *staging_name) {
  char staging[BR_MAX_PATH];
  snprintf(staging, sizeof(staging), "%s/%s", server->config.root_dir, staging_name);
  if (br_asset_remove_tree(staging) != 0 || br_asset_mkdir_p(staging) != 0) {
    return "cannot prepare staging";
  }
  return NULL;
}

/* Stream one base64 chunk into <staging>/<path>.b64 (truncate on index 0,
   append otherwise), newline-terminated for line-by-line decode at commit.
   Decode is deferred to commit so the main loop stays responsive. Fills tid
   for the caller's ack (left empty on bad json). */
const char *br_ota_stream_chunk(
  br_server_state *server,
  const char *staging_name,
  const char *payload,
  char *tid,
  size_t tid_size
) {
  br_json_token tokens[32]; char rel[BR_MAX_PATH];
  static char data[90000];
  char staging[BR_MAX_PATH], target[BR_MAX_PATH];
  int index_val, flags, fd;
  size_t data_len;

  tid[0] = '\0';
  int count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) return "bad json";

  int ti = br_json_find_key(payload, tokens, count, 0, "transferId");
  int pi = br_json_find_key(payload, tokens, count, 0, "path");
  int di = br_json_find_key(payload, tokens, count, 0, "data");
  int ii = br_json_find_key(payload, tokens, count, 0, "index");

  rel[0] = data[0] = '\0';
  if (ti >= 0) br_json_token_to_string(payload, &tokens[ti], tid, tid_size);
  if (pi >= 0) br_json_token_to_string(payload, &tokens[pi], rel, sizeof(rel));
  if (di >= 0) br_json_token_to_string(payload, &tokens[di], data, sizeof(data));

  char idx_str[32] = "0";
  if (ii >= 0) br_json_token_to_string(payload, &tokens[ii], idx_str, sizeof(idx_str));
  index_val = atoi(idx_str);

  if (!tid[0] || !br_asset_safe_path(rel) || !data[0]) return "invalid chunk";

  data_len = strlen(data);
  snprintf(staging, sizeof(staging), "%s/%s", server->config.root_dir, staging_name);
  snprintf(target, sizeof(target), "%s/%s.b64", staging, rel);
  if (br_asset_ensure_parent(target) != 0) return "mkdir failed";
  flags = O_CREAT | O_WRONLY | (index_val == 0 ? O_TRUNC : O_APPEND);
  fd = open(target, flags, 0644);
  if (fd < 0) return "open failed";
  if (br_asset_write_all(fd, data, data_len) != 0 ||
      br_asset_write_all(fd, "\n", 1) != 0) {
    close(fd);
    return "write failed";
  }
  close(fd);
  return NULL;
}

/* Rotate target → previous (best-effort clearing the stale previous first)
   and activate staging as the new target. */
const char *br_ota_rotate_activate(const char *staging, const char *target, const char *previous) {
  (void) br_asset_remove_tree(previous);
  if (access(target, F_OK) == 0 && rename(target, previous) != 0) return "rotate failed";
  if (rename(staging, target) != 0) return "activate failed";
  return NULL;
}

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

void br_handle_widget_install_begin(br_server_state *server, const char *payload) {
  br_json_token tokens[32]; char tid[128]; char wid[128];
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
  const char *err = br_ota_prepare_staging(server, ".incoming-widget");
  if (err) { br_widget_send_ack(server, tid, "begin", false, err); return; }
  br_server_logf("widget_install_begin: %s id=%s", tid, wid);
  br_widget_send_ack(server, tid, "begin", true, "");
}

void br_handle_widget_install_chunk(br_server_state *server, const char *payload) {
  char tid[128];
  const char *err = br_ota_stream_chunk(server, ".incoming-widget", payload, tid, sizeof(tid));
  if (err) { br_widget_send_ack(server, tid, "chunk", false, err); return; }
  /* skip ack for successful chunks — client streams without waiting */
}

void br_handle_widget_install_commit(br_server_state *server, const char *payload) {
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
  /* Rotate existing widgets/<id>/ → widgets/<id>.previous/, then move
     staging → widgets/<id>/. */
  {
    const char *err = br_ota_rotate_activate(staging, target, previous);
    if (err) { br_widget_send_ack(server, tid, "commit", false, err); return; }
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

void br_handle_asset_begin(br_server_state *server, const char *payload) {
  br_json_token tokens[32]; char tid[128];
  int count = br_json_parse(payload, strlen(payload), tokens, 32);
  if (count < 1) { br_asset_send_ack(server, "", "begin", false, "bad json"); return; }
  int idx = br_json_find_key(payload, tokens, count, 0, "transferId");
  if (idx < 0 || !br_json_token_to_string(payload, &tokens[idx], tid, sizeof(tid)) || !tid[0]) {
    br_asset_send_ack(server, "", "begin", false, "missing transferId"); return;
  }
  const char *err = br_ota_prepare_staging(server, ".incoming-desktop-pet");
  if (err) { br_asset_send_ack(server, tid, "begin", false, err); return; }
  br_server_logf("asset_begin: %s", tid);
  br_asset_send_ack(server, tid, "begin", true, "");
}

void br_handle_asset_stat(br_server_state *server, const char *payload) {
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

void br_handle_asset_chunk(br_server_state *server, const char *payload) {
  char tid[128];
  const char *err = br_ota_stream_chunk(server, ".incoming-desktop-pet", payload, tid, sizeof(tid));
  if (err) { br_asset_send_ack(server, tid, "chunk", false, err); return; }
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

int br_asset_decode_b64_file(const char *b64_path, const char *outpath) {
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

int br_asset_file_stats_checksum(
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

int br_asset_tree_stats(
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

void br_handle_asset_file_commit(br_server_state *server, const char *payload) {
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

void br_handle_asset_commit(br_server_state *server, const char *payload) {
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
  {
    const char *err = br_ota_rotate_activate(staging, current, previous);
    if (err) { br_asset_send_ack(server, tid, "commit", false, err); return; }
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

void br_handle_asset_patch_commit(br_server_state *server, const char *payload) {
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

