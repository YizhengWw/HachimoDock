#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "runtime_common.h"
#include "runtime_debug.h"
#include "runtime_pairing.h"
#include "runtime_protocol.h"
#include "runtime_session_state.h"
#include "runtime_stats.h"
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

static void test_normalize_topic_part(void) {
  char output[128];
  assert_true(br_normalize_topic_part(" /desk/linux pet/ ", "fallback", output, sizeof(output)), "normalize topic");
  assert_true(strcmp(output, "desk-linux-pet") == 0, "normalized topic content");
}

static void test_parse_input_action(void) {
  br_input_action action;
  char error[64];
  const char *json = "{\"type\":\"tap\",\"x\":10,\"y\":20,\"view\":\"main\"}";
  assert_true(br_parse_input_action_json(json, &action, error, sizeof(error)), "parse input action");
  assert_true(strcmp(action.type, "tap") == 0, "action type");
  assert_true(action.has_x && (int) action.x == 10, "action x");
  assert_true(action.has_y && (int) action.y == 20, "action y");
  assert_true(strcmp(action.view, "main") == 0, "action view");
}

static void test_remote_binding(void) {
  br_remote_binding binding;
  const char *json = "{\"command\":\"remote_cli_binding.update\",\"enabled\":true,\"targetDeviceId\":\"desktop-01\",\"targetSource\":\"active\"}";
  assert_true(br_parse_remote_binding_json(json, &binding), "parse remote binding");
  assert_true(strcmp(binding.target_device_id, "desktop-01") == 0, "binding target");
  assert_true(strcmp(binding.target_source, "active") == 0, "binding source");
}

static void test_audio_bridge_command(void) {
  br_audio_bridge_command command;
  const char *json = "{\"type\":\"audio_bridge\",\"action\":\"start\",\"pc_ip\":\"192.0.2.10\",\"pc_port\":50001,\"listen_port\":50002,\"voice_button\":\"encoder_button.hold\",\"capture_dev\":\"hw:1,0\",\"play_dev\":\"default\"}";
  assert_true(br_parse_audio_bridge_command_json(json, &command), "parse audio bridge command");
  assert_true(command.enabled, "audio bridge start enabled");
  assert_true(strcmp(command.pc_ip, "192.0.2.10") == 0, "audio bridge pc ip");
  assert_true(command.pc_port == 50001, "audio bridge pc port");
  assert_true(command.listen_port == 50002, "audio bridge listen port");
  assert_true(strcmp(command.voice_button, "encoder_button.hold") == 0, "audio bridge voice button");
  assert_true(strcmp(command.capture_dev, "hw:1,0") == 0, "audio bridge capture dev");
  assert_true(strcmp(command.play_dev, "default") == 0, "audio bridge play dev");

  assert_true(br_parse_audio_bridge_command_json("{\"type\":\"audio_bridge\",\"action\":\"stop\"}", &command),
              "parse audio bridge stop");
  assert_true(!command.enabled, "audio bridge stop disabled");

  assert_true(br_voice_button_normalize("encoder_button.hold", command.voice_button, sizeof(command.voice_button)),
              "normalize encoder voice button");
  assert_true(strcmp(command.voice_button, "encoder_button.hold") == 0, "encoder voice button accepted");
  assert_true(!br_voice_button_normalize("button.primary.short_press", command.voice_button, sizeof(command.voice_button)),
              "widget button event is not a voice button");
}

static void test_button_config_command(void) {
  br_button_config_command command;
  char action[64];
  const char *json =
    "{\"type\":\"button_config\",\"version\":1,\"voiceButton\":\"encoder_button.hold\","
    "\"voiceEnabled\":true,\"requestId\":\"button-config-123\",\"bindings\":["
    "{\"event\":\"button.encoder.short_press\",\"action\":\"system_page\"},"
    "{\"event\":\"button.encoder.long_press\",\"action\":\"system_reset\"},"
    "{\"event\":\"knob.rotate_cw / knob.rotate_ccw\",\"action\":\"volume_adjust\"},"
    "{\"event\":\"screen.region.tap\",\"action\":\"disabled\"},"
    "{\"event\":\"screen.region.long_press\",\"action\":\"disabled\"}"
    "]}";

  assert_true(br_parse_button_config_command_json(json, &command), "parse button config command");
  assert_true(command.version == 1, "button config version");
  assert_true(command.voice_enabled, "button config voice enabled");
  assert_true(strcmp(command.voice_button, "encoder_button.hold") == 0, "button config voice button");
  assert_true(strcmp(command.request_id, "button-config-123") == 0, "button config request id");
  assert_true(command.binding_count == 5, "button config has all visible bindings");
  assert_true(strcmp(command.bindings[0].event, "button.encoder.short_press") == 0,
              "button config encoder short event");
  assert_true(strcmp(command.bindings[0].action, "system_page") == 0,
              "button config encoder short action");

  assert_true(br_button_config_find_action_json(json,
                                                "button.encoder.short_press",
                                                action,
                                                sizeof(action)),
              "find encoder short action");
  assert_true(strcmp(action, "system_page") == 0, "encoder short action lookup");
  assert_true(br_button_config_find_action_json(json,
                                                "knob.rotate_cw / knob.rotate_ccw",
                                                action,
                                                sizeof(action)),
              "find knob action");
  assert_true(strcmp(action, "volume_adjust") == 0, "knob action lookup");
  assert_true(!br_parse_button_config_command_json(
                "{\"type\":\"button_config\",\"bindings\":[{\"event\":\"button.primary.short_press\",\"action\":\"voice_ptt\"}]}",
                &command),
              "primary button config event is rejected");
  assert_true(!br_parse_button_config_command_json(
                "{\"type\":\"button_config\",\"bindings\":[{\"event\":\"screen.region.tap\",\"action\":\"negative_screen_primary\"}]}",
                &command),
              "negative-screen remap action is rejected");
  assert_true(!br_parse_button_config_command_json(
                "{\"type\":\"button_config\",\"bindings\":[{\"event\":\"button.encoder.short_press\",\"action\":\"shell_rm_rf\"}]}",
                &command),
              "reject unknown button action");
}

