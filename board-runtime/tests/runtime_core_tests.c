#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "rotary_decoder.h"
#include "runtime_common.h"
#include "runtime_debug.h"
#include "runtime_json.h"
#include "runtime_mqtt.h"
#include "runtime_pairing.h"
#include "runtime_protocol.h"
#include "runtime_usb_serial.h"
#include "screen_page.h"
#include "touch_gesture.h"
#include "voice_button.h"

static void assert_true(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "assertion failed: %s\n", message);
    exit(1);
  }
}

static void assert_string(const char *actual, const char *expected, const char *message) {
  if (!actual || !expected || strcmp(actual, expected) != 0) {
    fprintf(stderr,
            "assertion failed: %s\nexpected: %s\nactual:   %s\n",
            message,
            expected ? expected : "(null)",
            actual ? actual : "(null)");
    exit(1);
  }
}

static void assert_contains(const char *haystack, const char *needle, const char *message) {
  if (!haystack || !needle || !strstr(haystack, needle)) {
    fprintf(stderr,
            "assertion failed: %s\nmissing: %s\nwithin:  %s\n",
            message,
            needle ? needle : "(null)",
            haystack ? haystack : "(null)");
    exit(1);
  }
}

static void make_temp_root(char *output, size_t output_size) {
  snprintf(output, output_size, "/tmp/runtime-core-tests-%d-XXXXXX", (int) getpid());
  assert_true(mkdtemp(output) != NULL, "mkdtemp");
}

static void cleanup_temp_root(const char *path) {
  char command[BR_MAX_PATH + 32];
  snprintf(command, sizeof(command), "rm -rf %s", path);
  (void) system(command);
}

static bool bytes_contain_text(const unsigned char *data, size_t data_size, const char *text) {
  size_t text_size = strlen(text);
  if (!data || !text || text_size == 0 || text_size > data_size) {
    return false;
  }
  for (size_t i = 0; i + text_size <= data_size; i += 1) {
    if (memcmp(data + i, text, text_size) == 0) {
      return true;
    }
  }
  return false;
}

static void test_common_normalization_paths_and_mqtt_urls(void) {
  char output[256];
  char small[5];
  size_t used = 0;
  br_mqtt_endpoint endpoint;

  assert_true(br_normalize_text("  hello  ", "fallback", output, sizeof(output)), "normalize trims text");
  assert_string(output, "hello", "trimmed text");

  assert_true(br_normalize_text(" \t\n ", "fallback", output, sizeof(output)), "normalize blank fallback");
  assert_string(output, "fallback", "blank input uses fallback");

  assert_true(br_normalize_text("\xE7\x8C\xAB\xE5\x92\xAA" "abc", "", small, sizeof(small)),
              "normalize truncates utf8 safely");
  assert_string(small, "\xE7\x8C\xAB", "utf8 truncation keeps complete codepoint");

  snprintf(output, sizeof(output), " /A B//C? ");
  assert_true(br_normalize_topic_part(output, "fallback", output, sizeof(output)), "topic normalize in-place");
  assert_string(output, "A-B-C", "topic normalized in-place");

  assert_true(br_safe_join("/runtime", "/ui/index.html?cache=1", output, sizeof(output)),
              "safe join strips leading slash and query");
  assert_string(output, "/runtime/ui/index.html", "safe join result");
  assert_true(!br_safe_join("/runtime", "/%2e%2e/passwd", output, sizeof(output)),
              "safe join rejects encoded traversal");
  assert_true(!br_safe_join(NULL, "index.html", output, sizeof(output)),
              "safe join rejects null base");

  assert_string(br_content_type("index.html"), "text/html; charset=utf-8", "html content type");
  assert_string(br_content_type("image.svg"), "image/svg+xml", "svg content type");
  assert_string(br_content_type("blob.bin"), "application/octet-stream", "unknown content type");

  assert_true(br_parse_mqtt_url("mqtt://broker.local:1884", &endpoint), "parse mqtt url");
  assert_string(endpoint.host, "broker.local", "mqtt host");
  assert_true(endpoint.port == 1884, "mqtt explicit port");
  assert_true(br_parse_mqtt_url("tcp://broker.local", &endpoint), "parse tcp mqtt url");
  assert_true(endpoint.port == 1883, "mqtt default port");
  assert_true(br_parse_mqtt_url("mqtt://broker.local:999999", &endpoint), "parse mqtt bad port");
  assert_true(endpoint.port == 1883, "mqtt bad port falls back");
  assert_true(!br_parse_mqtt_url("http://broker.local", &endpoint), "reject non-mqtt url");

  output[0] = '\0';
  assert_true(br_snprintf_append(output, sizeof(output), &used, "prefix=%d", 7) == 0,
              "snprintf append");
  br_json_escape_append(output, sizeof(output), &used, "\n\"\\\x01");
  assert_string(output, "prefix=7\\n\\\"\\\\\\u0001", "json escape append");
}

