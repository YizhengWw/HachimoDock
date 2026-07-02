#define main br_board_server_main
#include "../src/board_server.c"
#undef main

static void fail(const char *message) {
  fprintf(stderr, "FAIL: %s\n", message);
  exit(1);
}

static void assert_true(bool condition, const char *message) {
  if (!condition) {
    fail(message);
  }
}

static void assert_string(const char *actual, const char *expected, const char *message) {
  if (strcmp(actual, expected) != 0) {
    fprintf(stderr, "FAIL: %s\nexpected: %s\nactual:   %s\n", message, expected, actual);
    exit(1);
  }
}

static void assert_contains(const char *actual, const char *expected, const char *message) {
  if (!actual || !strstr(actual, expected)) {
    fprintf(stderr, "FAIL: %s\nmissing: %s\nactual:   %s\n", message, expected, actual ? actual : "(null)");
    exit(1);
  }
}

static void write_text_file(const char *path, const char *text) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) {
    fail("open write text file");
  }
  if (br_asset_write_all(fd, text, strlen(text)) != 0) {
    close(fd);
    fail("write text file");
  }
  close(fd);
}

static void test_file_decode_checksum_and_tree_stats(void) {
  char template_path[BR_MAX_PATH];
  char videos_dir[BR_MAX_PATH];
  char b64_path[BR_MAX_PATH];
  char out_path[BR_MAX_PATH];
  char orphan_path[BR_MAX_PATH];
  char checksum[17];
  unsigned long long size = 0;
  unsigned long long file_count = 0;
  unsigned long long byte_count = 0;
  bool has_b64 = false;
  char bytes[8];
  int fd;
  ssize_t nread;

  snprintf(template_path, sizeof(template_path), "%s/openclaw-asset-test-XXXXXX",
           getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
  assert_true(mkdtemp(template_path) != NULL, "create temp dir");

  snprintf(videos_dir, sizeof(videos_dir), "%s/videos", template_path);
  assert_true(br_asset_mkdir_p(videos_dir) == 0, "create videos dir");
  snprintf(b64_path, sizeof(b64_path), "%s/idle.mp4.b64", videos_dir);
  snprintf(out_path, sizeof(out_path), "%s/idle.mp4", videos_dir);
  write_text_file(b64_path, "aGVs\nbG8=\n");

  assert_true(br_asset_decode_b64_file(b64_path, out_path) == 0, "decode one b64 file");
  assert_true(access(b64_path, F_OK) != 0, "remove committed b64 file");
  assert_true(br_asset_file_stats_checksum(out_path, &size, checksum) == 0, "checksum output file");
  assert_true(size == 5, "decoded file size");
  assert_string(checksum, "a430d84680aabd0b", "decoded file checksum");

  fd = open(out_path, O_RDONLY);
  assert_true(fd >= 0, "open decoded output");
  nread = read(fd, bytes, sizeof(bytes));
  close(fd);
  assert_true(nread == 5, "read decoded output bytes");
  bytes[5] = '\0';
  assert_string(bytes, "hello", "decoded output bytes");

  assert_true(br_asset_tree_stats(template_path, &file_count, &byte_count, &has_b64) == 0,
              "scan committed tree");
  assert_true(file_count == 1, "committed tree file count");
  assert_true(byte_count == 5, "committed tree byte count");
  assert_true(!has_b64, "committed tree has no b64 leftovers");

  snprintf(orphan_path, sizeof(orphan_path), "%s/orphan.b64", template_path);
  write_text_file(orphan_path, "AA==\n");
  assert_true(br_asset_tree_stats(template_path, &file_count, &byte_count, &has_b64) == 0,
              "scan tree with leftover b64");
  assert_true(has_b64, "detect leftover b64");

  (void)br_asset_remove_tree(template_path);
}

static void test_audio_patch_path_validation(void) {
  assert_true(br_asset_is_audio_patch_path("videos/done.wav"), "allow done wav patch");
  assert_true(br_asset_is_audio_patch_path("videos/working.thinking.wav"), "allow dotted family wav patch");
  assert_true(!br_asset_is_audio_patch_path("videos/done.mp4"), "reject video patch path");
  assert_true(!br_asset_is_audio_patch_path("done.wav"), "reject non-videos patch path");
  assert_true(!br_asset_is_audio_patch_path("videos/nested/done.wav"), "reject nested patch path");
  assert_true(!br_asset_is_audio_patch_path("../videos/done.wav"), "reject traversal patch path");
}

static void test_websocket_accept_uses_rfc_magic_guid(void) {
  int fds[2];
  br_server_state server;
  const char *request =
    "GET / HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n";
  char response[2048];
  ssize_t nread;

  memset(&server, 0, sizeof(server));
  server.listen_fd = -1;
  server.discovery_fd = -1;
  server.mdns_fd = -1;
  server.pairing.state = BR_PAIRING_STA_READY;
  snprintf(server.config.root_dir, sizeof(server.config.root_dir), "/tmp");
  snprintf(server.config.local_device_id, sizeof(server.config.local_device_id), "board-test");
  snprintf(server.config.board_device_id, sizeof(server.config.board_device_id), "board-test");
  snprintf(server.config.mqtt_namespace, sizeof(server.config.mqtt_namespace), "desk");
  br_build_topics(&server);

  assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair for websocket test");
  assert_true(write(fds[0], request, strlen(request)) == (ssize_t) strlen(request), "write websocket request");
  shutdown(fds[0], SHUT_WR);

  assert_true(br_handle_http_connection(&server, fds[1]), "websocket request is upgraded");
  nread = read(fds[0], response, sizeof(response) - 1);
  assert_true(nread > 0, "read websocket response");
  response[nread] = '\0';
  assert_contains(response,
                  "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n",
                  "websocket accept matches RFC sample");

  close(fds[0]);
  close(fds[1]);
}

static void test_config_json_uses_config_mqtt_url_when_client_not_connected(void) {
  br_server_state server;
  char json[BR_MAX_JSON];

  memset(&server, 0, sizeof(server));
  server.pairing.state = BR_PAIRING_STA_READY;
  server.config.http_port = 80;
  snprintf(server.config.mqtt_url, sizeof(server.config.mqtt_url), "mqtt://broker.openclaw.example:1883");
  snprintf(server.config.mqtt_namespace, sizeof(server.config.mqtt_namespace), "desk");
  snprintf(server.config.local_device_id, sizeof(server.config.local_device_id), "board-test");
  snprintf(server.config.board_device_id, sizeof(server.config.board_device_id), "board-test");
  snprintf(server.config.target_device_id, sizeof(server.config.target_device_id), "desktop-test");
  br_build_topics(&server);

  br_build_config_json(&server, "192.0.2.68", json, sizeof(json));

  assert_contains(json,
                  "\"brokerUrl\":\"mqtt://broker.openclaw.example:1883\"",
                  "config brokerUrl falls back to configured MQTT URL");
}

static void read_small_file(const char *path, char *buf, size_t buf_size) {
  int fd = open(path, O_RDONLY);
  ssize_t nread;
  if (fd < 0) {
    fail("open small file for read");
  }
  nread = read(fd, buf, buf_size - 1);
  close(fd);
  if (nread < 0) {
    fail("read small file");
  }
  buf[nread] = '\0';
}

/* The shared OTA helpers carry the streaming mechanics for BOTH the appearance
   (asset) and widget (widget) channels, so pin their behavior directly:
   staging prep, truncate-on-index-0 / append-after chunk writes, path-traversal
   rejection, and the rotate→activate commit step. */
static void test_ota_shared_transfer_helpers(void) {
  char root[] = "/tmp/br-ota-helpers-XXXXXX";
  br_server_state server;
  char tid[128];
  char path[BR_MAX_PATH];
  char staging[BR_MAX_PATH], target[BR_MAX_PATH], previous[BR_MAX_PATH];
  char content[256];
  const char *err;

  assert_true(mkdtemp(root) != NULL, "mkdtemp ota helper root");
  memset(&server, 0, sizeof(server));
  snprintf(server.config.root_dir, sizeof(server.config.root_dir), "%s", root);

  /* begin: staging dir is created clean */
  assert_true(br_ota_prepare_staging(&server, ".incoming-test") == NULL, "prepare staging succeeds");
  snprintf(staging, sizeof(staging), "%s/.incoming-test", root);
  assert_true(access(staging, R_OK) == 0, "staging dir exists");

  /* chunk index 0 truncates, later indexes append newline-terminated b64 */
  err = br_ota_stream_chunk(&server, ".incoming-test",
    "{\"transferId\":\"t1\",\"path\":\"videos/a.bin\",\"data\":\"aGVsbG8=\",\"index\":\"0\"}",
    tid, sizeof(tid));
  assert_true(err == NULL, "first chunk streams");
  assert_string(tid, "t1", "chunk fills transferId");
  err = br_ota_stream_chunk(&server, ".incoming-test",
    "{\"transferId\":\"t1\",\"path\":\"videos/a.bin\",\"data\":\"d29ybGQ=\",\"index\":\"1\"}",
    tid, sizeof(tid));
  assert_true(err == NULL, "second chunk appends");
  snprintf(path, sizeof(path), "%s/videos/a.bin.b64", staging);
  read_small_file(path, content, sizeof(content));
  assert_string(content, "aGVsbG8=\nd29ybGQ=\n", "chunks append newline-terminated b64");
  err = br_ota_stream_chunk(&server, ".incoming-test",
    "{\"transferId\":\"t1\",\"path\":\"videos/a.bin\",\"data\":\"cmVzZXQ=\",\"index\":\"0\"}",
    tid, sizeof(tid));
  assert_true(err == NULL, "index 0 restart streams");
  read_small_file(path, content, sizeof(content));
  assert_string(content, "cmVzZXQ=\n", "index 0 truncates the staged file");

  /* invalid chunks are rejected with a short error for the caller's ack */
  err = br_ota_stream_chunk(&server, ".incoming-test", "not json", tid, sizeof(tid));
  assert_true(err != NULL && tid[0] == '\0', "bad json rejected with empty tid");
  err = br_ota_stream_chunk(&server, ".incoming-test",
    "{\"transferId\":\"t1\",\"path\":\"../evil\",\"data\":\"eA==\",\"index\":\"0\"}",
    tid, sizeof(tid));
  assert_true(err != NULL, "path traversal rejected");

  /* commit: rotate existing target to previous, activate staging */
  snprintf(target, sizeof(target), "%s/widgets-current", root);
  snprintf(previous, sizeof(previous), "%s/widgets-current.previous", root);
  assert_true(br_asset_mkdir_p(target) == 0, "mkdir existing target");
  snprintf(path, sizeof(path), "%s/old.txt", target);
  write_text_file(path, "old install");
  assert_true(br_ota_rotate_activate(staging, target, previous) == NULL, "rotate+activate succeeds");
  snprintf(path, sizeof(path), "%s/videos/a.bin.b64", target);
  assert_true(access(path, R_OK) == 0, "staged tree became the target");
  snprintf(path, sizeof(path), "%s/old.txt", previous);
  assert_true(access(path, R_OK) == 0, "old install rotated to previous");

  (void) br_asset_remove_tree(root);
}

int main(void) {
  test_file_decode_checksum_and_tree_stats();
  test_audio_patch_path_validation();
  test_websocket_accept_uses_rfc_magic_guid();
  test_config_json_uses_config_mqtt_url_when_client_not_connected();
  test_ota_shared_transfer_helpers();
  printf("asset transaction tests passed\n");
  return 0;
}