static void test_parse_speech_text_card_payload(void) {
  char output[1024];
  br_speech_update update;
  assert_true(
    br_parse_speech_text(
      "{\"title\":\"Analyze Insta Mic Pro\",\"content\":\"Thinking\"}",
      output,
      sizeof(output)
    ),
    "parse speech card"
  );
  assert_true(strcmp(output, "Analyze Insta Mic Pro\nThinking") == 0, "speech card title and content");

  assert_true(
    br_parse_speech_text("{\"sessionName\":\"Session A\",\"displayStatus\":\"Done\"}", output, sizeof(output)),
    "parse speech card aliases"
  );
  assert_true(strcmp(output, "Session A\nDone") == 0, "speech card alias fields");

  assert_true(br_parse_speech_text("{\"content\":\"Plain speech\"}", output, sizeof(output)), "parse plain content");
  assert_true(strcmp(output, "Plain speech") == 0, "plain content remains one line");

  assert_true(
    br_parse_speech_update(
      "{\"displayTitle\":\"Session A\",\"displayContent\":\"Done\","
      "\"source\":\"codex\",\"sessionId\":\"session-a\",\"tsMs\":123,\"expiresAtMs\":456}",
      &update
    ),
    "parse speech update"
  );
  assert_true(strcmp(update.source, "codex") == 0, "speech source");
  assert_true(strcmp(update.session_id, "session-a") == 0, "speech session id");
  assert_true(strcmp(update.title, "Session A") == 0, "speech title");
  assert_true(strcmp(update.body, "Done") == 0, "speech body");
  assert_true(strcmp(update.text, "Session A\nDone") == 0, "speech text");
  assert_true(update.has_payload_ts_ms && update.payload_ts_ms == 123, "speech ts");
  assert_true(update.has_expires_at_ms && update.expires_at_ms == 456, "speech expires");
}

static void test_bridge_state(void) {
  br_bridge_state_update update;
  const char *json = "{\"payload\":{\"state\":\"tool_running\",\"event\":\"PreToolUse\",\"source\":\"openclaw\",\"tsMs\":1000}}";
  assert_true(br_bridge_state_from_message("desk/linux-pet-01/state/active", json, &update), "bridge state parse");
  assert_true(update.should_write, "bridge state should write");
  assert_true(update.allow_interrupt, "bridge state interrupts by default");
  assert_true(strcmp(update.state, "working") == 0, "bridge state canonical value");
  assert_true(strcmp(update.event, "PreToolUse") == 0, "bridge event");
}

static void test_bridge_state_interrupt_flag(void) {
  br_bridge_state_update update;
  const char *json = "{\"payload\":{\"state\":\"tool_running\",\"screenInterrupt\":false,\"source\":\"active\",\"tsMs\":1000}}";
  assert_true(br_bridge_state_from_message("desk/linux-pet-01/state/active", json, &update), "bridge state parse interrupt flag");
  assert_true(update.should_write, "bridge state interrupt flag should write");
  assert_true(!update.allow_interrupt, "bridge state interrupt disabled");
  assert_true(strcmp(update.state, "working") == 0, "bridge interrupt flag state canonical");
}

static void test_bridge_state_session_identity(void) {
  br_bridge_state_update update;
  const char *json =
    "{\"payload\":{\"state\":\"done\",\"event\":\"AssistantMessage\","
    "\"source\":\"codex\",\"sessionId\":\"session-a\",\"runId\":\"run-a\","
    "\"sessionKey\":\"key-a\",\"reason\":\"codex.done\",\"tsMs\":12345}}";

  assert_true(br_bridge_state_from_message("desk/devbox/state/codex", json, &update), "bridge state identity parse");
  assert_true(update.should_write, "identity update should write");
  assert_true(strcmp(update.state, "done") == 0, "identity state");
  assert_true(strcmp(update.source, "codex") == 0, "identity source");
  assert_true(strcmp(update.session_id, "session-a") == 0, "identity session id");
  assert_true(strcmp(update.run_id, "run-a") == 0, "identity run id");
  assert_true(strcmp(update.session_key, "key-a") == 0, "identity session key");
  assert_true(strcmp(update.reason, "codex.done") == 0, "identity reason");
  assert_true(update.has_payload_ts_ms && update.payload_ts_ms == 12345, "identity ts");
}

static void test_bridge_state_canonical_from_event(void) {
  br_bridge_state_update update;

  assert_true(br_bridge_state_from_message(
    "desk/linux-pet-01/state/active",
    "{\"payload\":{\"event\":\"UserPromptSubmit\",\"source\":\"openclaw\",\"tsMs\":1000}}",
    &update
  ), "user prompt event parse");
  assert_true(strcmp(update.state, "working") == 0, "user prompt maps to working");

  assert_true(br_bridge_state_from_message(
    "desk/linux-pet-01/state/active",
    "{\"payload\":{\"event\":\"PermissionRequest\",\"source\":\"openclaw\",\"tsMs\":1001}}",
    &update
  ), "permission event parse");
  assert_true(strcmp(update.state, "waiting_user") == 0, "permission maps to waiting_user");

  assert_true(br_bridge_state_from_message(
    "desk/linux-pet-01/state/active",
    "{\"payload\":{\"event\":\"AssistantMessage\",\"source\":\"openclaw\",\"tsMs\":1002}}",
    &update
  ), "assistant message event parse");
  assert_true(strcmp(update.state, "done") == 0, "assistant message maps to done");

  assert_true(br_bridge_state_from_message(
    "desk/linux-pet-01/state/active",
    "{\"payload\":{\"event\":\"StopFailure\",\"source\":\"openclaw\",\"tsMs\":1003}}",
    &update
  ), "failure event parse");
  assert_true(strcmp(update.state, "error") == 0, "failure maps to error");
}

static void test_bridge_state_canonical_from_legacy_state(void) {
  br_bridge_state_update update;

  assert_true(br_bridge_state_from_message(
    "desk/linux-pet-01/state/active",
    "{\"payload\":{\"state\":\"speaking\",\"event\":\"AssistantDelta\"}}",
    &update
  ), "speaking state parse");
  assert_true(strcmp(update.state, "working") == 0, "speaking maps to working");

  assert_true(br_bridge_state_from_message(
    "desk/linux-pet-01/state/active",
    "{\"payload\":{\"state\":\"notification\"}}",
    &update
  ), "notification state parse");
  assert_true(strcmp(update.state, "waiting_user") == 0, "notification maps to waiting_user");

  assert_true(br_bridge_state_from_message(
    "desk/linux-pet-01/state/active",
    "{\"payload\":{\"state\":\"active\"}}",
    &update
  ), "active state parse");
  assert_true(strcmp(update.state, "working") == 0, "active maps to working");
}