static void test_common_asset_checksum_hex(void) {
  unsigned long long checksum = BR_FNV1A64_OFFSET;
  char hex[17];
  const unsigned char hello[] = "hello";

  checksum = br_fnv1a64_update(checksum, hello, strlen((const char *) hello));
  br_fnv1a64_hex(checksum, hex, sizeof(hex));

  assert_string(hex, "a430d84680aabd0b", "fnv1a64 hello checksum");
}

static void test_common_file_io_device_id_and_payload_write(void) {
  char root[BR_MAX_PATH];
  char path[BR_MAX_PATH];
  char output[512];
  const char *error_msg = NULL;

  make_temp_root(root, sizeof(root));
  snprintf(path, sizeof(path), "%s/device.json", root);

  assert_true(br_atomic_write_text(path, "{\"deviceId\":\"board-01\"}\n"), "atomic write device id");
  assert_true(br_read_text_file(path, output, sizeof(output)), "read text file");
  assert_contains(output, "board-01", "read back content");
  assert_true(br_read_device_id_json(path, output, sizeof(output)), "read deviceId");
  assert_string(output, "board-01", "deviceId value");

  assert_true(br_atomic_write_text(path, "{\"desktopDeviceId\":\"desktop-01\"}\n"),
              "atomic write desktop id");
  assert_true(br_read_device_id_json(path, output, sizeof(output)), "read desktopDeviceId fallback");
  assert_string(output, "desktop-01", "desktopDeviceId value");

  assert_true(br_apply_payload_write(root, ".current-speech", "hello", &error_msg),
              "payload write current speech");
  snprintf(path, sizeof(path), "%s/.current-speech", root);
  assert_true(br_read_text_file(path, output, sizeof(output)), "read payload write output");
  assert_string(output, "hello", "payload write content");

  error_msg = NULL;
  assert_true(!br_apply_payload_write(root, "current-speech", "hello", &error_msg),
              "payload write rejects non-whitelisted spelling");
  assert_true(error_msg != NULL, "payload write sets error");

  cleanup_temp_root(root);
}

static void test_runtime_json_helpers(void) {
  const char *json =
    "{\"name\":\"line\\n\\u4f60\",\"num\":12.5,\"ok\":true,"
    "\"arr\":[1,2],\"raw\":{\"a\":1},\"bad\":\"\\u12\"}";
  br_json_token tokens[64];
  char output[128];
  double number = 0;
  bool value = false;
  int count = br_json_parse(json, strlen(json), tokens, 64);

  assert_true(count > 0, "json parse");
  assert_true(tokens[0].type == BR_JSON_OBJECT, "json root object");

  int index = br_json_find_key(json, tokens, count, 0, "name");
  assert_true(index > 0, "json find string key");
  assert_true(br_json_token_to_string(json, &tokens[index], output, sizeof(output)),
              "json string unescape");
  assert_string(output, "line\n?", "json string newline and unicode placeholder");

  index = br_json_find_key(json, tokens, count, 0, "num");
  assert_true(br_json_token_to_double(json, &tokens[index], &number), "json double");
  assert_true(number > 12.49 && number < 12.51, "json double value");

  index = br_json_find_key(json, tokens, count, 0, "ok");
  assert_true(br_json_token_to_bool(json, &tokens[index], &value), "json bool");
  assert_true(value, "json bool value");

  index = br_json_find_key(json, tokens, count, 0, "raw");
  assert_true(br_json_copy_raw(json, &tokens[index], output, sizeof(output)), "json copy raw object");
  assert_string(output, "{\"a\":1}", "json raw object");

  index = br_json_find_key(json, tokens, count, 0, "bad");
  assert_true(!br_json_token_to_string(json, &tokens[index], output, sizeof(output)),
              "json truncated unicode escape rejected");
  assert_true(br_json_find_key(json, tokens, count, 0, "missing") < 0, "json missing key");
  assert_true(br_json_parse("{\"x\":", strlen("{\"x\":"), tokens, 64) < 0, "json rejects malformed");
  assert_true(br_json_parse(json, strlen(json), tokens, 1) < 0, "json rejects token exhaustion");
}

