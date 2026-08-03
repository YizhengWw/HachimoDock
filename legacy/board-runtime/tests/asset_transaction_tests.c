/*
 * [Input] board_server.c transaction helpers and temporary filesystem/pipe fixtures.
 * [Output] Host behavior coverage for asset integrity plus ACK-gated widget begin/chunk/commit/list/delete, checksum validation, multi-package inventory, activation, and package cleanup.
 * [Pos] board-runtime C transaction test.
 * [Sync] If this file changes, update board-runtime/README.md.
 */

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

static void read_ack_line(int fd, char *buffer, size_t buffer_size) {
  ssize_t nread = read(fd, buffer, buffer_size - 1);
  if (nread <= 0) {
    fail("read widget ack");
  }
  buffer[nread] = '\0';
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

static void test_widget_install_and_delete_transaction(void) {
  char template_path[BR_MAX_PATH];
  char installed_path[BR_MAX_PATH];
  char inactive_dir[BR_MAX_PATH];
  char inactive_runtime_dir[BR_MAX_PATH];
  char inactive_component_path[BR_MAX_PATH];
  char inactive_widget_path[BR_MAX_PATH];
  char active_path[BR_MAX_PATH];
  char page_path[BR_MAX_PATH];
  char value[128];
  char ack[8192];
  int ack_pipe[2];
  br_server_state server;
  const char *begin_payload =
    "{\"transferId\":\"widget-install-test\",\"widgetId\":\"test-widget\"}";
  const char *chunk_payload =
    "{\"transferId\":\"widget-install-test\","
    "\"path\":\"runtime/widget.json\",\"data\":\"aGVsbG8=\","
    "\"index\":\"0\",\"decodedSize\":5,\"checksum\":\"a430d84680aabd0b\"}";
  const char *commit_payload =
    "{\"transferId\":\"widget-install-test\",\"widgetId\":\"test-widget\"}";
  const char *list_payload =
    "{\"requestId\":\"widget-inventory-test\"}";
  const char *inactive_delete_payload =
    "{\"transferId\":\"widget-delete-inactive\",\"widgetId\":\"other-widget\"}";
  const char *delete_payload =
    "{\"transferId\":\"widget-delete-test\",\"widgetId\":\"test-widget\"}";

  snprintf(
    template_path,
    sizeof(template_path),
    "%s/openclaw-widget-test-XXXXXX",
    getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp"
  );
  assert_true(mkdtemp(template_path) != NULL, "create widget temp dir");
  assert_true(pipe(ack_pipe) == 0, "create widget ack pipe");

  memset(&server, 0, sizeof(server));
  snprintf(server.config.root_dir, sizeof(server.config.root_dir), "%s", template_path);
  snprintf(server.config.board_device_id, sizeof(server.config.board_device_id), "board-test");
  server.usb_serial.fd = ack_pipe[1];
  server.usb_serial.connected = true;

  br_handle_widget_install_begin(&server, begin_payload);
  read_ack_line(ack_pipe[0], ack, sizeof(ack));
  assert_contains(ack, "\"phase\":\"begin\"", "begin ack phase");
  assert_contains(ack, "\"ok\":true", "begin ack success");

  br_handle_widget_install_chunk(&server, chunk_payload);
  read_ack_line(ack_pipe[0], ack, sizeof(ack));
  assert_contains(ack, "\"phase\":\"chunk\"", "chunk ack phase");
  assert_contains(ack, "\"path\":\"runtime/widget.json\"", "chunk ack path");
  assert_contains(ack, "\"index\":0", "chunk ack index");
  assert_contains(ack, "\"decodedSize\":5", "chunk ack decoded size");
  assert_contains(ack, "\"checksum\":\"a430d84680aabd0b\"", "chunk ack checksum");
  assert_contains(ack, "\"ok\":true", "chunk ack success");

  br_handle_widget_install_commit(&server, commit_payload);
  read_ack_line(ack_pipe[0], ack, sizeof(ack));
  assert_contains(ack, "\"phase\":\"commit\"", "commit ack phase");
  assert_contains(ack, "\"ok\":true", "commit ack success");

  snprintf(
    installed_path,
    sizeof(installed_path),
    "%s/widgets/test-widget/runtime/widget.json",
    template_path
  );
  assert_true(br_read_text_file(installed_path, value, sizeof(value)), "read installed widget");
  assert_string(value, "hello", "installed widget bytes");
  snprintf(active_path, sizeof(active_path), "%s/.active-widget", template_path);
  assert_true(br_read_text_file(active_path, value, sizeof(value)), "read active widget");
  br_widget_trim(value);
  assert_string(value, "test-widget", "active widget id");
  snprintf(page_path, sizeof(page_path), "%s/.screen-page", template_path);
  assert_true(br_read_text_file(page_path, value, sizeof(value)), "read widget screen page");
  br_widget_trim(value);
  assert_string(value, "stats", "widget install switches to stats");

  snprintf(inactive_dir, sizeof(inactive_dir), "%s/widgets/other-widget", template_path);
  snprintf(inactive_runtime_dir, sizeof(inactive_runtime_dir), "%s/runtime", inactive_dir);
  assert_true(br_asset_mkdir_p(inactive_runtime_dir) == 0, "create inactive widget package");
  snprintf(
    inactive_component_path,
    sizeof(inactive_component_path),
    "%s/component.json",
    inactive_dir
  );
  snprintf(
    inactive_widget_path,
    sizeof(inactive_widget_path),
    "%s/widget.json",
    inactive_runtime_dir
  );
  write_text_file(
    inactive_component_path,
    "{\"id\":\"other-widget\",\"name\":\"Other Widget\",\"kind\":\"tool\","
    "\"version\":\"2.0.0\"}"
  );
  write_text_file(inactive_widget_path, "{\"schema_version\":1}");

  br_handle_usb_message("widget/list", list_payload, &server);
  read_ack_line(ack_pipe[0], ack, sizeof(ack));
  assert_contains(ack, "\"topic\":\"widget/inventory\"", "inventory response topic");
  assert_contains(
    ack,
    "\"requestId\":\"widget-inventory-test\"",
    "inventory request identity"
  );
  assert_contains(ack, "\"boardDeviceId\":\"board-test\"", "inventory board identity");
  assert_contains(ack, "\"activeWidgetId\":\"test-widget\"", "inventory active widget");
  assert_contains(
    ack,
    "{\"id\":\"other-widget\",\"name\":\"Other Widget\",\"kind\":\"tool\","
    "\"version\":\"2.0.0\",\"active\":false,\"manifestState\":\"valid\","
    "\"removable\":true}",
    "inventory valid inactive package"
  );
  assert_contains(
    ack,
    "{\"id\":\"test-widget\",\"name\":\"test-widget\",\"kind\":null,"
    "\"version\":null,\"active\":true,\"manifestState\":\"missing\","
    "\"removable\":true}",
    "inventory active package with missing manifest"
  );
  assert_contains(ack, "\"complete\":true", "inventory is complete");

  br_handle_widget_delete(&server, inactive_delete_payload);
  read_ack_line(ack_pipe[0], ack, sizeof(ack));
  assert_contains(ack, "\"phase\":\"delete\"", "inactive delete ack phase");
  assert_contains(ack, "\"ok\":true", "inactive delete ack success");
  assert_true(access(inactive_dir, F_OK) != 0, "inactive widget package removed");
  assert_true(
    br_read_text_file(active_path, value, sizeof(value)),
    "read active widget after inactive delete"
  );
  br_widget_trim(value);
  assert_string(value, "test-widget", "inactive delete preserves active widget");
  assert_true(
    br_read_text_file(page_path, value, sizeof(value)),
    "read screen page after inactive delete"
  );
  br_widget_trim(value);
  assert_string(value, "stats", "inactive delete preserves widget screen page");

  br_handle_widget_delete(&server, delete_payload);
  read_ack_line(ack_pipe[0], ack, sizeof(ack));
  assert_contains(ack, "\"phase\":\"delete\"", "delete ack phase");
  assert_contains(ack, "\"ok\":true", "delete ack success");
  assert_true(access(installed_path, F_OK) != 0, "installed widget package removed");
  assert_true(br_read_text_file(active_path, value, sizeof(value)), "read cleared active widget");
  br_widget_trim(value);
  assert_string(value, "", "active widget cleared");
  assert_true(br_read_text_file(page_path, value, sizeof(value)), "read main screen page");
  br_widget_trim(value);
  assert_string(value, "main", "widget delete switches to main");

  close(ack_pipe[0]);
  close(ack_pipe[1]);
  server.usb_serial.connected = false;
  server.usb_serial.fd = -1;
  (void) br_asset_remove_tree(template_path);
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

int main(void) {
  test_file_decode_checksum_and_tree_stats();
  test_audio_patch_path_validation();
  test_widget_install_and_delete_transaction();
  test_websocket_accept_uses_rfc_magic_guid();
  test_config_json_uses_config_mqtt_url_when_client_not_connected();
  printf("asset transaction tests passed\n");
  return 0;
}