static void test_bridge_state_token_usage_top_level_camel_case(void) {
  br_bridge_state_update update;
  const char *json =
    "{\"state\":\"tool_running\",\"event\":\"event_msg:token_count\",\"source\":\"codex\","
    "\"sessionId\":\"codex:abc\",\"tokenUsage\":{\"inputTokens\":47700096,\"outputTokens\":214114,"
    "\"cachedInputTokens\":45144960,\"reasoningOutputTokens\":52743,\"totalTokens\":47914210,"
    "\"lastInputTokens\":1200,\"lastOutputTokens\":80,\"lastCachedInputTokens\":900,"
    "\"lastReasoningOutputTokens\":20,\"lastTotalTokens\":1280,\"modelContextWindow\":258400}}";
  assert_true(br_bridge_state_from_message("desk/board-x/state/codex", json, &update),
              "token usage top-level parse");
  assert_true(update.has_token_usage, "token usage flagged");
  assert_true(update.token_usage.has_total_tokens && update.token_usage.total_tokens == 47914210LL,
              "token usage total");
  assert_true(update.token_usage.has_input_tokens && update.token_usage.input_tokens == 47700096LL,
              "token usage input");
  assert_true(update.token_usage.has_output_tokens && update.token_usage.output_tokens == 214114LL,
              "token usage output");
  assert_true(update.token_usage.has_cached_input_tokens && update.token_usage.cached_input_tokens == 45144960LL,
              "token usage cached");
  assert_true(update.token_usage.has_reasoning_output_tokens && update.token_usage.reasoning_output_tokens == 52743LL,
              "token usage reasoning");
  assert_true(update.token_usage.has_last_total_tokens && update.token_usage.last_total_tokens == 1280LL,
              "token usage last total");
  assert_true(update.token_usage.has_last_input_tokens && update.token_usage.last_input_tokens == 1200LL,
              "token usage last input");
  assert_true(update.token_usage.has_last_output_tokens && update.token_usage.last_output_tokens == 80LL,
              "token usage last output");
  assert_true(update.token_usage.has_last_cached_input_tokens
                && update.token_usage.last_cached_input_tokens == 900LL,
              "token usage last cached");
  assert_true(update.token_usage.has_last_reasoning_output_tokens
                && update.token_usage.last_reasoning_output_tokens == 20LL,
              "token usage last reasoning");
  assert_true(!update.token_usage.has_estimated_cost_usd, "token usage cost missing");
}

static void test_bridge_state_token_usage_snake_case_in_payload_wrapper(void) {
  br_bridge_state_update update;
  const char *json =
    "{\"payload\":{\"state\":\"working\",\"event\":\"PreToolUse\",\"source\":\"openclaw\","
    "\"tokenUsage\":{\"input_tokens\":120,\"output_tokens\":30,\"cache_creation_input_tokens\":5,"
    "\"estimated_cost_usd\":0.12}}}";
  assert_true(br_bridge_state_from_message("desk/board-x/state/active", json, &update),
              "token usage snake-case parse");
  assert_true(update.has_token_usage, "snake-case token usage flagged");
  assert_true(update.token_usage.has_input_tokens && update.token_usage.input_tokens == 120LL,
              "snake-case input");
  assert_true(update.token_usage.has_output_tokens && update.token_usage.output_tokens == 30LL,
              "snake-case output");
  assert_true(update.token_usage.has_cache_creation_input_tokens
                && update.token_usage.cache_creation_input_tokens == 5LL,
              "snake-case cache creation");
  assert_true(update.token_usage.has_total_tokens && update.token_usage.total_tokens == 155LL,
              "derived total");
  assert_true(update.token_usage.has_estimated_cost_usd && update.token_usage.estimated_cost_usd > 0.119
                && update.token_usage.estimated_cost_usd < 0.121,
              "snake-case cost usd");
}

static void test_bridge_state_token_usage_missing(void) {
  br_bridge_state_update update;
  const char *json =
    "{\"state\":\"idle\",\"event\":\"process.missing\",\"source\":\"claude-code\","
    "\"sessionId\":\"claude:local\"}";
  assert_true(br_bridge_state_from_message("desk/board-x/state/claude-code", json, &update),
              "missing token usage parse");
  assert_true(!update.has_token_usage, "missing token usage flag");
}

static void test_session_state_done_expires_to_idle(void) {
  br_session_machine machine;
  br_bridge_state_update update;
  br_session_resolution resolution;

  br_session_machine_init(&machine, 3000, 60000);
  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "codex");
  strcpy(update.session_id, "session-a");
  strcpy(update.state, "done");
  strcpy(update.event, "AssistantMessage");
  strcpy(update.reason, "codex.done");
  update.payload_ts_ms = 10000;
  update.has_payload_ts_ms = true;

  assert_true(br_session_machine_apply(&machine, &update, 10000, &resolution), "done apply changed");
  assert_true(strcmp(resolution.state, "done") == 0, "done is active");
  assert_true(resolution.should_interrupt, "done should interrupt into completion affordance");

  assert_true(!br_session_machine_tick(&machine, 12999, &resolution), "done not expired before hold");
  assert_true(br_session_machine_tick(&machine, 13001, &resolution), "done expires after hold");
  assert_true(strcmp(resolution.state, "idle") == 0, "done expires to idle");
  assert_true(!resolution.should_interrupt, "expiry must not interrupt");
}

static void test_session_state_done_reveals_existing_working_without_interrupt(void) {
  br_session_machine machine;
  br_bridge_state_update update;
  br_session_resolution resolution;

  br_session_machine_init(&machine, 3000, 60000);

  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "openclaw");
  strcpy(update.session_id, "working-session");
  strcpy(update.state, "working");
  strcpy(update.event, "PreToolUse");
  update.payload_ts_ms = 20000;
  update.has_payload_ts_ms = true;
  assert_true(br_session_machine_apply(&machine, &update, 20000, &resolution), "working apply changed");
  assert_true(strcmp(resolution.state, "working") == 0, "working active");
  assert_true(!resolution.should_interrupt, "push into working waits for clip boundary");

  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "codex");
  strcpy(update.session_id, "done-session");
  strcpy(update.state, "done");
  strcpy(update.event, "AssistantMessage");
  update.payload_ts_ms = 21000;
  update.has_payload_ts_ms = true;
  assert_true(br_session_machine_apply(&machine, &update, 21000, &resolution), "done apply changed");
  assert_true(strcmp(resolution.state, "done") == 0, "done outranks working while held");
  assert_true(resolution.should_interrupt, "done push interrupts into completion affordance");

  assert_true(br_session_machine_tick(&machine, 24001, &resolution), "done expiry changes active");
  assert_true(strcmp(resolution.state, "working") == 0, "working revealed");
  assert_true(!resolution.should_interrupt, "expiry reveal does not interrupt");
}

static void test_session_state_done_reveals_same_source_working(void) {
  br_session_machine machine;
  br_bridge_state_update update;
  br_session_resolution resolution;

  br_session_machine_init(&machine, 3000, 60000);

  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "codex");
  strcpy(update.session_id, "old-working-session");
  strcpy(update.state, "working");
  strcpy(update.event, "event_msg:agent_message");
  update.payload_ts_ms = 20000;
  update.has_payload_ts_ms = true;
  assert_true(br_session_machine_apply(&machine, &update, 20000, &resolution), "old working apply changed");
  assert_true(strcmp(resolution.state, "working") == 0, "old working active");

  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "codex");
  strcpy(update.session_id, "done-session");
  strcpy(update.state, "done");
  strcpy(update.event, "event_msg:task_complete");
  update.payload_ts_ms = 21000;
  update.has_payload_ts_ms = true;
  assert_true(br_session_machine_apply(&machine, &update, 21000, &resolution), "done apply changed");
  assert_true(strcmp(resolution.state, "done") == 0, "done active");

  assert_true(br_session_machine_tick(&machine, 24001, &resolution), "done expiry changes active");
  assert_true(strcmp(resolution.state, "working") == 0, "same source older working can remain visible");
}