static void test_protocol_builders_and_payload_wrapping(void) {
  br_input_action action;
  br_bridge_state_update update;
  br_remote_binding binding;
  br_audio_bridge_command audio;
  char output[2048];
  char source[64];
  char error[64];
  long long ts_ms = 0;

  assert_true(!br_parse_input_action_json("{\"type\":\"drag\"}", &action, error, sizeof(error)),
              "input action rejects unknown type");
  assert_string(error, "invalid_input_action", "input action error");

  memset(&action, 0, sizeof(action));
  snprintf(action.type, sizeof(action.type), "swipe_left");
  action.has_x = true;
  action.x = 12.5;
  action.has_y = true;
  action.y = 30;
  action.has_duration_ms = true;
  action.duration_ms = 250;
  snprintf(action.view, sizeof(action.view), "stats\"page");
  snprintf(action.active_detail_id, sizeof(action.active_detail_id), "detail-1");
  assert_true(br_build_input_action_payload("board\"1", "local\n2", "usb", &action, 12345,
                                            output, sizeof(output)) == 0,
              "build input action payload");
  assert_contains(output, "\"boardDeviceId\":\"board\\\"1\"", "payload escapes board id");
  assert_contains(output, "\"localDeviceId\":\"local\\n2\"", "payload escapes local id");
  assert_contains(output, "\"type\":\"swipe_left\"", "payload type");
  assert_contains(output, "\"x\":12.500", "payload x");
  assert_contains(output, "\"tsMs\":12345", "payload timestamp");
  assert_contains(output, "\"view\":\"stats\\\"page\"", "payload view");
  assert_true(br_build_input_action_payload("board", "local", "usb", &action, 1, output, 16) == -1,
              "build input action rejects small buffer");

  assert_true(br_build_message_complete_json("{\"title\":\"Title\",\"content\":\"Body\"}",
                                             "fallback", output, sizeof(output)),
              "build message complete from card");
  assert_contains(output, "\"type\":\"message_complete\"", "message complete type");
  assert_contains(output, "Title\\nBody", "message complete response");

  assert_true(br_payload_to_json_object("{\"source\":\"codex\",\"tsMs\":99,\"ok\":true}",
                                        output, sizeof(output), &ts_ms, source, sizeof(source)),
              "payload object passes through");
  assert_string(output, "{\"source\":\"codex\",\"tsMs\":99,\"ok\":true}", "payload raw object");
  assert_true(ts_ms == 99, "payload timestamp extracted");
  assert_string(source, "codex", "payload source extracted");

  assert_true(br_payload_to_json_object("hello\nworld", output, sizeof(output), &ts_ms, source, sizeof(source)),
              "payload text wrapped");
  assert_string(output, "{\"text\":\"hello\\nworld\"}", "payload text wrapper");

  assert_true(br_bridge_state_from_message("desk/board/state/active",
                                           "{\"payload\":{\"reason\":\"active.no_sources\",\"source\":\"active\"}}",
                                           &update),
              "bridge active.no_sources parse");
  assert_string(update.state, "idle", "active.no_sources maps idle");

  assert_true(!br_parse_remote_binding_json(
                "{\"command\":\"remote_cli_binding.update\",\"enabled\":false,"
                "\"targetDeviceId\":\"desktop-01\"}",
                &binding),
              "disabled remote binding rejected");

  assert_true(br_parse_audio_bridge_command_json(
                "{\"type\":\"audio_bridge\",\"action\":\"start\",\"pcIp\":\"192.168.1.2\"}",
                &audio),
              "audio bridge camel-case defaults");
  assert_true(audio.enabled, "audio bridge enabled");
  assert_string(audio.pc_ip, "192.168.1.2", "audio bridge pc ip");
  assert_true(audio.pc_port == 50001 && audio.listen_port == 50002, "audio bridge default ports");
  assert_string(audio.capture_dev, "default", "audio bridge default capture");
  assert_string(audio.voice_button, BR_VOICE_BUTTON_ENCODER_HOLD, "audio bridge default voice button");
  assert_true(!br_parse_audio_bridge_command_json(
                "{\"type\":\"audio_bridge\",\"action\":\"start\",\"pcIp\":\"192.168.1.2\","
                "\"pcPort\":70000}",
                &audio),
              "audio bridge rejects invalid port");
}