static void test_session_state_latest_update_wins_over_priority(void) {
  br_session_machine machine;
  br_bridge_state_update update;
  br_session_resolution resolution;

  br_session_machine_init(&machine, 3000, 60000);

  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "codex");
  strcpy(update.session_id, "waiting-session");
  strcpy(update.state, "waiting_user");
  strcpy(update.event, "PermissionRequest");
  update.payload_ts_ms = 20000;
  update.has_payload_ts_ms = true;
  assert_true(br_session_machine_apply(&machine, &update, 20000, &resolution), "waiting apply changed");
  assert_true(strcmp(resolution.state, "waiting_user") == 0, "waiting active");

  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "codex");
  strcpy(update.session_id, "working-session");
  strcpy(update.state, "working");
  strcpy(update.event, "PreToolUse");
  update.payload_ts_ms = 21000;
  update.has_payload_ts_ms = true;
  assert_true(br_session_machine_apply(&machine, &update, 21000, &resolution), "newer working apply changed");
  assert_true(strcmp(resolution.state, "working") == 0, "newer lower-priority state wins");
  assert_true(strcmp(resolution.active_key, "codex:session:working-session") == 0, "newest session key active");
}

static void test_session_state_keeps_working_during_short_idle_gap(void) {
  br_session_machine machine;
  br_bridge_state_update update;
  br_session_resolution resolution;

  br_session_machine_init(&machine, 3000, 60000);

  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "codex");
  strcpy(update.session_id, "session-a");
  strcpy(update.state, "working");
  strcpy(update.event, "PreToolUse");
  update.payload_ts_ms = 10000;
  update.has_payload_ts_ms = true;
  assert_true(br_session_machine_apply(&machine, &update, 10000, &resolution),
              "working apply changed");
  assert_true(strcmp(resolution.state, "working") == 0, "working is active");

  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "codex");
  strcpy(update.session_id, "session-a");
  strcpy(update.state, "idle");
  strcpy(update.event, "PostToolUse");
  update.payload_ts_ms = 11000;
  update.has_payload_ts_ms = true;
  assert_true(!br_session_machine_apply(&machine, &update, 11000, &resolution),
              "idle inside working buffer does not change visible state");
  assert_true(strcmp(resolution.state, "working") == 0, "short idle gap remains working");

  assert_true(!br_session_machine_tick(&machine, 12999, &resolution),
              "working buffer still active before three seconds");
  assert_true(strcmp(resolution.state, "working") == 0, "tick before buffer keeps working");

  assert_true(br_session_machine_tick(&machine, 13001, &resolution),
              "working buffer expires after three seconds");
  assert_true(strcmp(resolution.state, "idle") == 0, "idle becomes visible after buffer");
}

static void test_session_state_idle_probe_cannot_preempt_working(void) {
  br_session_machine machine;
  br_bridge_state_update update;
  br_session_resolution resolution;

  br_session_machine_init(&machine, 3000, 60000);

  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "codex");
  strcpy(update.session_id, "codex-active");
  strcpy(update.state, "working");
  strcpy(update.event, "event_msg:token_count");
  update.payload_ts_ms = 20000;
  update.has_payload_ts_ms = true;
  assert_true(br_session_machine_apply(&machine, &update, 20000, &resolution), "codex working apply changed");
  assert_true(strcmp(resolution.state, "working") == 0, "codex working active");

  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "claude-code");
  strcpy(update.session_id, "claude:local");
  strcpy(update.state, "idle");
  strcpy(update.event, "process.missing");
  update.payload_ts_ms = 21000;
  update.has_payload_ts_ms = true;
  assert_true(!br_session_machine_apply(&machine, &update, 21000, &resolution),
              "newer idle probe does not change active session");
  assert_true(strcmp(resolution.state, "working") == 0, "working remains active after idle probe");
  assert_true(strcmp(resolution.active_key, "codex:session:codex-active") == 0,
              "idle probe cannot preempt active codex session");

  br_session_machine_init(&machine, 3000, 60000);
  assert_true(br_session_machine_apply(&machine, &update, 21000, &resolution),
              "idle probe is fallback when it is the only record");
  assert_true(strcmp(resolution.state, "idle") == 0, "lone idle probe resolves idle");
  assert_true(strcmp(resolution.active_key, "claude-code:session:claude:local") == 0,
              "lone idle probe remains visible in debug");
}

static void test_session_state_ignores_expired_retained_done(void) {
  br_session_machine machine;
  br_bridge_state_update update;
  br_session_resolution resolution;

  br_session_machine_init(&machine, 3000, 60000);
  memset(&update, 0, sizeof(update));
  update.should_write = true;
  update.allow_interrupt = true;
  strcpy(update.source, "codex");
  strcpy(update.session_id, "old-done");
  strcpy(update.state, "done");
  strcpy(update.event, "AssistantMessage");
  update.payload_ts_ms = 10000;
  update.has_payload_ts_ms = true;

  assert_true(!br_session_machine_apply(&machine, &update, 14000, &resolution), "expired retained done ignored");
  assert_true(strcmp(resolution.state, "idle") == 0, "expired retained done keeps idle");
}

static void test_touch_gesture(void) {
  br_touch_gesture_state gesture;
  br_touch_action action;
  br_touch_gesture_init(&gesture, 40, 5000);
  br_touch_gesture_set_position(&gesture, 100, 100);
  br_touch_gesture_start(&gesture, 0);
  br_touch_gesture_set_position(&gesture, 170, 110);
  assert_true(br_touch_gesture_finish(&gesture, 300, &action), "touch finish");
  assert_true(action.type == BR_TOUCH_SWIPE_RIGHT, "touch swipe right");
}

static void test_touch_swipe_toggles_screen_page(void) {
  char page[32];
  assert_true(strcmp(br_screen_page_default_page(), "main") == 0, "default page is main");
  assert_true(br_screen_page_toggle_main_stats("main", page, sizeof(page)), "toggle from main");
  assert_true(strcmp(page, "stats") == 0, "main toggles to stats");
  assert_true(br_screen_page_toggle_main_stats("stats", page, sizeof(page)), "toggle from stats");
  assert_true(strcmp(page, "main") == 0, "stats toggles to main");
  assert_true(br_screen_page_toggle_main_stats("", page, sizeof(page)), "toggle from empty");
  assert_true(strcmp(page, "stats") == 0, "empty toggles to stats");
  assert_true(br_screen_page_toggle_main_stats("unknown", page, sizeof(page)), "toggle from unknown");
  assert_true(strcmp(page, "stats") == 0, "unknown toggles to stats");
  assert_true(br_screen_page_touch_action_should_toggle("swipe_left"), "left swipe toggles");
  assert_true(br_screen_page_touch_action_should_toggle("swipe_right"), "right swipe toggles");
  assert_true(br_screen_page_touch_action_should_toggle("swipe_up"), "up swipe toggles");
  assert_true(br_screen_page_touch_action_should_toggle("swipe_down"), "down swipe toggles");
  assert_true(!br_screen_page_touch_action_should_toggle("tap"), "tap does not toggle");
  assert_true(!br_screen_page_touch_action_should_toggle("long_press"), "long press does not toggle");
}