static void test_debug_overlay_state_files(void) {
  char root[BR_MAX_PATH];
  char path[BR_MAX_PATH];
  char output[4096];

  make_temp_root(root, sizeof(root));

  br_debug_overlay_flag_path(root, path, sizeof(path));
  assert_contains(path, ".debug-overlay-enabled", "overlay flag path");
  assert_true(!br_debug_overlay_enabled(root), "overlay initially disabled");
  assert_true(br_debug_set_overlay_enabled(root, false), "disable absent overlay flag");
  assert_true(!br_debug_overlay_enabled(root), "absent disable remains disabled");

  assert_true(br_debug_set_overlay_enabled(root, true), "enable overlay flag");
  assert_true(br_debug_overlay_enabled(root), "overlay enabled");

  br_debug_session_snapshot_path(root, path, sizeof(path));
  assert_true(br_atomic_write_text(path, "{\"state\":\"working\"}"), "write session snapshot");
  br_debug_screen_snapshot_path(root, path, sizeof(path));
  assert_true(br_atomic_write_text(path, "{\"displayedState\":\"working\"}"), "write screen snapshot");
  assert_true(br_debug_build_state_json(root, output, sizeof(output)), "build debug state json");
  assert_contains(output, "\"overlayEnabled\":true", "debug state overlay true");
  assert_contains(output, "\"session\":{\"state\":\"working\"}", "debug state session object");
  assert_contains(output, "\"screen\":{\"displayedState\":\"working\"}", "debug state screen object");

  assert_true(br_debug_set_overlay_enabled(root, false), "disable existing overlay flag");
  assert_true(!br_debug_overlay_enabled(root), "overlay disabled after unlink");
  br_debug_screen_snapshot_path(root, path, sizeof(path));
  assert_true(br_atomic_write_text(path, "not-json"), "write invalid screen snapshot");
  assert_true(br_debug_build_state_json(root, output, sizeof(output)), "build debug state with invalid screen");
  assert_contains(output, "\"overlayEnabled\":false", "debug state overlay false");
  assert_contains(output, "\"screen\":null", "debug invalid screen becomes null");

  cleanup_temp_root(root);
}

static void test_pairing_discovery_branches(void) {
  br_pairing_machine machine;

  br_pairing_init(&machine, false, 1000, 10);
  assert_true(br_pairing_start_discovery(&machine, 20), "start lan discovery explicitly");
  assert_true(machine.state == BR_PAIRING_LAN_DISCOVERY, "lan discovery state");
  assert_true(machine.last_discovery_ms == 20, "discovery timestamp");
  assert_true(!br_pairing_tick(&machine, 1019), "no timeout before threshold");
  assert_true(br_pairing_tick(&machine, 1020), "timeout at threshold");
  assert_true(machine.state == BR_PAIRING_AP_FALLBACK, "ap fallback after timeout");

  br_pairing_init(&machine, false, 0, 0);
  assert_true(br_pairing_mark_discovered(&machine, 30), "mark discovered starts discovery");
  assert_true(machine.discovered_once, "discovered flag");
  assert_true(!br_pairing_tick(&machine, 999999), "zero timeout disables auto fallback");
  assert_string(br_pairing_state_name(BR_PAIRING_BOOT), "boot", "pairing state boot name");
  assert_string(br_pairing_state_name((br_pairing_state) 99), "unknown", "pairing unknown name");
  assert_string(br_pairing_mode_name(BR_PAIRING_AP_FALLBACK), "ap", "pairing ap mode");
  assert_string(br_pairing_mode_name(BR_PAIRING_WAITING_CONFIG), "pairing", "pairing mode name");
  assert_true(br_pairing_is_waiting(BR_PAIRING_AP_FALLBACK), "ap fallback is waiting");
}