static void test_screen_page_resolution_accepts_home_alias(void) {
  char page[32];
  assert_true(br_screen_page_resolve("home", page, sizeof(page)), "home alias resolves");
  assert_true(strcmp(page, "main") == 0, "home aliases main");
  assert_true(br_screen_page_resolve("{\"page\":\"home\"}", page, sizeof(page)), "json home alias resolves");
  assert_true(strcmp(page, "main") == 0, "json home aliases main");
}

static void test_button_press_duration_classification(void) {
  assert_true(br_button_press_resolve_threshold_ms(0, 8000) == 8000, "rotary reset threshold default");
  assert_true(br_button_press_resolve_threshold_ms(9000, 8000) == 9000, "rotary reset threshold override");
  assert_true(br_button_press_classify(0, 1500) == BR_BUTTON_PRESS_SHORT, "zero duration is short");
  assert_true(br_button_press_classify(1499, 1500) == BR_BUTTON_PRESS_SHORT, "below threshold is short");
  assert_true(br_button_press_classify(1500, 1500) == BR_BUTTON_PRESS_LONG, "threshold is long");
  assert_true(br_button_press_classify(3000, 1500) == BR_BUTTON_PRESS_LONG, "above threshold is long");
  assert_true(br_button_press_classify(800, 0) == BR_BUTTON_PRESS_SHORT, "invalid threshold uses default");
  assert_true(br_button_press_classify(7999, br_button_press_resolve_threshold_ms(0, 8000)) == BR_BUTTON_PRESS_SHORT,
              "rotary reset below 8s is short");
  assert_true(br_button_press_classify(8000, br_button_press_resolve_threshold_ms(0, 8000)) == BR_BUTTON_PRESS_LONG,
              "rotary reset at 8s is long");
}

static void test_primary_button_behavior_ignores_active_widget(void) {
  assert_true(br_primary_button_resolve_action(BR_BUTTON_PRESS_SHORT, false) == BR_PRIMARY_BUTTON_TOGGLE_PAGE,
              "short press toggles page without widget");
  assert_true(br_primary_button_resolve_action(BR_BUTTON_PRESS_SHORT, true) == BR_PRIMARY_BUTTON_TOGGLE_PAGE,
              "short press toggles page even with active stats widget");
  assert_true(br_primary_button_resolve_action(BR_BUTTON_PRESS_LONG, true) == BR_PRIMARY_BUTTON_RESTART_RUNTIME,
              "long press restarts even with active stats widget");
}

static void test_pairing_machine_transitions(void) {
  br_pairing_machine machine;
  br_pairing_init(&machine, false, 1000, 0);
  assert_true(machine.state == BR_PAIRING_WAITING_CONFIG, "pairing init waiting");

  assert_true(br_pairing_tick(&machine, 0), "pairing no config enters ap");
  assert_true(machine.state == BR_PAIRING_AP_FALLBACK, "pairing ap fallback");

  assert_true(br_pairing_apply_config(&machine, 1200), "pairing apply config");
  assert_true(machine.state == BR_PAIRING_STA_READY, "pairing sta ready");

  assert_true(br_pairing_reset_to_waiting(&machine, 1300), "pairing reset waiting");
  assert_true(machine.state == BR_PAIRING_WAITING_CONFIG, "pairing waiting after reset");
}

static void test_pairing_machine_valid_config_boot(void) {
  br_pairing_machine machine;
  br_pairing_init(&machine, true, 1000, 0);
  assert_true(machine.state == BR_PAIRING_STA_READY, "pairing init sta ready");
  assert_true(strcmp(br_pairing_mode_name(machine.state), "sta") == 0, "pairing mode sta");
  assert_true(!br_pairing_is_waiting(machine.state), "pairing not waiting");
}

static char br_test_tmp_root[256];

static void br_test_make_tmp_root(void) {
  snprintf(br_test_tmp_root, sizeof(br_test_tmp_root),
           "/tmp/board-runtime-tests-%d", (int) getpid());
  /* mkdir -p */
  char cmd[300];
  snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", br_test_tmp_root, br_test_tmp_root);
  if (system(cmd) != 0) {
    fprintf(stderr, "failed to prepare tmp root\n");
    exit(1);
  }
}

static void br_test_cleanup_tmp_root(void) {
  char cmd[300];
  snprintf(cmd, sizeof(cmd), "rm -rf %s", br_test_tmp_root);
  (void) system(cmd);
}

static void br_test_make_token_update(
  br_bridge_state_update *update,
  const char *source,
  const char *session_key,
  long long total,
  long long input,
  long long output
) {
  memset(update, 0, sizeof(*update));
  update->should_write = true;
  update->allow_interrupt = true;
  snprintf(update->source, sizeof(update->source), "%s", source);
  snprintf(update->session_key, sizeof(update->session_key), "%s", session_key);
  snprintf(update->state, sizeof(update->state), "working");
  update->has_token_usage = true;
  update->token_usage.total_tokens = total;
  update->token_usage.has_total_tokens = true;
  update->token_usage.input_tokens = input;
  update->token_usage.has_input_tokens = true;
  update->token_usage.output_tokens = output;
  update->token_usage.has_output_tokens = true;
}

static void test_runtime_stats_delta_accumulation(void) {
  br_test_make_tmp_root();
  /* tz_offset 0：UTC，避免本地时区漂移到上一天 */
  assert_true(runtime_stats_init(br_test_tmp_root, 1000000LL, 0, 1746780000000LL) == 0, "stats init");

  br_bridge_state_update u;
  br_test_make_token_update(&u, "codex", "session-A", 1000, 600, 400);
  /* 第一帧：prev=0，cur=1000，delta=1000 */
  runtime_stats_ingest(&u, 1746780000000LL);

  br_test_make_token_update(&u, "codex", "session-A", 1500, 900, 600);
  /* delta=500 */
  runtime_stats_ingest(&u, 1746780001000LL);

  /* 同 session 重启：cur 突然变小，应当重置 prev，不产生负 delta */
  br_test_make_token_update(&u, "codex", "session-A", 200, 100, 100);
  runtime_stats_ingest(&u, 1746780002000LL);

  /* 新 session：cur=300，按 prev=0 算 delta=300 */
  br_test_make_token_update(&u, "codex", "session-B", 300, 200, 100);
  runtime_stats_ingest(&u, 1746780003000LL);

  /* 不同 source */
  br_test_make_token_update(&u, "claude-code", "session-C", 500, 400, 100);
  runtime_stats_ingest(&u, 1746780004000LL);

  assert_true(runtime_stats_flush(), "stats flush ok");

  char display[1024];
  size_t n = runtime_stats_render_display(display, sizeof(display));
  assert_true(n > 0, "render display non-empty");
  /* 1500-0 + 300 + 500 = 2300 -> dashboard payload for framebuffer renderer. */
  assert_true(strstr(display, "STATS_DASHBOARD_V1") != NULL, "render dashboard marker");
  assert_true(strstr(display, "sources=") != NULL,
              "render contains source line");

  runtime_stats_shutdown();
  br_test_cleanup_tmp_root();
}