static void test_touch_gesture_branches_and_names(void) {
  br_touch_gesture_state state;
  br_touch_action action;

  br_touch_gesture_init(&state, 0, 0);
  assert_true(state.swipe_threshold == 40, "touch default swipe threshold");
  assert_true(state.long_press_ms == 5000, "touch default long press");
  assert_true(!br_touch_gesture_finish(&state, 0, &action), "finish without touch down");

  br_touch_gesture_set_position(&state, 10, 10);
  br_touch_gesture_start(&state, 100);
  assert_true(!br_touch_gesture_sync(&state, 5099, &action), "long press not yet emitted");
  assert_true(br_touch_gesture_sync(&state, 5100, &action), "long press emitted at threshold");
  assert_true(action.type == BR_TOUCH_LONG_PRESS, "long press type");
  assert_true(!br_touch_gesture_sync(&state, 5200, &action), "long press emitted once");
  assert_true(br_touch_gesture_finish(&state, 5200, &action), "finish after long press");
  assert_true(action.type == BR_TOUCH_LONG_PRESS, "finish preserves long press");

  br_touch_gesture_set_position(&state, 20, 20);
  br_touch_gesture_start(&state, 6000);
  br_touch_gesture_set_position(&state, 25, 24);
  assert_true(br_touch_gesture_finish(&state, 6100, &action), "tap finish");
  assert_true(action.type == BR_TOUCH_TAP, "tap classified");
  assert_true(action.dx == 5 && action.dy == 4, "tap delta");

  br_touch_gesture_set_position(&state, 100, 100);
  br_touch_gesture_start(&state, 7000);
  br_touch_gesture_set_position(&state, 50, 95);
  assert_true(br_touch_gesture_finish(&state, 7100, &action), "left swipe finish");
  assert_true(action.type == BR_TOUCH_SWIPE_LEFT, "left swipe");

  br_touch_gesture_set_position(&state, 100, 100);
  br_touch_gesture_start(&state, 8000);
  br_touch_gesture_set_position(&state, 102, 145);
  assert_true(br_touch_gesture_finish(&state, 8100, &action), "down swipe finish");
  assert_true(action.type == BR_TOUCH_SWIPE_DOWN, "down swipe");

  br_touch_gesture_set_position(&state, 100, 100);
  br_touch_gesture_start(&state, 9000);
  br_touch_gesture_set_position(&state, 98, 55);
  assert_true(br_touch_gesture_finish(&state, 9100, &action), "up swipe finish");
  assert_true(action.type == BR_TOUCH_SWIPE_UP, "up swipe");

  assert_string(br_touch_action_type_name(BR_TOUCH_TAP), "tap", "touch tap name");
  assert_string(br_touch_action_type_name(BR_TOUCH_LONG_PRESS), "long_press", "touch long name");
  assert_string(br_touch_action_type_name(BR_TOUCH_SWIPE_LEFT), "swipe_left", "touch left name");
  assert_string(br_touch_action_type_name(BR_TOUCH_SWIPE_RIGHT), "swipe_right", "touch right name");
  assert_string(br_touch_action_type_name(BR_TOUCH_SWIPE_UP), "swipe_up", "touch up name");
  assert_string(br_touch_action_type_name(BR_TOUCH_SWIPE_DOWN), "swipe_down", "touch down name");
  assert_string(br_touch_action_type_name(BR_TOUCH_NONE), "", "touch none name");
}