static void test_runtime_stats_uses_last_usage_for_existing_codex_session_first_sample(void) {
  br_test_make_tmp_root();
  assert_true(runtime_stats_init(br_test_tmp_root, 1000LL, 0, 1746780000000LL) == 0,
              "stats init last usage");

  br_bridge_state_update u;
  br_test_make_token_update(&u, "codex", "long-session", 1000000LL, 800000LL, 200000LL);
  u.token_usage.last_total_tokens = 1200LL;
  u.token_usage.has_last_total_tokens = true;
  u.token_usage.last_input_tokens = 900LL;
  u.token_usage.has_last_input_tokens = true;
  u.token_usage.last_output_tokens = 300LL;
  u.token_usage.has_last_output_tokens = true;
  runtime_stats_ingest(&u, 1746780000000LL);

  br_test_make_token_update(&u, "codex", "long-session", 1001500LL, 800900LL, 200600LL);
  runtime_stats_ingest(&u, 1746780001000LL);

  char display[1024];
  runtime_stats_render_display(display, sizeof(display));
  assert_true(strstr(display, "metricValue=2.7K") != NULL,
              "first sample uses last usage, next sample uses cumulative delta");

  runtime_stats_shutdown();
  br_test_cleanup_tmp_root();
}

static void test_runtime_stats_dashboard_lunch_branches(void) {
  br_test_make_tmp_root();
  assert_true(runtime_stats_init(br_test_tmp_root, 1000000LL, 0, 1746780000000LL) == 0, "stats init lunch");

  br_bridge_state_update u;
  /* 注入 1.86M tokens：1.9 顿工作午餐分支 */
  br_test_make_token_update(&u, "codex", "session-A", 1860000LL, 1200000LL, 660000LL);
  runtime_stats_ingest(&u, 1746780000000LL);
  char display[1024];
  runtime_stats_render_display(display, sizeof(display));
  assert_true(strstr(display, "lunch=1.9") != NULL, "lunch 1.9 units");
  assert_true(strstr(display, "1.86M") != NULL, "compact M format");

  /* 100M tokens：100.0 顿工作午餐分支 */
  br_test_make_token_update(&u, "codex", "session-A", 100000000LL, 50000000LL, 50000000LL);
  runtime_stats_ingest(&u, 1746780001000LL);
  runtime_stats_render_display(display, sizeof(display));
  assert_true(strstr(display, "lunch=100.0") != NULL, "lunch 100.0 branch");

  runtime_stats_shutdown();
  br_test_cleanup_tmp_root();
}

static void test_runtime_stats_dashboard_payload_for_framebuffer_renderer(void) {
  br_test_make_tmp_root();
  assert_true(runtime_stats_init(br_test_tmp_root, 350000LL, 0, 1746780000000LL) == 0,
              "stats init dashboard payload");

  br_bridge_state_update u;
  br_test_make_token_update(&u, "codex", "session-dashboard", 1300000LL, 900000LL, 400000LL);
  runtime_stats_ingest(&u, 1746780000000LL);

  char display[1024];
  runtime_stats_render_display(display, sizeof(display));
  assert_true(strstr(display, "STATS_DASHBOARD_V1") != NULL, "dashboard payload marker");
  assert_true(strstr(display, "agent=Codex") != NULL, "dashboard agent label");
  assert_true(strstr(display, "lunch=3.7") != NULL, "dashboard lunch conversion");
  assert_true(strstr(display, "headline=约 3.7 顿工作午餐") != NULL, "dashboard headline");
  assert_true(strstr(display, "metricTitle=今日累计 Token") != NULL, "dashboard metric title");
  assert_true(strstr(display, "metricValue=1.30M") != NULL, "dashboard compact token value");

  runtime_stats_shutdown();
  br_test_cleanup_tmp_root();
}

static void test_runtime_stats_ignores_missing_token_usage(void) {
  br_test_make_tmp_root();
  assert_true(runtime_stats_init(br_test_tmp_root, 1000000LL, 0, 1746780000000LL) == 0, "stats init missing");

  br_bridge_state_update u;
  memset(&u, 0, sizeof(u));
  u.should_write = true;
  u.allow_interrupt = true;
  snprintf(u.source, sizeof(u.source), "claude-code");
  snprintf(u.state, sizeof(u.state), "idle");
  /* has_token_usage=false：应当跳过 */
  runtime_stats_ingest(&u, 1746780000000LL);

  char display[1024];
  runtime_stats_render_display(display, sizeof(display));
  assert_true(strstr(display, "STATS_DASHBOARD_V1") != NULL, "no token usage dashboard marker");
  assert_true(strstr(display, "metricValue=0") != NULL, "no token usage stays at zero");
  runtime_stats_shutdown();
  br_test_cleanup_tmp_root();
}

static void test_runtime_stats_persistence(void) {
  br_test_make_tmp_root();
  assert_true(runtime_stats_init(br_test_tmp_root, 1000000LL, 0, 1746780000000LL) == 0, "stats init persist");

  br_bridge_state_update u;
  br_test_make_token_update(&u, "codex", "session-A", 2500000LL, 1500000LL, 1000000LL);
  runtime_stats_ingest(&u, 1746780000000LL);
  assert_true(runtime_stats_flush(), "first flush");
  runtime_stats_shutdown();

  /* 重新 init：应当从 today.json + sessions.json 恢复 */
  assert_true(runtime_stats_init(br_test_tmp_root, 1000000LL, 0, 1746780002000LL) == 0, "re-init");
  /* 同 session 同 cur：delta 应为 0 */
  br_test_make_token_update(&u, "codex", "session-A", 2500000LL, 1500000LL, 1000000LL);
  runtime_stats_ingest(&u, 1746780002000LL);
  char display[1024];
  runtime_stats_render_display(display, sizeof(display));
  assert_true(strstr(display, "lunch=2.5") != NULL, "persisted total preserved");

  /* 增量帧：delta = 500K */
  br_test_make_token_update(&u, "codex", "session-A", 3000000LL, 1800000LL, 1200000LL);
  runtime_stats_ingest(&u, 1746780003000LL);
  runtime_stats_render_display(display, sizeof(display));
  assert_true(strstr(display, "lunch=3.0") != NULL, "delta after persistence");

  runtime_stats_shutdown();
  br_test_cleanup_tmp_root();
}