static void test_voice_button_aliases_and_rotary_edges(void) {
  char output[64];
  br_rotary_decoder decoder;

  assert_string(br_voice_button_default(), BR_VOICE_BUTTON_ENCODER_HOLD, "voice button default");
  assert_true(!br_voice_button_normalize(" TOP-BUTTON ", output, sizeof(output)),
              "voice top alias rejected");
  assert_true(br_voice_button_normalize("rotary button", output, sizeof(output)),
              "voice encoder alias");
  assert_string(output, BR_VOICE_BUTTON_ENCODER_HOLD, "voice encoder normalized");
  assert_true(!br_voice_button_is_top_hold("button.primary.hold"), "voice top predicate rejected");
  assert_true(br_voice_button_is_encoder_hold("knob-button.hold"), "voice encoder predicate");
  assert_true(!br_voice_button_normalize("button.primary.short_press", output, sizeof(output)),
              "voice rejects non-hold action");
  assert_true(!br_voice_button_normalize("encoder_button.hold", output, 4),
              "voice rejects too-small output");

  br_rotary_decoder_init(&decoder, 1, 1, 0);
  assert_true(decoder.trigger_steps == 4, "rotary default trigger steps");
  assert_true(br_rotary_decoder_update(NULL, 1, 1) == BR_ROTARY_NONE, "rotary null update");
  assert_true(br_rotary_select_page(" stats \n", "stats", "main", true,
                                    BR_ROTARY_CLOCKWISE, output, sizeof(output)),
              "rotary toggle trims current page");
  assert_string(output, "main", "rotary trimmed toggle target");
  assert_true(!br_rotary_select_page("stats", "stats", "main", true,
                                     BR_ROTARY_NONE, output, sizeof(output)),
              "rotary none direction rejected");

  br_rotary_decoder_init(&decoder, 1, 1, 4);
  assert_true(br_rotary_decoder_update(&decoder, 0, 1) == BR_ROTARY_NONE, "rotary partial cw 1");
  assert_true(br_rotary_decoder_update(&decoder, 1, 1) == BR_ROTARY_NONE, "rotary reversal resets");
  assert_true(br_rotary_decoder_update(&decoder, 1, 0) == BR_ROTARY_NONE, "rotary partial ccw 1");
  assert_true(br_rotary_decoder_update(&decoder, 0, 0) == BR_ROTARY_NONE, "rotary partial ccw 2");
  assert_true(br_rotary_decoder_update(&decoder, 0, 1) == BR_ROTARY_COUNTER_CLOCKWISE,
              "rotary emits ccw after reset path");
}

typedef struct {
  char topic[128];
  char payload[256];
  int call_count;
} mqtt_recv;

static void mqtt_on_publish(const char *topic, const char *payload, void *userdata) {
  mqtt_recv *recv = (mqtt_recv *) userdata;
  recv->call_count += 1;
  snprintf(recv->topic, sizeof(recv->topic), "%s", topic);
  snprintf(recv->payload, sizeof(recv->payload), "%s", payload);
}