static void test_runtime_stats_end_to_end_with_real_payload(void) {
  /* 这个 fixture 取自 2026-05-09 真实抓到的 desk/<id>/state/codex retained 帧
   * （已脱敏 sessionId）。验证整个链路：raw payload → br_bridge_state_from_message
   * → runtime_stats_ingest → today.json/.stats-display。 */
  br_test_make_tmp_root();
  long long now_ms = 1746780000000LL;
  assert_true(runtime_stats_init(br_test_tmp_root, 1000000LL, 0, now_ms) == 0,
              "stats init e2e");

  const char *codex_frame =
    "{\"state\":\"tool_running\",\"event\":\"event_msg:token_count\","
    "\"source\":\"codex\",\"sessionId\":\"codex:abc\","
    "\"tokenUsage\":{\"inputTokens\":47700096,\"outputTokens\":214114,"
    "\"cachedInputTokens\":45144960,\"reasoningOutputTokens\":52743,"
    "\"totalTokens\":47914210,\"modelContextWindow\":258400}}";
  br_bridge_state_update update;
  assert_true(br_bridge_state_from_message("desk/board-x/state/codex", codex_frame, &update),
              "parse codex frame");
  assert_true(update.has_token_usage, "codex frame has token usage");
  runtime_stats_ingest(&update, now_ms);

  /* 第二个 source：claude-code，不带 tokenUsage */
  const char *claude_frame =
    "{\"state\":\"idle\",\"event\":\"process.missing\","
    "\"source\":\"claude-code\",\"sessionId\":\"claude:local\"}";
  assert_true(br_bridge_state_from_message("desk/board-x/state/claude-code", claude_frame, &update),
              "parse claude frame");
  assert_true(!update.has_token_usage, "claude frame has no token usage");
  runtime_stats_ingest(&update, now_ms + 1);  /* should be no-op */

  /* 1 亿 token 的极限帧 */
  const char *big_frame =
    "{\"state\":\"tool_running\",\"source\":\"codex\",\"sessionId\":\"codex:big\","
    "\"tokenUsage\":{\"totalTokens\":100000000,\"inputTokens\":50000000,\"outputTokens\":50000000}}";
  assert_true(br_bridge_state_from_message("desk/board-x/state/codex", big_frame, &update),
              "parse big frame");
  runtime_stats_ingest(&update, now_ms + 2);

  assert_true(runtime_stats_flush(), "flush e2e");

  /* 检查 today.json 包含两个 source 的累加 */
  char today[8192];
  char today_path[300];
  snprintf(today_path, sizeof(today_path), "%s/stats/today.json", br_test_tmp_root);
  assert_true(br_read_text_file(today_path, today, sizeof(today)), "read today.json");
  /* total = 47914210 + 100000000 = 147914210 */
  assert_true(strstr(today, "\"totalTokens\":147914210") != NULL, "e2e total tokens");
  assert_true(strstr(today, "\"source\":\"codex\"") != NULL, "e2e codex bucket");

  /* 检查 .stats-display 显示 dashboard payload */
  char display[1024];
  char display_path[300];
  snprintf(display_path, sizeof(display_path), "%s/.stats-display", br_test_tmp_root);
  assert_true(br_read_text_file(display_path, display, sizeof(display)), "read .stats-display");
  assert_true(strstr(display, "STATS_DASHBOARD_V1") != NULL, "e2e dashboard payload");
  assert_true(strstr(display, "147.91M") != NULL || strstr(display, "147.91") != NULL,
              "e2e total compact format");
  assert_true(strstr(display, "codex") != NULL, "e2e codex line");

  runtime_stats_shutdown();
  br_test_cleanup_tmp_root();
}

static void test_runtime_stats_rollover_archives_yesterday(void) {
  br_test_make_tmp_root();
  /* day1 = 1746748800000 ms = 2025-05-09 00:00:00 UTC; day2 +24h */
  long long day1 = 1746748800000LL;
  long long day2 = day1 + 86400000LL;
  assert_true(runtime_stats_init(br_test_tmp_root, 1000000LL, 0, day1) == 0, "stats init day1");

  br_bridge_state_update u;
  br_test_make_token_update(&u, "codex", "session-A", 5000000LL, 3000000LL, 2000000LL);
  runtime_stats_ingest(&u, day1 + 1000);
  assert_true(runtime_stats_flush(), "flush day1");

  /* 第二天的帧到来 → check_rollover 应当把昨日 today.json 归档到 2026-05-09.json，
   * 并把 today 重置。 */
  br_test_make_token_update(&u, "codex", "session-A", 5500000LL, 3300000LL, 2200000LL);
  runtime_stats_ingest(&u, day2 + 1000);
  assert_true(runtime_stats_flush(), "flush day2");

  /* 归档文件存在 */
  char archive_path[256];
  snprintf(archive_path, sizeof(archive_path), "%s/stats/2025-05-09.json", br_test_tmp_root);
  char buf[4096];
  assert_true(br_read_text_file(archive_path, buf, sizeof(buf)), "yesterday archive exists");
  assert_true(strstr(buf, "\"totalTokens\":5000000") != NULL, "archive captures yesterday total");

  /* today 只剩 day2 的 delta = 500000 */
  char display[1024];
  runtime_stats_render_display(display, sizeof(display));
  assert_true(strstr(display, "metricValue=500.0K") != NULL, "today reset after rollover");

  runtime_stats_shutdown();
  br_test_cleanup_tmp_root();
}

static void test_debug_overlay_toggle_parse(void) {
  bool enabled = false;
  assert_true(br_debug_parse_overlay_toggle_json("{\"enabled\":true}", &enabled), "debug overlay true parse");
  assert_true(enabled, "debug overlay true value");
  assert_true(br_debug_parse_overlay_toggle_json("{\"enabled\":false}", &enabled), "debug overlay false parse");
  assert_true(!enabled, "debug overlay false value");
}

static void test_apply_payload_write_accepts_allowed_paths(void) {
  char tmp_dir_template[] = "/tmp/runtime-payload-XXXXXX";
  char *tmp_dir = mkdtemp(tmp_dir_template);
  assert_true(tmp_dir != NULL, "mkdtemp succeeded");
  const char *err = NULL;
  assert_true(br_apply_payload_write(tmp_dir, ".stats-display", "COMPONENT_DASHBOARD_V1\ntitle=ok\n", &err),
              "payload_write to .stats-display succeeded");
  /* verify file content */
  char read_back[512];
  char full_path[BR_MAX_PATH];
  snprintf(full_path, sizeof(full_path), "%s/.stats-display", tmp_dir);
  assert_true(br_read_text_file(full_path, read_back, sizeof(read_back)),
              "read back stats-display");
  assert_true(strstr(read_back, "COMPONENT_DASHBOARD_V1") != NULL,
              "stats-display has magic line");
  assert_true(strstr(read_back, "title=ok") != NULL, "stats-display has slot");
  /* cleanup */
  unlink(full_path);
  rmdir(tmp_dir);
}

static void test_apply_payload_write_rejects_unknown_path(void) {
  const char *err = NULL;
  assert_true(!br_apply_payload_write("/tmp", "../../etc/passwd", "x", &err),
              "rejected path traversal");
  assert_true(err != NULL && strstr(err, "whitelist") != NULL,
              "error mentions whitelist");

  err = NULL;
  assert_true(!br_apply_payload_write("/tmp", ".some-other-file", "x", &err),
              "rejected non-whitelisted file");
  assert_true(err != NULL, "error set");
}