static void test_mqtt_client_init_publish_subscribe_and_poll(void) {
  br_mqtt_client client;
  unsigned char packet[512];
  int sockets[2];
  ssize_t nread;

  br_mqtt_client_init(&client,
                      "mqtt://broker.local:1884",
                      "client-01",
                      "user",
                      "pass",
                      "will/topic",
                      "{\"online\":false}",
                      0,
                      NULL,
                      NULL);
  assert_string(client.host, "broker.local", "mqtt client host");
  assert_true(client.port == 1884, "mqtt client port");
  assert_true(client.keepalive_seconds == 30, "mqtt default keepalive");
  assert_true(client.next_packet_id == 1, "mqtt initial packet id");
  assert_true(!client.connected && client.socket_fd == -1, "mqtt starts disconnected");
  br_mqtt_client_close(&client);

  assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "mqtt socketpair");
  br_mqtt_client_init(&client, "mqtt://localhost:1883", "client", "", "", "", "", 15, NULL, NULL);
  client.socket_fd = sockets[0];
  client.connected = true;
  assert_true(br_mqtt_client_publish(&client, "topic/a", "{\"ok\":true}", true) == 0,
              "mqtt publish");
  nread = read(sockets[1], packet, sizeof(packet));
  assert_true(nread > 0, "mqtt publish packet read");
  assert_true(packet[0] == 0x31U, "mqtt publish retain header");
  assert_true(bytes_contain_text(packet, (size_t) nread, "topic/a"), "mqtt publish topic");
  assert_true(bytes_contain_text(packet, (size_t) nread, "{\"ok\":true}"), "mqtt publish payload");

  assert_true(br_mqtt_client_subscribe(&client, "topic/#") == 0, "mqtt subscribe");
  nread = read(sockets[1], packet, sizeof(packet));
  assert_true(nread > 0, "mqtt subscribe packet read");
  assert_true(packet[0] == 0x82U, "mqtt subscribe header");
  assert_true(bytes_contain_text(packet, (size_t) nread, "topic/#"), "mqtt subscribe topic");

  assert_true(br_mqtt_client_unsubscribe(&client, "topic/#") == 0, "mqtt unsubscribe");
  nread = read(sockets[1], packet, sizeof(packet));
  assert_true(nread > 0, "mqtt unsubscribe packet read");
  assert_true(packet[0] == 0xa2U, "mqtt unsubscribe header");
  assert_true(bytes_contain_text(packet, (size_t) nread, "topic/#"), "mqtt unsubscribe topic");

  br_mqtt_client_close(&client);
  close(sockets[1]);

  assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "mqtt poll socketpair");
  mqtt_recv recv = {0};
  br_mqtt_client_init(&client, "mqtt://localhost:1883", "client", "", "", "", "", 15,
                      mqtt_on_publish, &recv);
  client.socket_fd = sockets[0];
  client.connected = true;
  client.last_write_ms = br_now_ms();

  const char *topic = "incoming/topic";
  const char *payload = "{\"value\":42}";
  size_t topic_len = strlen(topic);
  size_t payload_len = strlen(payload);
  size_t remaining = 2 + topic_len + payload_len;
  size_t used = 0;
  packet[used++] = 0x30U;
  packet[used++] = (unsigned char) remaining;
  packet[used++] = (unsigned char) ((topic_len >> 8U) & 0xffU);
  packet[used++] = (unsigned char) (topic_len & 0xffU);
  memcpy(packet + used, topic, topic_len);
  used += topic_len;
  memcpy(packet + used, payload, payload_len);
  used += payload_len;
  assert_true(write(sockets[1], packet, used) == (ssize_t) used, "write incoming mqtt packet");
  assert_true(br_mqtt_client_poll(&client, 0) == 0, "mqtt poll incoming publish");
  assert_true(recv.call_count == 1, "mqtt callback count");
  assert_string(recv.topic, "incoming/topic", "mqtt callback topic");
  assert_string(recv.payload, "{\"value\":42}", "mqtt callback payload");

  br_mqtt_client_close(&client);
  close(sockets[1]);
}

static void test_usb_serial_raw_send_uses_line_protocol(void) {
  int sockets[2];
  br_usb_serial serial;
  char buffer[256] = {0};
  ssize_t nread;

  assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "usb raw socketpair");
  memset(&serial, 0, sizeof(serial));
  serial.fd = sockets[0];
  serial.connected = true;
  assert_true(br_usb_serial_send_raw(&serial, "{\"topic\":\"raw\",\"payload\":{}}") == 0,
              "usb raw send");
  nread = read(sockets[1], buffer, sizeof(buffer) - 1);
  assert_true(nread > 0, "usb raw read");
  assert_string(buffer, "{\"topic\":\"raw\",\"payload\":{}}\n", "usb raw line");
  br_usb_serial_close(&serial);
  close(sockets[1]);
}

int main(void) {
  test_common_normalization_paths_and_mqtt_urls();
  test_common_asset_checksum_hex();
  test_common_file_io_device_id_and_payload_write();
  test_runtime_json_helpers();
  test_protocol_builders_and_payload_wrapping();
  test_debug_overlay_state_files();
  test_pairing_discovery_branches();
  test_touch_gesture_branches_and_names();
  test_voice_button_aliases_and_rotary_edges();
  test_mqtt_client_init_publish_subscribe_and_poll();
  test_usb_serial_raw_send_uses_line_protocol();
  printf("runtime core tests passed\n");
  return 0;
}