/* ---------- USB serial protocol tests (PTY loopback) ---------- */

typedef struct {
  char topic[128];
  char payload[1024];
  int call_count;
} usb_test_recv;

static void usb_on_message(const char *topic, const char *payload, void *userdata) {
  usb_test_recv *r = (usb_test_recv *) userdata;
  r->call_count++;
  snprintf(r->topic, sizeof(r->topic), "%s", topic);
  snprintf(r->payload, sizeof(r->payload), "%s", payload);
}

static void test_usb_serial_send_recv(void) {
  /* Create a PTY pair: master (host side) <-> slave (device side) */
  int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
  assert_true(master_fd >= 0, "posix_openpt");
  assert_true(grantpt(master_fd) == 0, "grantpt");
  assert_true(unlockpt(master_fd) == 0, "unlockpt");

  char *slave_name = ptsname(master_fd);
  assert_true(slave_name != NULL, "ptsname");

  usb_test_recv recv = {0};
  br_usb_serial serial = {0};
  int rc = br_usb_serial_open(&serial, slave_name, 115200, usb_on_message, &recv);
  assert_true(rc == 0, "usb_serial_open on PTY slave");
  assert_true(serial.connected, "serial.connected after open");

  /* Simulate host sending a JSON line to the device */
  const char *host_msg = "{\"topic\":\"hello\",\"payload\":{\"online\":true}}\n";
  ssize_t written = write(master_fd, host_msg, strlen(host_msg));
  assert_true(written == (ssize_t) strlen(host_msg), "write to PTY master");

  /* Poll: device side should receive and parse the message */
  usleep(10000);
  rc = br_usb_serial_poll(&serial);
  assert_true(rc == 0, "usb_serial_poll");
  assert_true(recv.call_count == 1, "received exactly 1 message");
  assert_true(strcmp(recv.topic, "hello") == 0, "received topic == hello");
  assert_true(strstr(recv.payload, "\"online\":true") != NULL, "received payload contains online:true");

  /* Test send: device -> host */
  serial.peer_acked = true;
  rc = br_usb_serial_send(&serial, "state/active", "{\"state\":\"idle\"}");
  assert_true(rc == 0, "usb_serial_send");

  /* Read back from master */
  char buf[512] = {0};
  usleep(10000);
  ssize_t nread = read(master_fd, buf, sizeof(buf) - 1);
  assert_true(nread > 0, "host received data after send");
  assert_true(strstr(buf, "state/active") != NULL, "host message contains topic");
  assert_true(strstr(buf, "\"state\":\"idle\"") != NULL, "host message contains payload");

  br_usb_serial_close(&serial);
  close(master_fd);
}

static void test_usb_serial_hello_handshake(void) {
  int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
  assert_true(master_fd >= 0, "posix_openpt hello");
  assert_true(grantpt(master_fd) == 0, "grantpt hello");
  assert_true(unlockpt(master_fd) == 0, "unlockpt hello");

  char *slave_name = ptsname(master_fd);
  usb_test_recv recv = {0};
  br_usb_serial serial = {0};
  assert_true(br_usb_serial_open(&serial, slave_name, 115200, usb_on_message, &recv) == 0, "open for hello");

  /* Send hello */
  int rc = br_usb_serial_send_hello(&serial, "test-device-01");
  assert_true(rc == 0, "send_hello ok");

  char buf[1024] = {0};
  usleep(10000);
  ssize_t nread = read(master_fd, buf, sizeof(buf) - 1);
  assert_true(nread > 0, "host got hello data");
  assert_true(strstr(buf, "\"topic\":\"hello\"") != NULL, "hello topic present");
  assert_true(strstr(buf, "\"transport\":\"usb\"") != NULL, "hello transport=usb");
  assert_true(strstr(buf, "test-device-01") != NULL, "hello contains boardDeviceId");

  br_usb_serial_close(&serial);
  close(master_fd);
}

static void test_usb_serial_multi_line_buffer(void) {
  int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
  assert_true(master_fd >= 0, "posix_openpt multi");
  assert_true(grantpt(master_fd) == 0, "grantpt multi");
  assert_true(unlockpt(master_fd) == 0, "unlockpt multi");

  char *slave_name = ptsname(master_fd);
  usb_test_recv recv = {0};
  br_usb_serial serial = {0};
  assert_true(br_usb_serial_open(&serial, slave_name, 115200, usb_on_message, &recv) == 0, "open multi");

  /* Send two JSON lines at once */
  const char *two_msgs =
    "{\"topic\":\"cmd/1\",\"payload\":{\"v\":1}}\n"
    "{\"topic\":\"cmd/2\",\"payload\":{\"v\":2}}\n";
  write(master_fd, two_msgs, strlen(two_msgs));
  usleep(20000);
  br_usb_serial_poll(&serial);
  assert_true(recv.call_count == 2, "received 2 messages from buffered input");

  br_usb_serial_close(&serial);
  close(master_fd);
}

int main(void) {
  test_normalize_topic_part();
  test_parse_input_action();
  test_remote_binding();
  test_audio_bridge_command();
  test_button_config_command();
  test_parse_speech_text_card_payload();
  test_bridge_state();
  test_bridge_state_interrupt_flag();
  test_bridge_state_session_identity();
  test_bridge_state_canonical_from_event();
  test_bridge_state_canonical_from_legacy_state();
  test_bridge_state_token_usage_top_level_camel_case();
  test_bridge_state_token_usage_snake_case_in_payload_wrapper();
  test_bridge_state_token_usage_missing();
  test_session_state_done_expires_to_idle();
  test_session_state_done_reveals_existing_working_without_interrupt();
  test_session_state_done_reveals_same_source_working();
  test_session_state_latest_update_wins_over_priority();
  test_session_state_keeps_working_during_short_idle_gap();
  test_session_state_idle_probe_cannot_preempt_working();
  test_session_state_ignores_expired_retained_done();
  test_touch_gesture();
  test_touch_swipe_toggles_screen_page();
  test_screen_page_resolution_accepts_home_alias();
  test_button_press_duration_classification();
  test_primary_button_behavior_ignores_active_widget();
  test_pairing_machine_transitions();
  test_pairing_machine_valid_config_boot();
  test_runtime_stats_delta_accumulation();
  test_runtime_stats_uses_last_usage_for_existing_codex_session_first_sample();
  test_runtime_stats_dashboard_lunch_branches();
  test_runtime_stats_dashboard_payload_for_framebuffer_renderer();
  test_runtime_stats_ignores_missing_token_usage();
  test_runtime_stats_persistence();
  test_runtime_stats_end_to_end_with_real_payload();
  test_runtime_stats_rollover_archives_yesterday();
  test_debug_overlay_toggle_parse();
  test_apply_payload_write_accepts_allowed_paths();
  test_apply_payload_write_rejects_unknown_path();
  test_usb_serial_send_recv();
  test_usb_serial_hello_handshake();
  test_usb_serial_multi_line_buffer();
  printf("board-runtime tests passed\n");
  return 0;
}
