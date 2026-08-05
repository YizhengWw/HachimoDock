"""
[Input] ESP32-P4 source, protocol docs, desktop counterparts, and built-in widget manifests.
[Output] Static cross-module regression coverage for the P4 firmware contract,
including authoritative NVS input-config readback and desktop/device widget
visual-preset parity, including the built-in two-key catch game.
Also locks GPIO20/GPIO21 four-direction joystick decoding and legacy aliases.
Also verifies the installable native Flappy Bird package and its one-button contract.
Also locks logical RGB565 to the matching RGB element order used by the panel.
Also locks completed conversation visibility to 60 seconds before returning idle.
Also locks deterministic H.264 appearance pack IDs and protected slot roles.
[Pos] Fast host-side contract suite run before firmware build and hardware smoke tests.
"""

from pathlib import Path


import json
import re


ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def read_required(rel):
    path = ROOT / rel
    assert path.exists(), f"missing required file: {rel}"
    return path.read_text(encoding="utf-8")


def read_workspace(rel):
    path = WORKSPACE / rel
    assert path.exists(), f"missing required workspace file: {rel}"
    return path.read_text(encoding="utf-8")


def read_usb_serial_contract():
    sources = [
        "ref/src-tauri/src/usb_serial.rs",
        "ref/src-tauri/src/usb_serial/appearance_transaction.rs",
        "ref/src-tauri/src/usb_serial/connection_handle.rs",
        "ref/src-tauri/src/usb_serial/firmware_transaction.rs",
        "ref/src-tauri/src/usb_serial/native_usb_protocol.rs",
        "ref/src-tauri/src/usb_serial/transaction_waiters.rs",
        "ref/src-tauri/src/usb_serial/widget_transaction.rs",
    ]
    return "\n".join(read_workspace(path) for path in sources)


def test_protocol_doc_declares_usb_only_p4_runtime():
    doc = read("protocol.md")
    assert '"topic": "hello"' in doc
    assert '"runtime": "esp-p4"' in doc
    assert '"wireProtocol": "pet-usb-jsonl-v3"' in doc
    assert '"usbOnly": true' in doc
    assert '"nativeUsb": true' in doc
    assert '"nativeProtocol": "pet-usb-native-v1"' in doc
    assert '"p4-h264-v1"' in doc
    assert '"p4-mjpeg-v1"' in doc
    assert '"hardware"' in doc
    assert '"soc": "ESP32-P4 RISC-V Dual-Core"' in doc
    assert '"psram": "32MB In-package stacked"' in doc
    assert '"flash": "32MB QSPI NOR Flash"' in doc
    assert '"version": 1' in doc
    assert '"widgets": true' in doc
    assert '"widgetRuntime": "p4-bounded-runtime-v3"' in doc
    assert '"widgetScene": "p4-grid-scene-v1"' in doc
    assert '"widgetGames": ["blocks", "snake", "flappy"]' in doc
    assert '"widgetGamePresets": ["blocks", "snake", "flappy"]' in doc
    assert '"voice": true' in doc
    assert '"audio": true' in doc
    assert '"codec": "ES8311"' in doc
    assert "ES7210" in doc
    assert '"transport": "usb-jsonl-pcm-v1"' in doc
    assert '"firmwareOta": true' in doc
    assert '"transport": "usb-jsonl-ota-v1"' in doc
    assert '"stats": true' in doc
    assert '"screenPages": ["main", "components", "app"]' in doc
    assert '"componentCatalogMax": 16' in doc


def test_p4_native_usb_vendor_bulk_contract_is_declared():
    doc = read("protocol.md")
    cmake = read("main/CMakeLists.txt")
    manifest = read("main/idf_component.yml")
    source = read_required("main/pet_p4_usb_native.c")
    descriptors = read_required("main/pet_p4_usb_descriptors.c")
    config = read_required("main/tusb_config.h")

    assert "pet-usb-native-v1" in doc
    assert "vendor bulk" in doc
    assert "0x303A" in doc
    assert "0x4040" in doc
    assert '"maxAppearanceSlots": 2' in doc
    assert '"appearanceSlotReuse": true' in doc
    assert '"bulk": true' in doc
    assert "pet_p4_usb_native.c" in cmake
    assert "pet_p4_usb_descriptors.c" in cmake
    assert "tinyusb" in cmake
    assert "espressif/tinyusb" in manifest
    assert "pet_p4_native_usb_init" in source
    assert "pet_p4_native_usb_send_json_line" in source
    assert "tusb_init" in source
    assert "tud_vendor_rx_cb" in source
    assert "P4BU" in source
    assert "TUD_VENDOR_DESCRIPTOR" in descriptors
    assert "0x303A" in descriptors
    assert "0x4040" in descriptors
    assert "OpenClaw P4 Native USB" in descriptors
    assert ".bcdUSB = 0x0210" in descriptors
    assert "TUD_BOS_MS_OS_20_DESCRIPTOR" in descriptors
    assert "MS_OS_20_FEATURE_COMPATBLE_ID" in descriptors
    assert "'W', 'I', 'N', 'U', 'S', 'B'" in descriptors
    assert "tud_vendor_control_xfer_cb" in descriptors
    assert "request->wIndex != 7" in descriptors
    assert "WINUSB" in doc
    assert "CFG_TUD_VENDOR" in config
    assert "OPT_MODE_HIGH_SPEED" in config


def test_pc_p4_sync_prefers_native_usb_bulk_transport():
    cargo = read_workspace("ref/src-tauri/Cargo.toml")
    usb = read_usb_serial_contract()
    native_protocol = read_workspace(
        "ref/src-tauri/src/usb_serial/native_usb_protocol.rs"
    )
    lib = read_workspace("ref/src-tauri/src/lib.rs")
    native = read("main/pet_p4_usb_native.c")
    protocol = read("protocol.md")

    assert re.search(r'rusb\s*=\s*\{[^}]*version\s*=\s*"0\.9"[^}]*features\s*=\s*\["vendored"\]', cargo)
    assert "P4_NATIVE_USB_VID: u16 = 0x303a" in usb
    assert "P4_NATIVE_USB_PID: u16 = 0x4040" in usb
    assert "mod native_usb_protocol;" in usb
    assert "P4_NATIVE_USB_FRAME_MAGIC" in native_protocol
    assert "encode_native_usb_frame" in native_protocol
    assert "NativeUsbP4Transport" in usb
    assert "sync_appearance_p4_native" in usb
    assert "native USB bulk" in usb
    assert "sync_appearance_p4_native" in lib
    assert "Native USB" in lib
    assert "P4_NATIVE_KIND_PING" in native_protocol
    assert "parse_native_usb_pong" in native_protocol
    assert "select_native_usb_candidate" in native_protocol
    assert "probe_identity" in usb
    assert "expected_board_device_id" in usb[
        usb.index("pub fn sync_appearance_p4_native"):
        usb.index("fn ensure_p4_native_full_pack_supported")
    ]
    assert "handle_identity_ping" in native
    assert 'cJSON_AddStringToObject(response, "boardDeviceId", g_state->board_device_id)' in native
    assert 'cJSON_AddStringToObject(response, "nonce", nonce)' in native
    assert 'cJSON_AddNumberToObject(response, "protocolSchema", PET_P4_PROTOCOL_SCHEMA)' in native
    assert 'cJSON_AddStringToObject(response, "buildId", PET_P4_BUILD_ID)' in native
    assert "duplicate devices" in protocol
    assert "fail closed" in protocol
    assert "WIDGET_CHUNK_ACK_TIMEOUT" in usb
    assert "WIDGET_CHUNK_MAX_ATTEMPTS" in usb
    assert '"decodedSize": decoded_size' in usb
    assert '"checksum": checksum' in usb


def test_firmware_contract_accepts_p4_assets_and_rejects_linux_mp4_ota():
    source = read("main/pet_p4_protocol.c")
    cmake = read("main/CMakeLists.txt")
    assert 'cJSON_AddStringToObject(payload, "runtime", "esp-p4")' in source
    assert 'cJSON_AddStringToObject(payload, "deviceModel", "ESP32-P4 RISC-V Dual-Core + ESP32-C6")' in source
    assert 'cJSON_AddStringToObject(hardware, "hpCpu", "HP@360MHz(Max 400MHz)")' in source
    assert 'cJSON_AddStringToObject(hardware, "onChipSram", "768KB L2MEM + 32KB LP SRAM + 8KB TCM")' in source
    assert 'cJSON_AddStringToObject(hardware, "psram", "32MB In-package stacked")' in source
    assert 'cJSON_AddStringToObject(hardware, "flash", "32MB QSPI NOR Flash")' in source
    assert 'cJSON_AddBoolToObject(capabilities, "usbOnly", true)' in source
    assert 'cJSON_AddBoolToObject(capabilities, "mp4", false)' in source
    assert 'cJSON_AddNumberToObject(capabilities, "version", 1)' in source
    assert 'cJSON_AddBoolToObject(capabilities, "widgets", true)' in source
    assert 'cJSON_AddStringToObject(capabilities, "widgetRuntime", "p4-bounded-runtime-v3")' in source
    assert 'cJSON_AddStringToObject(capabilities, "widgetScene", "p4-grid-scene-v1")' in source
    assert 'cJSON_AddItemToArray(widget_games, cJSON_CreateString("blocks"))' in source
    assert 'cJSON_AddItemToArray(widget_games, cJSON_CreateString("snake"))' in source
    assert 'cJSON_AddItemToArray(widget_games, cJSON_CreateString("flappy"))' in source
    assert 'cJSON_AddItemToObject(capabilities, "widgetGamePresets", widget_game_presets)' in source
    assert 'bool audio_ready = pet_p4_audio_ready()' in source
    assert 'cJSON_AddBoolToObject(capabilities, "voice", audio_ready)' in source
    assert 'cJSON_AddBoolToObject(capabilities, "audio", audio_ready || audio_playback_ready)' in source
    assert 'cJSON_AddBoolToObject(capabilities, "stats", true)' in source
    assert 'cJSON_CreateString("p4-h264-v1")' in source
    assert 'cJSON_CreateString("p4-mjpeg-v1")' in source
    assert 'send_asset_ack(' in source
    assert "path_is_p4_asset" in source
    assert "handle_asset_chunk" in source
    assert "mbedtls" in cmake
    assert "lvgl" in cmake
    assert "linux mp4/wav assets are not supported on esp-p4" in source
    assert "g_line_buffer[32768]" in read("main/pet_p4_main.c")


def test_p4_bounded_miniapp_contract_is_explicit_and_heap_safe():
    source = read_required("main/pet_p4_miniapp.c")
    header = read_required("main/pet_p4_miniapp.h")
    game = read_required("main/pet_p4_game.c")
    game_header = read_required("main/pet_p4_game.h")
    protocol = read("main/pet_p4_protocol.c")
    renderer = read("main/pet_p4_renderer.c")
    cmake = read("main/CMakeLists.txt")
    desktop_preview = read_workspace("ref/src/component-center/DeviceScreenPreview.jsx")
    desktop_styles = read_workspace("ref/src/styles.css")

    assert "pet_p4_miniapp.c" in cmake
    assert "pet_p4_game.c" in cmake
    assert "PET_P4_MINIAPP_ACTION_MAX" in header
    assert "pet_p4_miniapp_get_view" in header
    assert "#define MINIAPP_MAX_VARS 8" in source
    assert "#define MINIAPP_MAX_STATES 6" in source
    assert "#define MINIAPP_MAX_PAGES 4" in source
    assert "#define MINIAPP_MAX_TRANSITIONS 12" in source
    assert "#define MINIAPP_MAX_TICKS 8" in source
    assert "#define MINIAPP_MAX_BUTTONS 8" in source
    assert "#define MINIAPP_MAX_EFFECTS 4" in source
    assert "at most four" in read("protocol.md")
    assert "P4 mini-apps do not allow fetchers or readers" in source
    assert "mini-app chunk checksum mismatch" in source
    assert "mini-app chunk decoded size mismatch" in source
    assert "widget_next_index" in source
    assert "buttons_next_index" in source
    assert "chunk index is out of sequence" in source
    assert "mini-app chunk does not match the active transfer" in source
    assert "g_staging.failed" not in source
    assert "g_staging.widget_len = decoded_offset" in source
    assert "g_staging.buttons_len = decoded_offset" in source
    assert 'strcmp(event_name, "button.sw1.short_press") == 0' not in source
    assert 'strcmp(event_name, "button.sw1.long_press") == 0' not in source
    input_source = read("main/pet_p4_input.c")
    assert 'strcmp(state->screen_page, "app") != 0' in input_source
    assert '"miniapp_screen_tap"' in input_source
    assert '"miniapp_screen_long_press"' in input_source
    assert 'miniapp_event = "screen.region.tap"' in input_source
    assert 'miniapp_event = "screen.region.long_press"' in input_source
    assert 'pet_p4_miniapp_dispatch_input(\n      event_name' not in input_source
    assert "miniapp_runtime_t *parsed" in source
    assert "miniapp_runtime_t *next" in source
    assert "calloc(1, sizeof(*parsed))" in source
    assert "calloc(1, sizeof(*next))" in source
    assert "miniapp_runtime_t parsed;" not in source
    assert "miniapp_runtime_t next;" not in source
    assert "if (now_ms < tick->last_ms)" in source
    assert "for (int tick = 0; tick < g_runtime.tick_count; tick += 1) g_runtime.ticks[tick].last_ms = now_ms;" not in source
    assert "portENTER_CRITICAL(&g_runtime_lock)" in source
    assert "pet_p4_miniapp_get_view(&snapshot)" in protocol
    assert 'send_topic_payload(send_line, ctx, "widget-install-ack", payload)' in protocol
    assert 'send_widget_ack(send_line, ctx, transfer_id, phase, payload, ok, error)' in protocol
    assert 'cJSON_AddStringToObject(payload, "path", path)' in protocol
    assert 'cJSON_AddItemToObject(payload, "index", cJSON_Duplicate(index, true))' in protocol
    assert 'send_topic_payload(send_line, ctx, "miniapp/state", payload)' in protocol
    assert 'strcmp(topic, "miniapp/query") == 0' in protocol
    assert 'cJSON_CreateString("app")' in protocol
    assert 'cJSON_AddBoolToObject(widget_limits, "fetchers", false)' in protocol
    assert 'cJSON_AddBoolToObject(widget_limits, "readers", false)' in protocol
    assert "PET_P4_GAME_GRID_MAX_WIDTH 16" in game_header
    assert "PET_P4_GAME_GRID_MAX_HEIGHT 16" in game_header
    assert "PET_P4_GAME_SHAPE_PLAYER_SHIP" in game_header
    assert "PET_P4_GAME_SHAPE_ENEMY_SHIP" in game_header
    assert "PET_P4_GAME_SHAPE_BULLET" in game_header
    assert "PET_P4_GAME_SHAPE_STAR" in game_header
    assert "pet_p4_game_command" in game
    assert "pet_p4_game_process" in game
    assert "BLOCK_MASKS[7][4]" in game
    assert "snake_step" in game
    assert "parse_game" in source
    assert "game actions must match transitions and buttons" in source
    assert "button binding action must be unique" in source
    assert "button binding event is already assigned" in source
    assert 'cJSON_AddStringToObject(payload, "gameType"' in protocol
    assert "render_miniapp_page" in renderer
    assert "render_pixel_miniapp_page" in renderer
    assert "draw_miniapp_pixel_sprite" in renderer
    for sprite in ["mole-ready", "mole-left", "mole-center", "mole-right"]:
        assert f'strcmp(name, "{sprite}") == 0' in renderer
        assert f'strcmp(literal, "{sprite}") == 0' in source
    for sprite in ["coffee", "timer", "droplet", "gauge"]:
        assert f'strcmp(name, "{sprite}") == 0' in renderer
        assert f'strcmp(literal, "{sprite}") == 0' in source
    assert "columns = 15" in renderer
    assert 'strncmp(app->visual_sprite, "mole-", 5) == 0' in renderer
    assert "draw_miniapp_game_grid" in renderer
    assert "draw_miniapp_game_entity" in renderer
    assert 'strcmp(app->visual_style, "clean") == 0' in renderer
    for palette in ["ocean", "forest", "ember", "mono"]:
        assert f'strcmp(name, "{palette}") == 0' in renderer
        assert f'strcmp(literal, "{palette}") == 0' in source
    assert "PET_P4_GAME_SNAKE || game->kind == PET_P4_GAME_FLAPPY" in renderer
    assert "bool flappy_game = game->kind == PET_P4_GAME_FLAPPY" in renderer
    for rgb in [
        "rgb565(22, 53, 79)",
        "rgb565(143, 216, 247)",
        "rgb565(55, 168, 73)",
        "rgb565(155, 228, 91)",
        "rgb565(243, 165, 31)",
        "rgb565(255, 241, 166)",
    ]:
        assert rgb in renderer
    for color in ["#16354f", "#8fd8f7", "#37a849", "#9be45b", "#f3a51f", "#fff1a6"]:
        assert color in desktop_styles
    assert "bool native_game = app->game.kind != PET_P4_GAME_NONE" in renderer
    assert "int play_w = native_game ? 462 : (scoreboard_first ? 364 : 430)" in renderer
    assert "int score_w = native_game ? 108 : (scoreboard_first ? 202 : 142)" in renderer
    assert "stage_h - 4" in renderer
    assert "int score_h = native_game ? 132" in renderer
    assert "blocks_game ? 350 : 318" in renderer
    assert "int footer_h = maximized_blocks ? 26" in renderer
    assert 'strcmp(app->visual_style, "pixel") == 0' in renderer
    assert 'strcmp(app->visual_layout, "tool") == 0' in renderer
    assert "bool pixel_tool" in renderer
    assert "render_pixel_tool_miniapp_page(app)" in renderer
    assert "static miniapp_tool_accent_t miniapp_tool_accent" in renderer
    assert "miniapp_tool_badge_visible" in renderer
    assert "draw_miniapp_tool_keyboard_icon" in renderer
    assert "PIXEL_TOOL_ACCENTS" in desktop_preview
    for palette, desktop_accent, board_rgb in [
        ("sunset", "coral", (255, 102, 92)),
        ("mint", "aqua", (87, 217, 255)),
        ("arcade", "violet", (170, 140, 255)),
    ]:
        assert f'{palette}: "{desktop_accent}"' in desktop_preview
        red, green, blue = board_rgb
        assert re.search(
            rf'strcmp\(palette_name, "{palette}"\).*?'
            rf'accent\.red = {red};.*?'
            rf'accent\.green = {green};.*?'
            rf'accent\.blue = {blue};',
            renderer,
            re.DOTALL,
        )
    assert 'strcmp(app->visual_style, "pixel") != 0' in renderer
    assert "miniapp_pixel_palette(app->visual_palette)" in renderer
    assert 'bool scoreboard = strcmp(app->visual_layout, "scoreboard") == 0' in renderer
    assert 'bool arcade_page = strcmp(app->visual_palette, "arcade") == 0' in renderer
    assert "soft_ivory = arcade_page ? palette.light : palette.ink" in renderer
    assert "page_muted = arcade_page ? palette.secondary : palette.shadow" in renderer
    assert '"visualStyle", "visualPalette", "visualLayout", "visualSprite"' in source
    assert "visual_literal_allowed" in source
    assert 'cJSON_AddStringToObject(payload, "visualStyle", view->visual_style)' in protocol
    assert 'cJSON_AddStringToObject(payload, "visualSprite", view->visual_sprite)' in protocol
    assert "pet_p4_miniapp_get_view(&snapshot)" in renderer
    main = read("main/pet_p4_main.c")
    assert "g_render_mutex" in main
    assert "xSemaphoreTake(g_render_mutex" in main


def test_p4_token_widget_uses_bounded_stats_instead_of_host_readers():
    miniapp = read_required("main/pet_p4_miniapp.c")
    miniapp_header = read_required("main/pet_p4_miniapp.h")
    protocol = read("main/pet_p4_protocol.c")
    desktop = read_usb_serial_contract()

    assert 'widget_id == "token-usage"' in desktop
    assert 'path == "runtime/widget.json"' in desktop
    assert 'object.remove("readers")' in desktop
    assert 'object.remove("fetchers")' in desktop
    assert '"P4 mini-apps do not allow fetchers or readers"' in miniapp
    assert "pet_p4_miniapp_sync_stats" in miniapp_header
    assert 'strcmp(g_runtime.widget_id, "token-usage") == 0' in miniapp
    for variable in [
        "agent_label",
        "headline_text",
        "total_display",
        "input_display",
        "output_display",
        "cache_display",
        "breakdown_text",
    ]:
        assert f'set_string_var(&g_runtime, "{variable}"' in miniapp
    assert protocol.count("pet_p4_miniapp_sync_stats(&state->stats)") >= 3


def test_builtin_tool_widgets_declare_bounded_completion_cycles():
    miniapp = read_required("main/pet_p4_miniapp.c")
    tomato = json.loads(read_workspace("ref/builtin-clawpkgs/tomato-clock/runtime/widget.json"))
    token_usage = json.loads(read_workspace("ref/builtin-clawpkgs/token-usage/runtime/widget.json"))

    assert '"when", "then"' in miniapp
    assert "tick->condition_var" in miniapp
    assert "tick->completion_state" in miniapp
    assert "tick->completion_effects" in miniapp
    assert "g_runtime.vars[tick->condition_var].int_value <= tick->condition_lte" in miniapp

    focus_tick, rest_tick = tomato["tick"]
    assert focus_tick["when"] == {"var": "remaining_s", "lte": 0}
    assert focus_tick["then"] == {
        "to": "rest",
        "set": {"remaining_s": 300, "phase_total_s": 300},
        "inc": {"completed_sessions": 1},
    }
    assert rest_tick["then"] == {
        "to": "focus",
        "set": {"remaining_s": 1500, "phase_total_s": 1500},
    }
    assert {"focus_paused", "rest_paused"}.issubset(tomato["states"])
    assert tomato["dashboard"]["progress"] == {
        "pct_of": "remaining_s",
        "of_max": "phase_total_s",
        "label": "本轮剩余",
    }

    assert token_usage["dashboard"]["title"] == "Token 仪表盘"
    assert token_usage["initial_page"] == "total"
    assert [page["id"] for page in token_usage["pages"]] == ["total", "input", "output"]
    assert all(set(page) == {"id"} for page in token_usage["pages"])


def test_builtin_tool_widgets_use_three_keys_and_fit_p4_limits():
    expected_actions = {
        "tomato-clock": [
            "tomato.start_pause",
            "tomato.skip_phase",
            "tomato.reset_phase",
        ],
        "drink-reminder": [
            "reminder.acknowledge",
            "reminder.pause_resume",
            "reminder.switch_view",
        ],
        "token-usage": [
            "stats.show_total",
            "stats.show_input",
            "stats.show_output",
        ],
    }
    kinds = {
        "two-key-pong": "game",
        "flappy-bird": "game",
        "block-combo": "game",
        "snake-turn": "game",
        "tomato-clock": "tool",
        "drink-reminder": "tool",
        "token-usage": "tool",
    }
    for widget_id, expected_kind in kinds.items():
        manifest = json.loads(
            read_workspace(f"ref/builtin-clawpkgs/{widget_id}/component.json")
        )
        assert manifest["kind"] == expected_kind

    for widget_id, actions in expected_actions.items():
        runtime = json.loads(
            read_workspace(f"ref/builtin-clawpkgs/{widget_id}/runtime/widget.json")
        )
        buttons = json.loads(
            read_workspace(f"ref/builtin-clawpkgs/{widget_id}/buttons.json")
        )
        assert len(runtime["vars"]) <= 8
        assert len(runtime["states"]) <= 6
        assert len(runtime["transitions"]) <= 12
        assert len(runtime.get("tick", [])) <= 8
        assert len(json.dumps(runtime, ensure_ascii=False, separators=(",", ":")).encode()) < 4096
        assert len(json.dumps(buttons, ensure_ascii=False, separators=(",", ":")).encode()) < 2048
        switch_bindings = [
            binding for binding in buttons
            if binding["control"] in {"SW1", "SW2", "SW3"}
        ]
        assert [binding["control"] for binding in switch_bindings] == ["SW1", "SW2", "SW3"]
        assert [binding["event"] for binding in switch_bindings] == [
            "button.sw1.short_press",
            "button.sw2.short_press",
            "button.sw3.short_press",
        ]
        assert [binding["action"] for binding in switch_bindings] == actions
        assert any(
            binding["event"] == "button.encoder.long_press"
            and binding["action"] == "page_main"
            for binding in buttons
        )
        assert runtime["dashboard"]["visualStyle"] == "pixel"
        assert runtime["dashboard"]["visualLayout"] in {"arcade", "scoreboard", "tool"}
        assert "visualPalette" in runtime["dashboard"]
        assert "visualSprite" in runtime["dashboard"]

    drink = json.loads(
        read_workspace("ref/builtin-clawpkgs/drink-reminder/runtime/widget.json")
    )
    assert drink["states"] == ["counting", "paused", "due"]
    assert [page["id"] for page in drink["pages"]] == ["next", "today"]
    assert all(set(page) == {"id"} for page in drink["pages"])
    assert drink["tick"][0]["then"] == {"to": "due", "set": {"remaining_min": 0}}

    token = json.loads(
        read_workspace("ref/builtin-clawpkgs/token-usage/runtime/widget.json")
    )
    assert set(token["vars"]) == {
        "agent_label",
        "headline_text",
        "total_display",
        "input_display",
        "output_display",
        "cache_display",
        "breakdown_text",
    }
    assert {
        reader["field_pattern"] for reader in token["readers"].values()
    } >= {
        "(?m)^tokenTotal=(.+)$",
        "(?m)^tokenInput=(.+)$",
        "(?m)^tokenOutput=(.+)$",
        "(?m)^tokenCache=(.+)$",
    }
    assert token["dashboard"]["visualStyle"] == "pixel"
    assert token["dashboard"]["visualPalette"]["switch_page"] == {
        "total": "arcade",
        "input": "mint",
        "output": "sunset",
    }
    assert token["dashboard"]["visualLayout"] == "tool"
    assert token["dashboard"]["visualSprite"]["switch_page"] == {
        "total": "gauge",
        "input": "bolt",
        "output": "gauge",
    }


def test_p4_widget_delete_clears_persistence_and_returns_to_main():
    miniapp = read_required("main/pet_p4_miniapp.c")
    miniapp_header = read_required("main/pet_p4_miniapp.h")
    protocol = read("main/pet_p4_protocol.c")

    assert "pet_p4_miniapp_remove" in miniapp_header
    assert "bool pet_p4_miniapp_remove(" in miniapp
    for path in [
        "MINIAPP_ID_PATH",
        "MINIAPP_ID_TMP_PATH",
        "MINIAPP_WIDGET_PATH",
        "MINIAPP_WIDGET_TMP_PATH",
        "MINIAPP_BUTTONS_PATH",
        "MINIAPP_BUTTONS_TMP_PATH",
    ]:
        assert f"remove_file_if_present({path})" in miniapp
    assert "memset(&g_runtime, 0, sizeof(g_runtime))" in miniapp
    assert "memset(&g_staging, 0, sizeof(g_staging))" in miniapp
    assert "if (value[0] < 'a' || value[0] > 'z') return false;" in miniapp
    assert 'strcmp(topic, "widget/delete") == 0' in protocol
    assert 'phase = "delete"' in protocol
    assert 'cJSON_AddBoolToObject(capabilities, "widgetDelete", true)' in protocol
    assert 'cJSON_AddBoolToObject(features, "widgetDelete", true)' in protocol
    assert 'copy_text(error, sizeof(error), "missing/invalid transferId")' in protocol
    assert "pet_p4_miniapp_remove(json_string(payload, \"widgetId\")" in protocol
    assert 'copy_text(state->screen_page, sizeof(state->screen_page), "main")' in protocol
    assert "send_widget_ack(send_line, ctx, transfer_id, phase, payload, ok, error)" in protocol


def test_p4_widget_inventory_is_request_matched_and_capacity_bounded():
    miniapp = read_required("main/pet_p4_miniapp.c")
    miniapp_header = read_required("main/pet_p4_miniapp.h")
    protocol = read("main/pet_p4_protocol.c")
    protocol_doc = read("protocol.md")

    assert "pet_p4_miniapp_installed_id" in miniapp_header
    assert "bool pet_p4_miniapp_installed_id(" in miniapp
    assert "read_file(MINIAPP_ID_PATH" in miniapp
    assert "pet_p4_miniapp_active_id" in miniapp_header
    assert "bool pet_p4_miniapp_active_id(" in miniapp
    assert "pet_p4_miniapp_active_id(active_widget_id" in protocol
    assert "pet_p4_miniapp_view_t view =" not in protocol
    assert 'cJSON_AddBoolToObject(capabilities, "widgetInventory", true)' in protocol
    assert 'cJSON_AddBoolToObject(features, "widgetInventory", true)' in protocol
    assert 'strcmp(topic, "widget/list") == 0' in protocol
    assert "send_widget_inventory(state, payload, send_line, ctx)" in protocol
    assert '"widget/inventory"' in protocol
    assert '"requestId"' in protocol
    assert '"supportsMultiple", true' in protocol
    assert '"maxInstalled", PET_P4_MINIAPP_CATALOG_MAX' in protocol
    assert 'cJSON_AddNullToObject(item, "name")' in protocol
    assert 'cJSON_AddNullToObject(item, "kind")' in protocol
    assert 'cJSON_AddNullToObject(item, "version")' in protocol
    assert "capabilities.widgetInventory=true" in protocol_doc


def test_p4_component_packages_commit_through_validated_ab_catalog_snapshots():
    miniapp = read_required("main/pet_p4_miniapp.c")
    factory = read_required("tools/build_factory_image.py")
    protocol_doc = read("protocol.md")
    commit_start = miniapp.index("bool pet_p4_miniapp_install_commit")
    commit = miniapp[
        commit_start:
        miniapp.index("static bool remove_file_if_present", commit_start)
    ]
    removal = miniapp[miniapp.index("bool pet_p4_miniapp_remove"):]

    assert "MINIAPP_CATALOG_GENERATION_COUNT 2" in miniapp
    assert "MINIAPP_PACKAGE_GENERATION_COUNT 2" in miniapp
    assert "MINIAPP_CATALOG_VERSION 2" in miniapp
    assert '"sequence"' in miniapp
    assert '"activeWidgetId"' in miniapp
    assert '"packageGeneration"' in miniapp
    assert '"widgetChecksum"' in miniapp
    assert '"buttonsChecksum"' in miniapp
    assert "load_catalog_snapshot(0, snapshot0)" in miniapp
    assert "load_catalog_snapshot(1, snapshot1)" in miniapp
    assert 'recovered ? " after rollback" : ""' in miniapp
    assert "package_generation = (uint8_t) (1 - next_catalog[catalog_index].package_generation)" in commit
    assert "next_catalog[catalog_index].widget_checksum = miniapp_checksum" in commit
    assert "next_catalog[catalog_index].buttons_checksum = miniapp_checksum" in commit
    assert commit.index("write_file_atomic(widget_tmp_path") < commit.index("load_catalog_runtime_item(")
    assert commit.index("load_catalog_runtime_item(") < commit.index("persist_catalog_snapshot(")
    assert "MINIAPP_ID_PATH" not in commit
    assert "Keep both committed generations" in removal
    assert "remove_file_if_present(widget_path)" not in removal

    assert 'MINIAPP_CATALOG_FILE = "p4-miniapps-0.json"' in factory
    assert '"version": 2' in factory
    assert '"sequence": 1' in factory
    assert '"activeWidgetId": active_id' in factory
    assert '"packageGeneration": 0' in factory
    assert "fnv1a32_bytes(widget_bytes)" in factory
    assert "componentCatalogGeneration=true" in protocol_doc


def test_builtin_pixel_games_use_bounded_native_game_presets():
    miniapp = read_required("main/pet_p4_miniapp.c")
    game = read_required("main/pet_p4_game.c")
    renderer = read_required("main/pet_p4_renderer.c")
    blocks = json.loads(read_workspace("ref/builtin-clawpkgs/block-combo/runtime/widget.json"))
    snake = json.loads(read_workspace("ref/builtin-clawpkgs/snake-turn/runtime/widget.json"))
    flappy = json.loads(read_workspace("ref/builtin-clawpkgs/flappy-bird/runtime/widget.json"))

    assert "pet_p4_game_kind_from_name(type)" in miniapp
    assert 'strcmp(name, "blocks") == 0' in game
    assert 'strcmp(name, "snake") == 0' in game
    assert 'strcmp(name, "flappy") == 0' in game
    assert "flappy_step" in game
    assert "FLAPPY_FLAP_VELOCITY_Q8" in game
    assert "pet_p4_game_process(&g_runtime.game.engine, now_ms)" in miniapp
    assert "draw_miniapp_game_grid" in renderer
    assert "bool native_game = app->game.kind != PET_P4_GAME_NONE" in renderer
    assert "int score_w = native_game ? 108 : (scoreboard_first ? 202 : 142)" in renderer
    assert blocks["game"] == {
        "type": "blocks",
        "tick_ms": 480,
        "playing_state": "playing",
        "result_state": "result",
        "score_var": "score",
        "actions": {
            "start": "blocks.start",
            "left": "blocks.left",
            "right": "blocks.right",
            "rotate": "blocks.rotate",
            "drop": "blocks.drop",
        },
    }
    assert snake["game"] == {
        "type": "snake",
        "tick_ms": 220,
        "playing_state": "playing",
        "result_state": "result",
        "score_var": "score",
        "actions": {
            "start": "snake.start",
            "left": "snake.turn_left",
            "right": "snake.turn_right",
        },
    }
    assert flappy["game"] == {
        "type": "flappy",
        "tick_ms": 100,
        "playing_state": "playing",
        "result_state": "result",
        "score_var": "score",
        "actions": {
            "flap": "flappy.flap",
        },
    }
    assert blocks["dashboard"]["visualSprite"]["switch_state"]["ready"] == "blocks"
    assert snake["dashboard"]["visualSprite"]["switch_state"]["playing"] == "snake"
    assert flappy["dashboard"]["visualSprite"]["switch_state"]["playing"] == "flappy"
    for widget_id in ("block-combo", "snake-turn"):
        buttons = json.loads(
            read_workspace(f"ref/builtin-clawpkgs/{widget_id}/buttons.json")
        )
        switch_controls = {
            binding["control"]
            for binding in buttons
            if binding["event"].endswith(".short_press")
        }
        assert {"SW1", "SW2", "SW3"} <= switch_controls
        actions = [binding["action"] for binding in buttons]
        assert len(actions) == len(set(actions))
    flappy_buttons = json.loads(
        read_workspace("ref/builtin-clawpkgs/flappy-bird/buttons.json")
    )
    assert [binding["action"] for binding in flappy_buttons] == [
        "flappy.flap",
        "page_main",
    ]


def test_generic_scene_runtime_is_shared_by_games_and_tools():
    miniapp = read_required("main/pet_p4_miniapp.c")
    game = read_required("main/pet_p4_game.c")
    header = read_required("main/pet_p4_game.h")
    protocol = read_required("main/pet_p4_protocol.c")
    two_key_pong = json.loads(
        read_workspace(
            "ref/builtin-clawpkgs/two-key-pong/runtime/widget.json"
        )
    )
    tomato = json.loads(
        read_workspace(
            "ref/builtin-clawpkgs/tomato-clock/runtime/widget.json"
        )
    )

    assert two_key_pong["engine"] == "p4-bounded-runtime-v3"
    assert two_key_pong["scene"]["grid"] == {"width": 16, "height": 16}
    assert two_key_pong.get("game") is None
    assert tomato["engine"] == "p4-bounded-runtime-v3"
    assert tomato.get("scene") is None
    assert '"engine", "scene"' in miniapp
    assert "parse_bounded_scene" in miniapp
    assert "parse_bounded_rules" in miniapp
    assert "PET_P4_GAME_MAX_ENTITIES 12" in header
    assert "PET_P4_GAME_MAX_RULES 20" in header
    assert '"bounds", "shape", "active", "collidable"' in miniapp
    assert "frame->entities[frame->entity_count++] = *entity" in game
    assert "pet_p4_game_configure_bounded" in game
    assert "bounded_process_trigger" in game
    assert "PET_P4_GAME_TRIGGER_COLLISION" in game
    assert "PET_P4_GAME_TRIGGER_EDGE" in game
    assert '"p4-bounded-runtime-v3"' in protocol
    assert '"p4-grid-scene-v1"' in protocol


def test_petui_skill_contains_no_copyable_component_packages():
    skill_root = WORKSPACE / "skills" / "petui"
    patterns = (skill_root / "references" / "patterns.md").read_text(encoding="utf-8")
    package_markers = {
        "component.json",
        "negative-screen.json",
        "buttons.json",
        "widget.json",
        "share.json",
    }

    assert "故意不提供完整 JSON" in patterns
    assert not any(
        path.name in package_markers
        for path in (skill_root / "references").rglob("*")
        if path.is_file()
    )


def test_p4_identity_and_protocol_nack_are_explicit():
    main = read("main/pet_p4_main.c")
    source = read("main/pet_p4_protocol.c")
    doc = read("protocol.md")

    assert "esp_read_mac(mac, ESP_MAC_BASE)" in main
    assert '"p4-%02x%02x%02x%02x%02x%02x"' in main
    assert 'pet_p4_state_init(&g_state, board_device_id)' in main
    assert 'send_topic_payload(send_line, ctx, "protocol/ack", payload)' in source
    assert '"unsupported_topic"' in source
    assert '"unknown_topic"' in source
    assert '"invalid_message"' in source
    assert '"topic": "protocol/ack"' in doc
    assert 'strcmp(topic, "system/heartbeat") == 0' in source
    assert "PET_P4_HOST_HEARTBEAT_TIMEOUT_MS 6000ULL" in read("main/pet_p4_protocol.h")
    assert "if (now_ms < state->host_last_seen_ms) return true;" in source
    assert "system/heartbeat" in doc
    desktop = read_workspace("ref/src-tauri/src/lib.rs")
    assert '"system/heartbeat"' in desktop
    assert "fn start_usb_host_heartbeat(" in desktop


def test_p4_diagnostics_are_persisted_and_asset_safe():
    cmake = read("main/CMakeLists.txt")
    source = read_required("main/pet_p4_diagnostics.c")
    header = read_required("main/pet_p4_diagnostics.h")
    protocol = read("main/pet_p4_protocol.c")
    main = read("main/pet_p4_main.c")
    desktop = read_usb_serial_contract()

    assert '"pet_p4_diagnostics.c"' in cmake
    assert "pet_p4_diagnostics_init" in header
    assert 'PET_P4_DIAGNOSTICS_NVS_NAMESPACE "pet_diag"' in source
    assert "nvs_erase_key" in source
    assert "esp_reset_reason()" in source
    assert "fault_reset_count" in source
    assert 'strcmp(topic, "diagnostics/query") == 0' in source
    assert "pet_p4_input_get_joystick_snapshot" in source
    assert '"centerX"' in source
    assert '"maximumX"' in source
    assert '"centerY"' in source
    assert '"maximumY"' in source
    assert 'strcmp(topic, "system/reset-inputs") == 0' in source
    assert 'strcmp(topic, "system/reboot") == 0' in source
    assert '"preservedAppearanceAssets"' in source
    assert '"imageState"' in source
    assert "diagnostics persistence unavailable" in main
    assert "pet_p4_diagnostics_process(now_ms, &g_state)" in main
    assert 'cJSON_AddBoolToObject(capabilities, "diagnostics", true)' in protocol
    assert '"diagnostics/query"' in desktop
    assert '"system/reset-inputs"' in desktop
    assert '"system/reboot"' in desktop
    assert "expected_board_device_id" in desktop
    assert "response_board_device_id" in desktop


def test_platformio_board_metadata_matches_actual_p4_hardware():
    platformio = read("platformio.ini")
    board = json.loads(read("boards/esp32-p4-wlk2802-32mb.json"))

    assert "board = esp32-p4-wlk2802-32mb" in platformio
    assert board["build"]["mcu"] == "esp32p4"
    assert board["build"]["f_cpu"] == "360000000L"
    assert board["build"]["f_flash"] == "80000000L"
    assert board["upload"]["flash_size"] == "32MB"
    assert board["upload"]["maximum_ram_size"] == 827392
    assert board["upload"]["maximum_size"] == 33554432


def test_firmware_contract_accepts_speech_card_and_stats_snapshot():
    header = read("main/pet_p4_protocol.h")
    source = read("main/pet_p4_protocol.c")
    doc = read("protocol.md")

    assert "current_title" in header
    assert "current_status_text" in header
    assert "stats_json" in header
    assert '"displayTitle", "sessionName", "title", "agent"' in source
    assert '"displayContent", "text", "message"' in source
    assert 'strcmp(topic, "stats/update") == 0' in source
    assert 'pet_p4_stats_update(&state->stats, payload, agent)' in source
    assert 'pet_p4_stats_update(&state->stats, payload, state->active_agent)' in source
    assert '"topic": "speech/text"' in doc
    assert '"displayContent": "Building the P4 runtime..."' in doc


def test_p4_stats_model_feeds_widgets_without_a_fixed_stats_page():
    cmake = read("main/CMakeLists.txt")
    header = read_required("main/pet_p4_stats.h")
    stats = read_required("main/pet_p4_stats.c")
    protocol = read("main/pet_p4_protocol.c")
    renderer = read("main/pet_p4_renderer.c")
    desktop = read_workspace("ref/src-tauri/src/lib.rs")

    assert '"pet_p4_stats.c"' in cmake
    assert "pet_p4_stats_model_t" in header
    assert "PET_P4_STATS_HAS_CONTEXT_PERCENT" in header
    assert "pet_p4_stats_update" in stats
    assert 'first_object(payload, usage_keys' in stats
    assert 'object_item(payload, "metrics")' in stats
    assert 'object_item(metrics, "latency")' in stats
    assert 'cJSON_AddBoolToObject(features, "statsPage", false)' in protocol
    assert 'cJSON_CreateString("stats")' not in protocol
    assert 'cJSON_CreateString("main")' in protocol
    assert 'cJSON_CreateString("components")' in protocol
    assert 'cJSON_CreateString("app")' in protocol
    assert 'strcmp(page, "components") != 0' in protocol
    assert '"invalid_page"' in protocol
    assert 'strcmp(page, "stats") == 0' not in renderer
    assert "render_stats_page" not in renderer
    assert 'draw_text_line("AGENT STATUS"' not in renderer
    assert 'pet_p4_miniapp_sync_stats(&state->stats)' in protocol
    touch = read("main/pet_p4_touch.c")
    assert 'pet_p4_miniapp_active()' in touch
    assert '? "app" : "main"' in touch
    assert 'payload.get("metrics")' in desktop


def test_p4_conversation_queue_is_synced_and_rendered_with_pixel_ellipsis():
    header = read("main/pet_p4_protocol.h")
    protocol = read("main/pet_p4_protocol.c")
    renderer = read("main/pet_p4_renderer.c")
    view = read("main/pet_p4_view.c")

    assert "current_session_title" in header
    assert "current_session_notice" in header
    assert "PET_P4_SESSION_QUEUE_MAX 8" in header
    assert "session_queue" in header
    assert "PET_P4_SESSION_QUEUE_ID_MAX 128" in header
    assert "PET_P4_SESSION_QUEUE_CONTENT_MAX 384" in header
    assert "transition_revision" in header
    assert "terminal_until_ms" in header
    assert "session_queue_staging" in header
    assert "session_voice_active" in header
    assert "PET_P4_SESSION_SNAPSHOT_TIMEOUT_MS 30000ULL" in header
    assert "retain_visible_active" in protocol
    assert "json_session_id_array_contains(active_session_ids, previous->id)" in protocol
    assert "session_snapshot_last_seen_ms" in header
    assert 'strcmp(topic, "session/current") == 0' in protocol
    assert "handle_session_current(state, payload)" in protocol
    assert "PET_P4_SESSION_NOTICE_MS" in protocol
    assert "draw_session_queue(state, now_ms)" in renderer
    assert "tray_y" not in renderer
    assert "draw_voice_waveform" in renderer
    assert "state->session_voice_active" in renderer
    assert "item->title," in renderer
    assert "item->content[0] ? item->content : item->state" in renderer
    assert 'json_string(session, "content")' in protocol
    assert 'json_string(payload, "sessionId")' in protocol
    assert 'cJSON_GetObjectItemCaseSensitive(payload, "displayEnabled")' in protocol
    assert 'cJSON_GetObjectItemCaseSensitive(payload, "activeSessionIds")' in protocol
    assert "json_session_id_array_contains(" in protocol
    assert "lifecycle_is_active" in protocol
    assert "pet_p4_session_queue_item_t candidate = {0}" in protocol
    assert "pet_p4_session_queue_item_t *previous = agent_changed" in protocol
    assert "if (!previous) continue" in protocol
    assert "candidate.terminal_until_ms = now_ms + PET_P4_DONE_HOLD_MS" in protocol
    assert '"transitionRevision"' in protocol
    assert '"terminalRemainingMs"' in protocol
    assert "PET_P4_JSON_SAFE_INTEGER_MAX" in protocol
    assert "revision_is_stale" in protocol
    assert "revision_is_same" in protocol
    assert "candidate.terminal_until_ms = now_ms + terminal_remaining_ms" in protocol
    assert "candidate = *previous" in protocol
    assert "previous->terminal_until_ms > now_ms" in protocol
    assert 'lifecycle_is(candidate.state, "error") ? "error" : "done"' in protocol
    assert "unsigned int ordered_count = 0" in protocol
    assert "matched_index = j" in protocol
    assert "matched_index - ordered_count" in protocol
    assert "state->session_queue_staging[ordered_count] = updated_item" in protocol
    assert "state->session_queue_staging[ordered_count] = *previous" in protocol
    assert "pet_p4_session_queue_item_t *selected_item = find_session_queue_item(" in protocol
    assert "session_ids_match(state->session_queue_staging[j].id, previous->id)" in protocol
    assert "(unsigned int) (selected_item - state->session_queue) + 1" in protocol
    assert "mark_current_session_terminal(" in protocol
    assert "(!session_id || !session_id[0])" in protocol
    assert "restore_idle_session_view" in protocol
    assert 'snapshot_stale ? "session_snapshot_timeout"' in protocol
    assert "state->session_snapshot_last_seen_ms = now_ms" in protocol
    assert "should_wait_for_session_card" in view
    assert "state->session_snapshot_last_seen_ms == 0" in view
    assert 'out->body = "休息中"' in view
    assert "out->status = PET_P4_VIEW_STATUS_IDLE" in view
    assert "cJSON_GetArraySize(active_session_ids) == 0" in protocol
    assert "&& (!display_enabled" in protocol
    assert 'copy_text(state->session_queue[0].state' not in protocol
    assert "retained_item" not in protocol
    assert "#define PET_P4_SESSION_CARD_WIDTH 556" in renderer
    assert "#define PET_P4_SESSION_CARD_HEIGHT 120" in renderer
    assert "#define PET_P4_SESSION_CARD_RADIUS 14" in renderer
    assert "#define PET_P4_SESSION_CARD_GAP 8" in renderer
    assert "#define PET_P4_SESSION_CARD_BOTTOM 460" in renderer
    assert "unsigned int visible = state->session_queue_count < 3" in renderer
    assert "draw_session_card_panel(" in renderer
    assert "rgb565(237, 249, 242)" in renderer
    assert "fill_round_rect(x + 4, y + 4, w - 8, h - 8" in renderer
    assert "is_selected ? 7 : 4" in renderer
    assert "rgb565(255, 255, 255)" in renderer
    assert "draw_text_line_vcenter(item->title, x + 30, y + 10, 38, w - 82, ink, 2, true)" in renderer
    assert "static void draw_card_body_lines(" in renderer
    assert "const char *second_line = draw_text_line_medium_vcenter(" in renderer
    assert "first_line_y + 27, 27" in renderer
    assert "draw_card_body_lines(body, x + 30, y + 50, w - 60, muted)" in renderer
    assert "static const char *draw_text_line_medium(" in renderer
    assert "static const char *draw_text_line_medium_vcenter(" in renderer
    assert "pet_p4_view_status_t marker_status = session_view_status(item->state)" in renderer
    assert "draw_working_markers || marker_status != PET_P4_VIEW_STATUS_WORKING" in renderer
    assert "draw_status_marker(marker_status, now_ms, x + w - 26, y + 26)" in renderer
    assert "bool show_session_queue = state && state->session_queue_count > 0;" in renderer
    assert "bool needs_ellipsis = ellipsis && utf8_text_width(cursor, -1, scale) > max_px" in renderer
    assert "bool needs_ellipsis = ellipsis && utf8_text_width_medium(cursor) > max_px" in renderer
    assert "static bool glyph_visual_bounds(" in renderer
    assert "static int text_y_in_box(" in renderer
    assert "box_y * 2 + box_h - 1 - top - bottom" in renderer
    assert "int first_ink_row = glyph.box_h" in renderer
    assert "glyph_alpha_at(bitmap, &glyph, x, y)" in renderer
    assert "int first_ink_row = scaled_height" in renderer
    assert "if (!compact)" in renderer
    assert "int h = 56" in renderer
    assert "w = 72 + utf8_text_width(body, -1, 2)" in renderer
    assert "#define PET_P4_COMPACT_TEXT_OPTICAL_Y (-2)" in renderer
    assert "text_y_in_box(body, 2, y, h) + PET_P4_COMPACT_TEXT_OPTICAL_Y" in renderer
    assert "draw_session_card_panel(x, y, w, h, PET_P4_SESSION_CARD_RADIUS, true, selection)" in renderer
    assert "int banner_w = utf8_text_width(label, -1, 1) + 48" in renderer
    assert "int banner_x = PET_P4_UI_WIDTH - banner_w - 20" in renderer
    assert "int banner_y = 18" in renderer
    assert "banner_w - 44" in renderer
    assert "pet_p4_state_host_connected(state, now_ms)" in renderer
    assert 'label = "客户端未连接"' in renderer
    assert "app->state[0] && show_app_state" in renderer


def test_view_model_is_part_of_p4_component():
    cmake = read("main/CMakeLists.txt")
    header = read("main/pet_p4_view.h")
    source = read("main/pet_p4_view.c")
    main = read("main/pet_p4_main.c")

    assert '"pet_p4_view.c"' in cmake
    assert "PET_P4_VIEW_STATUS_WORKING" in header
    assert "PET_P4_VIEW_STATUS_DONE" in header
    assert "pet_p4_build_view_model" in source
    assert "should_compact_bubble" in source
    assert "pet_p4_build_view_model(&g_state, &view)" in main


def test_p4_runtime_supports_native_usb_and_usb_uart_bridge():
    cmake = read("main/CMakeLists.txt")
    source = read("main/pet_p4_main.c")
    doc = read("README.md")

    assert '#include "driver/usb_serial_jtag.h"' in source
    assert '#include "driver/uart.h"' in source
    assert "esp_driver_usb_serial_jtag" in cmake
    assert "esp_driver_uart" in cmake
    assert "UART_NUM_0" in source
    assert "PET_P4_UART_BAUD 4000000" in source
    assert "uart_rx_task" in source
    assert "transport_send_line" in source
    assert "USB-UART" in doc


def test_p4_protocol_replies_are_routed_back_to_the_command_origin():
    source = read("main/pet_p4_main.c")
    transport = source[
        source.index("static void transport_send_line"):
        source.index("static void send_screenshot_chunk")
    ]
    screenshot = source[
        source.index("static void send_screenshot_chunk"):
        source.index("static bool maybe_handle_debug_line")
    ]

    assert "uart_write_all(line" in transport
    assert "pet_p4_native_usb_send_json_line" in transport
    assert "PET_P4_TRANSPORT_USB_SERIAL_JTAG" in transport
    assert "usb_write_all(line" in transport
    assert "uart_write_all(header" in screenshot
    assert "usb_write_all(header" in screenshot
    assert "reply_ctx" in screenshot

    handler = source[
        source.index("static bool maybe_handle_debug_line"):
        source.index("static bool consume_byte")
    ]
    assert "xSemaphoreTake(g_render_mutex" in handler
    assert "pet_p4_renderer_logical_framebuffer" in handler
    assert "pet_p4_renderer_render" not in handler


def test_p4_runtime_serializes_state_and_defers_native_usb_callback_work():
    main = read("main/pet_p4_main.c")
    native = read("main/pet_p4_usb_native.c")
    native_callback = native[
        native.index("void tud_vendor_rx_cb"):
        native.index("static void tusb_device_task")
    ]

    assert "g_state_mutex = xSemaphoreCreateMutex()" in main
    assert "xSemaphoreTake(g_state_mutex, portMAX_DELAY)" in main
    assert "pet_p4_native_usb_init(" in main
    assert "enqueue_protocol_line" in main
    assert "xQueueSend(g_rx_queue" in native
    assert "native_protocol_worker_task" in native
    assert "xQueueReceive(g_rx_queue" in native
    assert "handle_complete_frame" not in native_callback
    assert "fopen(" not in native_callback


def test_p4_protocol_executor_and_lcd_handoff_do_not_share_mutable_render_state():
    main = read("main/pet_p4_main.c")
    renderer = read("main/pet_p4_renderer.c")
    native_header = read("main/pet_p4_usb_native.h")

    assert "PET_P4_PROTOCOL_QUEUE_DEPTH 24" in main
    assert "protocol_worker_task" in main
    assert "xQueueSend(g_protocol_queue" in main
    assert "xQueueReceive(g_protocol_queue" in main
    assert "memcpy(g_render_state, &g_state, sizeof(*g_render_state))" in main
    assert "pet_p4_renderer_render(g_render_state, &render_view, now_ms)" in main
    render_call = main.index("pet_p4_renderer_render(g_render_state, &render_view, now_ms)")
    unlock = main.rindex("xSemaphoreGive(g_state_mutex)", 0, render_call)
    assert unlock < render_call
    assert "pet_p4_native_protocol_enqueue_fn" in native_header

    assert "PET_P4_NATIVE_OUTPUT_BUFFER_COUNT 2" in renderer
    assert "g_native_framebuffers[PET_P4_NATIVE_OUTPUT_BUFFER_COUNT]" in renderer
    assert "g_native_framebuffer_index + 1U" in renderer
    assert "convert_h264_frame_direct_native" not in renderer
    assert "g_native_framebuffer_authoritative" not in renderer
    assert "g_draw_to_native" not in renderer
    assert 'rotate_landscape_to_panel(), TAG, "rotate logical framebuffer"' in renderer


def test_p4_normal_boot_never_auto_formats_spiffs():
    source = read("main/pet_p4_main.c")

    assert ".format_if_mount_failed = false" in source
    assert "preserving storage for explicit recovery" in source


def test_pc_uses_high_baud_for_p4_ch343_usb_uart():
    usb = read_usb_serial_contract()

    assert "P4_USB_UART_BAUD: u32 = 4_000_000" in usb
    assert "P4_USB_UART_LEGACY_BAUD: u32 = 3_000_000" in usb
    assert "DEFAULT_USB_SERIAL_BAUD: u32 = 921_600" in usb
    assert "serial_baud_candidates_for_device" in usb
    assert "probe_serial_port(port_name, &baud_candidates" in usb
    assert "serialport::new(port_name, baud)" in usb
    assert "usb_uart_wire_bytes_per_sec(P4_USB_UART_BAUD)" in usb
    assert "P4_APPEARANCE_ASSET_CHUNK_SIZE: usize = 20_478" in usb
    assert "P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE: usize = 8 * 1024" in usb
    assert "P4_RAW_APPEARANCE_CHUNK_MAX_ATTEMPTS: u32 = 4" in usb
    assert "recover_raw_asset_stream" in usb
    assert "send_asset_raw_chunk_checked" in usb


def test_p4_runtime_initializes_waveshare_lcd_bsp():
    cmake = read("main/CMakeLists.txt")
    source = read("main/pet_p4_main.c")
    lcd = read("main/pet_p4_lcd.c")
    manifest = read("main/idf_component.yml")
    platformio = read("platformio.ini")
    sdkconfig = read("sdkconfig.defaults")

    assert "waveshare/esp32_p4_wifi6_touch_lcd_4_3" in manifest
    assert "override_path: ../components/esp_lvgl_adapter" in manifest
    assert "override_path: ../components/esp32_p4_wifi6_touch_lcd_4_3" in manifest
    assert "-DBSP_CONFIG_NO_GRAPHIC_LIB=1" in platformio
    assert '"pet_p4_lcd.c"' in cmake
    assert re.search(r"\bREQUIRES[\s\S]*?\busb\b", cmake)
    assert "espressif__esp_lcd_touch" in cmake
    assert "espressif__esp_lcd_touch_gt911" in cmake
    assert '#include "bsp/esp-bsp.h"' in lcd
    assert '#include "bsp/display.h"' in lcd
    assert "bsp_display_new" in lcd
    assert "bsp_display_backlight_on" in lcd
    assert "esp_lcd_dpi_panel_set_pattern" in lcd
    assert "pet_p4_lcd_init()" in source
    assert "debug/lcd" in source
    assert "pet_p4_lcd_backlight_status()" in source
    assert "CONFIG_SPIRAM=y" in sdkconfig


def test_p4_lcd_matches_wlk2802_st7701s_panel_geometry():
    display = read("components/esp32_p4_wifi6_touch_lcd_4_3/include/bsp/display.h")
    bsp = read("components/esp32_p4_wifi6_touch_lcd_4_3/esp32_p4_wifi6_touch_lcd_4_3.c")

    assert "#define BSP_LCD_H_RES              (480)" in display
    assert "#define BSP_LCD_V_RES              (640)" in display
    assert "#define BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS (800)" in display
    assert "#define BSP_LCD_MIPI_DSI_LANE_NUM          (1)" in display
    assert ".h_size = BSP_LCD_H_RES" in bsp
    assert ".v_size = BSP_LCD_V_RES" in bsp
    assert "#define BSP_LCD_PIXEL_FORMAT        LCD_COLOR_PIXEL_FORMAT_RGB565" in display
    assert "#define BSP_LCD_BITS_PER_PIXEL      (16)" in display
    assert ".pixel_format = BSP_LCD_PIXEL_FORMAT" in bsp
    assert ".bits_per_pixel = BSP_LCD_BITS_PER_PIXEL" in bsp
    assert ".rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB" in bsp
    assert ".dpi_clock_freq_mhz = 25" in bsp
    assert ".hsync_front_porch = 10" in bsp
    assert ".hsync_pulse_width = 4" in bsp
    assert ".hsync_back_porch = 20" in bsp
    assert ".vsync_front_porch = 8" in bsp
    assert ".vsync_pulse_width = 4" in bsp
    assert ".vsync_back_porch = 14" in bsp
    assert "{0xC0, (uint8_t[]){0x4F, 0x00}, 2, 0}" in bsp
    assert "{0xC2, (uint8_t[]){0x01, 0x14}, 2, 0}" in bsp
    assert "{0xB0, (uint8_t[]){0x5D}, 1, 0}" in bsp
    assert "{0xED, (uint8_t[]){0xFF, 0x45, 0x67, 0xFA" in bsp
    assert bsp.index("{0x11, (uint8_t[]){0x00}, 0, 120}") < bsp.index("{0x29, (uint8_t[]){0x00}, 0, 0}")
    assert bsp.index("{0x29, (uint8_t[]){0x00}, 0, 0}") < bsp.index("{0x35, (uint8_t[]){0x00}, 1, 0}")
    assert ".flags = {.output_invert = 0}" in bsp


def test_p4_lcd_write_only_command_link_cannot_wait_for_ack_or_id():
    bsp = read("components/esp32_p4_wifi6_touch_lcd_4_3/esp32_p4_wifi6_touch_lcd_4_3.c")
    st7701 = read("components/esp_lcd_st7701/esp_lcd_st7701_mipi.c")

    assert "mipi_dsi_host_ll_enable_cmd_ack(MIPI_DSI_LL_GET_HOST(bus_config.bus_id), false)" in bsp
    assert "MIPI DSI command ACK disabled for write-only panel link" in bsp
    assert ".skip_mipi_id_read = 1" in bsp
    assert "Skip LCD ID read on write-only MIPI link" in st7701
    assert "#define ST7701_HW_RESET_RECOVERY_MS 120" in st7701
    assert "pdMS_TO_TICKS(ST7701_HW_RESET_RECOVERY_MS)" in st7701


def test_p4_asset_commit_loads_manifest_state():
    header = read("main/pet_p4_protocol.h")
    source = read("main/pet_p4_protocol.c")
    assets = read_required("main/pet_p4_assets.c")
    main = read("main/pet_p4_main.c")

    assert "asset_manifest_json" in header
    assert "asset_family_count" in header
    assert "asset_catalog" in header
    assert "asset_revision" in header
    assert "pet_p4_load_asset_manifest" in header
    assert "pet_p4_load_asset_manifest" in source
    assert 'strcmp(topic, "asset/commit") == 0' in source
    assert "pet_p4_load_asset_manifest(state)" in source
    assert "pet_p4_asset_catalog_parse" in source
    assert "json_parse_frame_sizes" in assets
    assert "cJSON_Parse" not in assets
    assert main.index("pet_p4_load_asset_manifest(&g_state)") < main.index("ESP_ERROR_CHECK(init_usb())")
    assert main.index("pet_p4_load_asset_manifest(&g_state)") < main.index("ESP_ERROR_CHECK(init_uart())")


def test_p4_runtime_renders_pet_frames_to_lcd():
    cmake = read("main/CMakeLists.txt")
    main = read("main/pet_p4_main.c")
    lcd = read("main/pet_p4_lcd.c")
    lcd_header = read("main/pet_p4_lcd.h")
    renderer_header = read("main/pet_p4_renderer.h")
    renderer = read("main/pet_p4_renderer.c")
    assets = read_required("main/pet_p4_assets.c")

    assert '"pet_p4_renderer.c"' in cmake
    assert "pet_p4_lcd_draw_rgb565" in lcd_header
    assert "pet_p4_renderer_render" in renderer_header
    assert "esp_lcd_panel_draw_bitmap" in lcd
    assert "g_rgb888_buffer" not in lcd
    assert "x + width, y + height, pixels" in lcd
    assert "esp_lcd_dpi_panel_register_event_callbacks" in lcd
    assert ".on_refresh_done" in lcd
    assert "xSemaphoreTake(g_refresh_done_sem" in lcd
    assert "pet_p4_asset_catalog_t" in read_required("main/pet_p4_assets.h")
    assert "libs/tjpgd/tjpgd.h" in renderer
    assert "jd_prepare" in renderer
    assert "jd_decomp" in renderer
    assert "frameSizes" in assets
    assert "render_fallback_pet" not in renderer
    assert "draw_asset_waiting_marker" not in renderer
    assert "no usable P4 appearance frames yet; keeping bubble/status visible" in renderer
    assert "family_is_selftest" in assets
    assert "render_asset_pet_frame" in renderer
    assert "rotate_landscape_to_panel" in renderer
    assert "PET_P4_UI_WIDTH 640" in renderer
    assert "PET_P4_UI_HEIGHT 480" in renderer
    assert "int panel_x = PET_P4_UI_HEIGHT - 1 - y" in renderer
    assert "int panel_y = x" in renderer
    assert "PET_P4_PANEL_MOUNT_ROTATE_180" not in renderer
    assert "pet_p4_renderer_render(g_render_state, &render_view, now_ms)" in main
    assert "PET_P4_MAIN_RENDER_INTERVAL_MS 50ULL" in main
    assert "PET_P4_MAIN_LOOP_DELAY_MS 1" in main
    assert "now_ms - last_render_ms >= PET_P4_MAIN_RENDER_INTERVAL_MS" in main
    assert "now_ms - last_render_ms >= 250ULL" not in main
    assert "PET_P4_ASSET_DECODE_BACKOFF_MS 500ULL" in renderer
    assert "g_framebuffer_initialized" in renderer
    assert "if (!g_framebuffer_initialized)" in renderer
    assert "PET_P4_ASSET_DECODE_BACKOFF_MS 30000ULL" not in renderer
    assert "pet_p4_lcd_show_color_bar()" not in main


def test_p4_behavior_rotates_native_families_without_per_frame_manifest_parsing():
    cmake = read("main/CMakeLists.txt")
    renderer = read("main/pet_p4_renderer.c")
    behavior = read_required("main/pet_p4_behavior.c")
    behavior_header = read_required("main/pet_p4_behavior.h")
    assets = read_required("main/pet_p4_assets.c")
    touch = read_required("main/pet_p4_touch.c")
    view = read("main/pet_p4_view.c")

    assert '"pet_p4_assets.c"' in cmake
    assert '"pet_p4_behavior.c"' in cmake
    assert "pet_p4_asset_catalog_parse" in assets
    assert 'strcmp(key, "frameDurationMs") == 0' in assets
    assert 'strcmp(key, "durationMs") == 0' in assets
    assert "frame_duration_ms" in read_required("main/pet_p4_assets.h")
    assert "duration_ms" in read_required("main/pet_p4_assets.h")
    assert "PET_P4_ASSET_CATALOG_MAX_FAMILIES 24" in read_required("main/pet_p4_assets.h")
    assert "PET_P4_ASSET_CATALOG_MAX_FRAMES 320" in read_required("main/pet_p4_assets.h")
    assert "heap_caps_calloc(" in assets
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in assets
    assert "pet_p4_asset_catalog_t parsed;" not in assets
    assert 'pet_p4_asset_catalog_find_exact(catalog, "welcome")' in behavior
    assert re.search(r'rotating_prefix\(\s*behavior,\s*catalog,\s*"idle\."', behavior)
    assert re.search(r'rotating_prefix\(\s*behavior,\s*catalog,\s*"working"', behavior)
    assert 'pet_p4_asset_catalog_find_exact(catalog, "waiting_user")' not in behavior
    assert 'strcmp(canonical, "waiting_user")' in behavior
    assert 'pet_p4_asset_catalog_find_exact(catalog, "working.browsing")' in behavior
    assert "asset_duration_ms" in behavior
    assert "frame_duration_ms" in behavior
    assert "entry->duration_ms" in behavior
    assert "selected_since_ms" in behavior_header
    assert "PET_P4_WELCOME_MAX_MS" not in behavior
    assert "PET_P4_TOUCH_REACTION_MAX_MS" not in touch
    assert "playback_elapsed_ms" in renderer
    assert "playback_duration_ms = asset.duration_ms" in renderer
    assert "cycle_elapsed_ms * (unsigned long long) asset.frames" in renderer
    assert "now_ms - g_behavior.selected_since_ms" in renderer
    assert "cJSON_Parse" not in renderer
    assert "#define PET_P4_ASSET_CACHE_SLOTS 2U" in renderer
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in renderer
    assert "load_asset_stream_cache(asset.path, fs_path, stream_size)" in renderer
    assert "memcpy(g_jpeg_input, stream->bytes + offset, frame_size)" in renderer
    assert "pet_p4_asset_read_all(logical_path, entry->bytes, expected_size)" in renderer
    assert "fseek(g_asset_file" not in renderer
    assert "fread(g_jpeg_input" not in renderer
    assert "g_session_overlay_cache" in renderer
    assert "session_overlay_signature" in renderer
    assert "copy_session_overlay_cache" in renderer
    assert "draw_session_queue_working_markers" in renderer
    assert "PET_P4_VIEW_STATUS_WAITING" in view
    assert 'strcmp(status, "speaking") == 0' in view
    assert "return PET_P4_VIEW_STATUS_WORKING;" in view
    assert "PET_P4_VIEW_STATUS_SPEAKING" not in view
    assert 'strcmp(status, "speaking") == 0) return PET_P4_VIEW_STATUS_WORKING' in renderer


def test_p4_mjpeg_prefers_hardware_decoder_with_software_fallback():
    renderer = read("main/pet_p4_renderer.c")
    cmake = read("main/CMakeLists.txt")
    defaults = read("sdkconfig.defaults")

    assert "esp_driver_jpeg" in cmake
    assert "driver/jpeg_decode.h" in renderer
    assert "jpeg_alloc_decoder_mem" in renderer
    assert "jpeg_new_decoder_engine" in renderer
    assert "jpeg_decoder_get_info" in renderer
    assert "jpeg_decoder_process" in renderer
    assert "JPEG_DECODE_OUT_FORMAT_RGB565" in renderer
    assert "P4 JPEG decoder=hardware" in renderer
    assert "CONFIG_LV_USE_TJPGD=y" in defaults
    assert "PET_P4_TJPGD_WORK_BUFFER_BYTES" in renderer
    assert "tjpgd_input_func" in renderer
    assert "tjpgd_output_func" in renderer
    assert "using TJPGD fallback" in renderer


def test_p4_h264_uses_the_v9_single_slice_full_duration_aspect_fit_contract():
    renderer = read("main/pet_p4_renderer.c")
    cmake = read("main/CMakeLists.txt")
    component = read("main/idf_component.yml")
    assets = read("main/pet_p4_assets.c")
    desktop = read_usb_serial_contract()
    prebuilder = read_workspace("scripts/prepare-p4-ready-assets.mjs")

    assert "P4_APPEARANCE_WIDTH: u32 = 640" in desktop
    assert "P4_APPEARANCE_HEIGHT: u32 = 480" in desktop
    assert "P4_APPEARANCE_FPS: u32 = 15" in desktop
    assert "P4_APPEARANCE_MAX_FRAMES: u32 = 225" in desktop
    assert "P4_APPEARANCE_H264_CRF: u32 = 27" in desktop
    assert "P4_READY_PROFILE_VERSION: u32 = 9" in desktop
    assert '"p4-h264-v1"' in desktop
    assert '"annex-b"' in desktop
    assert "force_original_aspect_ratio=decrease" in desktop
    assert "pad={width}:{height}:(ow-iw)/2:(oh-ih)/2:black" in desktop
    assert "threads=1:sliced-threads=0" in desktop
    assert "rewrite_p4_h264_sps" in desktop
    assert "parse_p4_h264_stream" in desktop
    assert "esp_h264" in component
    assert "espressif__esp_h264" in cmake
    assert "PET_P4_ASSET_CODEC_H264" in assets
    assert "esp_h264_dec_sw_new" in renderer
    assert "ESP_H264_RAW_FMT_I420" in renderer
    assert "h264_stream_has_single_slice_access_units" in renderer
    assert "expected one slice per access unit" in renderer
    assert "cycle_elapsed_ms = playback_elapsed_ms % playback_duration_ms" in renderer
    assert "target_frame < g_h264_decoded_frame" in renderer
    assert "strcmp(g_h264_fs_path, fs_path) != 0" in renderer
    assert "rewriteH264Sps" in prebuilder
    assert "parseH264Stream" in prebuilder
    assert "threads=1:sliced-threads=0" in prebuilder
    assert "P4 ready pack differs from factory-config pin" in prebuilder


def test_saved_and_codex_imported_appearances_are_prepared_before_sync():
    desktop_lib = read_workspace("ref/src-tauri/src/lib.rs")
    appearance_store = read_workspace("ref/src/lib/appearance-store.js")
    gallery = read_workspace("ref/src/AppearanceGallery.jsx")

    import_command = desktop_lib[
        desktop_lib.index("async fn import_codex_pet("):
        desktop_lib.index("// --- USB serial Tauri commands ---")
    ]
    assert "codex_import::import_codex_pet" in import_command
    assert "usb_serial::prepare_p4_appearance" in import_command
    assert import_command.index("codex_import::import_codex_pet") < import_command.index(
        "usb_serial::prepare_p4_appearance"
    )
    assert 'invoke("prepare_p4_appearance"' in appearance_store
    assert "await installCodexCommunityPet(parsed.petId)" in gallery
    assert "const result = await onImport(parsed.petId)" in gallery


def test_p4_15fps_h264_assets_use_rgb565_and_ppa_render_scheduler():
    renderer = read("main/pet_p4_renderer.c")
    main = read("main/pet_p4_main.c")
    cmake = read("main/CMakeLists.txt")
    sdkconfig = read("sdkconfig.defaults")
    protocol_header = read("main/pet_p4_protocol.h")
    desktop = read_usb_serial_contract()

    assert "P4_APPEARANCE_FPS: u32 = 15" in desktop
    assert "P4_APPEARANCE_MAX_FRAMES: u32 = 225" in desktop
    assert "P4_APPEARANCE_ASSET_CHUNK_SIZE: usize = 20_478" in desktop
    assert "PET_P4_ASSET_MANIFEST_MAX 65536" in protocol_header
    assert "PET_P4_MAIN_RENDER_INTERVAL_MS 50ULL" in main
    assert "CONFIG_ESP_H264_DUAL_TASK=y" in sdkconfig
    assert "CONFIG_ESP_H264_DUAL_TASK_CORE=1" in sdkconfig
    assert 'PET_P4_H264_DECODER_MODE "software-dual"' in renderer
    assert "PET_P4_H264_MAX_DECODE_FRAMES_PER_RENDER 1U" in renderer
    assert "PET_P4_H264_PRIMARY_TASK_PRIORITY 16U" in renderer
    assert "vTaskPrioritySet(NULL, PET_P4_H264_PRIMARY_TASK_PRIORITY)" in renderer
    assert "vTaskPrioritySet(NULL, original_priority)" in renderer
    assert "decoded_in_pass < PET_P4_H264_MAX_DECODE_FRAMES_PER_RENDER" in renderer
    assert "esp_driver_ppa" in cmake
    assert 'driver/ppa.h' in renderer
    assert "ppa_register_client" in renderer
    assert "ppa_do_scale_rotate_mirror" in renderer
    assert "PPA_SRM_ROTATION_ANGLE_270" in renderer
    assert "PPA_SRM_ROTATION_ANGLE_0" in renderer
    assert "asset_fit_layout" in renderer
    assert "scale_x_sixteenths" in renderer
    assert "JPEG_DOWN_SAMPLING_YUV420" in renderer
    assert "decoded_stride" in renderer
    assert ".pic_w = (uint32_t) src_w" in renderer
    assert ".pic_h = (uint32_t) storage_h" in renderer
    assert ".block_h = (uint32_t) src_h" in renderer
    assert "pack_h264_i420_for_ppa" in renderer
    assert "sy * src_stride" in renderer
    assert ".block_offset_y = (uint32_t) dst_y" in renderer
    assert ".scale_x = scale" in renderer
    assert ".scale_y = scale" in renderer
    assert "memset(g_framebuffer, 0" in renderer
    assert "ppa_scale_factor_exact" not in renderer
    assert "P4 asset scale=PPA" in renderer
    assert "PPA_SRM_COLOR_MODE_RGB565" in renderer
    assert "P4 rotation=PPA" in renderer
    assert "P4 render perf fps=" in renderer
    assert "PET_P4_RENDER_FRAME_BUDGET_US 66667U" in renderer
    assert "memcpy(" in renderer


def test_p4_rgb565_output_uses_matching_rgb_panel_order():
    renderer = read("main/pet_p4_renderer.c")
    bsp = read("components/esp32_p4_wifi6_touch_lcd_4_3/esp32_p4_wifi6_touch_lcd_4_3.c")
    project = read("CMakeLists.txt")
    component_center = renderer[
        renderer.index("static void render_component_center_page"):
        renderer.index("static void draw_text_right", renderer.index("static void render_component_center_page"))
    ]

    assert '.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB' in bsp
    assert "rgb565_to_panel_bgr" not in renderer
    assert ".rgb_swap = true" not in renderer
    assert ".rgb_swap = false" in renderer
    assert ".byte_swap = false" in renderer
    assert "panel-order=RGB" in renderer
    assert "rgb565(255, 163, 31)" in component_center
    assert "rgb565(31, 163, 255)" not in component_center
    assert "Pre-swap red/blue" not in renderer
    assert 'set(PROJECT_VER "0.7.27-p4")' in project


def test_p4_renderer_keeps_screen_visible_when_assets_are_unusable():
    renderer = read("main/pet_p4_renderer.c")

    assert "bool drew_asset = render_asset_pet_frame(state, view, now_ms)" in renderer
    assert "if (!drew_asset)" in renderer
    assert "g_asset_decode_blocked_until_ms" in renderer
    assert "PET_P4_ASSET_DECODE_BACKOFF_MS 500ULL" in renderer
    assert "g_framebuffer_initialized" in renderer
    assert "if (!g_framebuffer_initialized)" in renderer
    assert "draw_bubble(view, now_ms)" in renderer
    assert "draw_boot_diagnostic(now_ms)" in renderer


def test_p4_renderer_boot_diagnostic_is_visible_on_black_screen_failures():
    renderer = read("main/pet_p4_renderer.c")

    assert "draw_boot_diagnostic" in renderer
    assert "P4_BOOT_DIAGNOSTIC_MS" in renderer
    assert "rgb565(255, 255, 255)" in renderer
    assert "rgb565(255, 120, 0)" in renderer


def test_p4_flash_layout_allocates_dual_10m_appearance_slots_on_32m_flash():
    partitions = read("partitions.csv")
    platformio = read("platformio.ini")
    sdkconfig = read("sdkconfig.defaults")
    migration = read_required("tools/migrate_to_ab.ps1")
    runtime_readme = read("README.md")
    protocol_doc = read("protocol.md")
    desktop = read_usb_serial_contract()

    assert "nvs,      data, nvs,     0x9000,   0x6000," in partitions
    assert "ota_0,    app,  ota_0,   0x10000,  0x280000," in partitions
    assert "ota_1,    app,  ota_1,   0x290000, 0x280000," in partitions
    assert "otadata,  data, ota,     0x510000, 0x2000," in partitions
    assert "storage,  data, spiffs,  0x520000, 0x6E0000," in partitions
    assert "appearance0,data, 0x40,   0xC00000, 0xA00000," in partitions
    assert "appearance1,data, 0x41,   0x1600000,0xA00000," in partitions
    assert "board_build.flash_size = 32MB" in platformio
    assert "board_upload.flash_size = 32MB" in platformio
    assert "CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y" in sdkconfig
    assert "CONFIG_ESPTOOLPY_FLASHSIZE=\"32MB\"" in sdkconfig
    assert "CONFIG_SPIFFS_PAGE_SIZE=512" in sdkconfig
    assert "CONFIG_SPIFFS_GC_MAX_RUNS=64" in sdkconfig
    assert "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" in sdkconfig
    assert "read-flash 0x210000 0x1000000" in migration
    assert "factory-flash -Port $Port -FactoryReset" in migration
    assert "Get-FileHash" in migration
    assert "device already uses an A/B partition table" in migration
    assert "P4_FIRMWARE_MAX_IMAGE_SIZE: usize = 0x280000" in desktop
    assert "two 2.5MiB slots" in runtime_readme
    assert "6.875MiB SPIFFS" in runtime_readme
    assert "both 10MiB appearance partitions" in runtime_readme
    assert ".\\tools\\migrate_to_ab.ps1 -Port COM5 -FactoryReset" in runtime_readme
    assert "inactive 2.5MiB OTA slot" in protocol_doc
    assert "two 4MB OTA slots" not in runtime_readme
    assert "two 4MiB OTA slots" not in runtime_readme
    assert "16MB SPIFFS volume" not in runtime_readme
    assert "restore the old filesystem byte-for-byte" in runtime_readme
    assert "3 Mbps" not in runtime_readme
    assert "3 Mbps" not in protocol_doc


def test_p4_ab_firmware_ota_is_verified_acknowledged_and_exposed_by_pc():
    cmake = read("main/CMakeLists.txt")
    project = read("CMakeLists.txt")
    ota = read_required("main/pet_p4_ota.c")
    protocol = read("main/pet_p4_protocol.c")
    main = read("main/pet_p4_main.c")
    desktop = read_usb_serial_contract()
    desktop_lib = read_workspace("ref/src-tauri/src/lib.rs")
    desktop_ui = read_workspace("ref/src/dashboard/FirmwareUpdateModal.jsx")

    assert 'set(PROJECT_VER "0.7.27-p4")' in project
    assert "esp_app_get_description()" in protocol
    assert "PET_P4_FW_VERSION" not in protocol
    assert '"pet_p4_ota.c"' in cmake
    assert "esp_ota_get_next_update_partition" in ota
    assert "esp_ota_begin" in ota
    assert "esp_ota_write" in ota
    assert "mbedtls_sha256_update" in ota
    assert "esp_ota_get_partition_description" in ota
    assert "esp_ota_set_boot_partition" in ota
    assert "esp_ota_mark_app_valid_cancel_rollback" in ota
    assert "esp_ota_mark_app_invalid_rollback_and_reboot" in ota
    assert "firmware project identity mismatch" in ota
    assert "PET_P4_OTA_TRANSFER_IDLE_TIMEOUT_MS" in ota
    assert "PET_P4_OTA_RESTART_DELAY_MS 5000ULL" in ota
    assert "PET_P4_OTA_VALIDATE_MIN_SAMPLES" in ota
    assert "g_commit_result" in ota
    assert "g_ota_mutex" in ota
    assert "pet_p4_ota_transfer_active" in ota
    assert "pet_p4_diagnostics_reboot_pending" in ota
    assert "modf(item->valuedouble" in ota
    for topic in [
        "firmware/begin",
        "firmware/chunk",
        "firmware/commit",
        "firmware/abort",
        "firmware/query",
    ]:
        assert topic in ota
    assert 'cJSON_AddBoolToObject(capabilities, "firmwareOta", true)' in protocol
    assert 'cJSON_AddNumberToObject(firmware_update, "chunkBytes", 4 * 1024)' in protocol
    assert "pet_p4_ota_runtime_ready" in main
    assert "pet_p4_ota_runtime_failed" in main
    assert "runtime_render_healthy" in main
    assert "P4_FIRMWARE_CHUNK_SIZE: usize = 4 * 1024" in desktop
    assert "P4_FIRMWARE_CHUNK_MAX_ATTEMPTS: usize = 3" in desktop
    assert "P4_FIRMWARE_COMMIT_ACK_TIMEOUT: Duration = Duration::from_secs(3)" in desktop
    assert "sequence + 1 == g_next_sequence" in ota
    assert "g_commit_result.next_sequence" in ota
    assert '"firmware/ack"' in desktop
    assert "Sha256::digest" in desktop
    assert "parse_esp_idf_app_descriptor" in desktop
    assert "expected_next_sequence" in desktop
    assert "wait_for_firmware_validation" in desktop
    assert '"imageState"' in desktop
    assert "usb_update_firmware" in desktop_lib
    assert "expected_board_device_id" in desktop_lib
    assert 'listen("usb-firmware-update-progress"' in desktop_ui


def test_pc_and_p4_build_identity_share_protocol_schema_and_diagnostics():
    project = read("CMakeLists.txt")
    main_cmake = read("main/CMakeLists.txt")
    build_info = read_required("main/pet_p4_build_info.h")
    protocol = read("main/pet_p4_protocol.c")
    diagnostics = read("main/pet_p4_diagnostics.c")
    protocol_doc = read("protocol.md")
    runtime_readme = read("README.md")
    desktop_build = read_workspace("ref/src-tauri/build.rs")
    desktop_serial = read_usb_serial_contract()
    desktop_lib = read_workspace("ref/src-tauri/src/lib.rs")
    desktop_ui = read_workspace("ref/src/dashboard/DeviceDiagnosticsModal.jsx")

    assert "set(PET_P4_PROTOCOL_SCHEMA 5)" in project
    assert "rev-parse --short=12 HEAD" in project
    assert 'PET_P4_BUILD_ID="${PET_P4_BUILD_ID}"' in main_cmake
    assert "PET_P4_PROTOCOL_SCHEMA=${PET_P4_PROTOCOL_SCHEMA}" in main_cmake
    assert '#define PET_P4_BUILD_GIT_SHA "unknown"' in build_info
    assert '#define PET_P4_BUILD_ID "unknown"' in build_info
    assert "esp_app_get_description()" in protocol
    for field in ["buildId", "gitSha", "buildDirty", "protocolSchema"]:
        assert f'"{field}"' in protocol
        assert f'"{field}"' in diagnostics

    assert "const PET_MANAGER_PROTOCOL_SCHEMA: u32 = 5" in desktop_build
    assert '"rev-parse", "--short=12", "HEAD"' in desktop_build
    assert "PET_MANAGER_BUILD_ID" in desktop_build
    assert "pub build_id: String" in desktop_serial
    assert "pub protocol_schema: u32" in desktop_serial
    assert "fn desktop_build_info()" in desktop_lib
    assert 'object.insert("desktopBuild".to_string(), desktop_build_info())' in desktop_lib
    assert "runtime.buildId" in desktop_ui
    assert "desktopBuild.buildId" in desktop_ui
    assert "protocolSchema" in protocol_doc
    assert "buildId" in runtime_readme


def test_p4_runtime_pauses_rendering_during_asset_transfer():
    header = read("main/pet_p4_protocol.h")
    source = read("main/pet_p4_protocol.c")
    main = read("main/pet_p4_main.c")

    assert "asset_transfer_active" in header
    assert "state->asset_transfer_active = true" in source
    assert "state->asset_transfer_active = false" in source
    assert "!g_state.asset_transfer_active" in main


def test_p4_asset_checksum_failure_cleans_staged_file():
    source = read("main/pet_p4_protocol.c")

    assert "checksum mismatch expected_size=" in source
    assert "remove(fs_path)" in source


def test_p4_asset_chunks_are_acknowledged_for_pacing():
    source = read("main/pet_p4_protocol.c")

    assert "bool handle_asset_chunk" in source
    assert "send_asset_ack_indexed(" in source
    assert '"chunk",' in source
    assert 'json_string(payload, "index")' in source


def test_p4_asset_chunks_keep_staged_file_open_between_acks():
    source = read("main/pet_p4_protocol.c")
    chunk_body = source[
        source.index("static bool write_asset_chunk_bytes"):
        source.index("static void handle_asset_stat")
    ]

    assert "static FILE *g_asset_chunk_file" in source
    assert "close_asset_chunk_file()" in source
    assert "g_asset_chunk_file = file" in chunk_body
    assert "fclose(file)" not in chunk_body
    assert "close_asset_chunk_file();" in source[
        source.index("static void handle_asset_file"):
        source.index("static void handle_asset_topic")
    ]


def test_p4_asset_transfer_prepares_spiffs_and_raw_slot_without_rereads():
    source = read("main/pet_p4_protocol.c")
    raw = read_required("main/pet_p4_raw_assets.c")
    renderer = read("main/pet_p4_renderer.c")
    cmake = read("main/CMakeLists.txt")
    desktop = read_usb_serial_contract()
    chunk_body = source[
        source.index("static bool write_asset_chunk_bytes"):
        source.index("static bool handle_asset_chunk")
    ]
    file_body = source[
        source.index("static void handle_asset_file"):
        source.index("static void handle_asset_topic")
    ]

    assert "written = fwrite(data, 1, data_len, file)" in chunk_body
    assert "PET_P4_ASSET_WRITE_SLICE" not in source
    assert "g_asset_chunk_checksum = fnv1a64_update" in chunk_body
    assert "g_asset_chunk_size +=" in chunk_body
    assert "if (tracked_stat)" in file_body
    assert "else if (!read_file_stat" in file_body
    assert "rename(" not in file_body
    assert "missing transferred file" in file_body
    assert "run_spiffs_gc_yielding()" in source
    assert 'esp_spiffs_gc("storage", target)' in source
    assert 'json_u64(payload, "totalBytes", 0)' in source
    assert 'json_u64(payload, "rawBytes", total_bytes)' in source
    assert "pet_p4_raw_assets_prepare(raw_bytes" in source
    assert "esp_partition_erase_range" in raw
    assert "erase_range_yielding" in raw
    assert "PET_P4_RAW_ERASE_SLICE_BYTES (16U * 1024U)" in raw
    assert "vTaskDelay(pdMS_TO_TICKS(2))" in raw
    assert "esp_partition_write(partition, g_transfer.cursor" in raw
    assert "g_transfer.next_chunk_index" in raw
    assert "g_transfer.last_chunk_checksum" in raw
    assert "raw appearance duplicate chunk does not match" in raw
    assert "PET_P4_RAW_HEADER_BYTES 4096U" in raw
    assert "pet_p4_raw_assets_commit" in source
    assert "pet_p4_asset_read_all" in renderer
    assert '"pet_p4_raw_assets.c"' in cmake
    assert "ASSET_BEGIN_ACK_TIMEOUT: Duration = Duration::from_secs(120)" in desktop
    assert '"totalBytes": total_bytes' in desktop

def test_p4_raw_asset_chunks_bypass_base64_with_bounded_ram_and_checksum_ack():
    protocol = read("main/pet_p4_protocol.c")
    header = read("main/pet_p4_protocol.h")
    main = read("main/pet_p4_main.c")

    assert "PET_P4_RAW_ASSET_CHUNK_LIMIT (64 * 1024)" in protocol
    assert 'strcmp(topic, "asset/raw-chunk") == 0' in protocol
    assert "pet_p4_consume_raw_asset_bytes" in protocol
    assert "fnv1a64_bytes(g_raw_asset_chunk.data" in protocol
    assert '"raw-chunk"' in protocol
    assert 'cJSON_AddStringToObject(payload, "index", index)' in protocol
    assert "send_asset_ack_indexed" in protocol
    assert "pet_p4_raw_asset_chunk_active" in header
    assert "consume_transport_bytes" in main
    assert "pet_p4_consume_raw_asset_bytes" in main
    assert '"rawAssetChunks", true' in protocol
    assert '"rawAssetChunkBytes", PET_P4_RAW_ASSET_CHUNK_LIMIT' in protocol
    assert "PET_P4_UART_RX_BUFFER_BYTES (128 * 1024)" in main
    assert "PET_P4_UART_READ_CHUNK_BYTES 2048" in main
    desktop = read_usb_serial_contract()
    assert "P4_RAW_APPEARANCE_ASSET_CHUNK_SIZE: usize = 8 * 1024" in desktop
    assert "P4_RAW_APPEARANCE_CHUNK_MAX_ATTEMPTS: u32 = 4" in desktop


def test_p4_serial_asset_sync_uses_replaceable_slot_until_commit():
    source = read("main/pet_p4_protocol.c")

    assert "ensure_asset_transfer_slot" in source
    assert "return PET_P4_RAW_APPEARANCE_SLOT" in source
    assert "g_asset_transfer_slot = PET_P4_RAW_APPEARANCE_SLOT" in source
    assert "pet_p4_asset_clean_legacy_files()" in source
    assert "clear_slot_files(g_asset_transfer_slot)" in source
    assert "pet_p4_asset_fs_path_for_slot(g_asset_transfer_slot, path" in source
    assert "pet_p4_asset_set_active_slot(g_asset_transfer_slot)" in source
    assert "reset_asset_transfer_slot()" in source


def test_p4_inactive_slot_cleanup_restarts_after_each_spiffs_delete():
    serial = read("main/pet_p4_protocol.c")
    native = read("main/pet_p4_usb_native.c")
    serial_clear = serial[
        serial.index("static bool clear_slot_files"):
        serial.index("static bool ensure_asset_transfer_slot")
    ]
    native_clear = native[
        native.index("static bool clear_slot_files"):
        native.index("static bool ensure_transfer_slot")
    ]

    for clear in (serial_clear, native_clear):
        assert "while (true)" in clear
        assert clear.index("closedir(dir)") < clear.index("remove(fs_path)")
        assert "if (!found)" in clear
        assert "errno != ENOENT" in clear
        assert "pet_p4_raw_assets_invalidate" in clear
    assert "if (!clear_slot_files(g_asset_transfer_slot)) return false" in serial
    assert "if (!clear_slot_files(g_target_slot)) return false" in native


def test_p4_native_usb_does_not_leave_a_serial_raw_transfer_open():
    native = read("main/pet_p4_usb_native.c")
    desktop = read_usb_serial_contract()
    native_sync = desktop[
        desktop.index("pub fn sync_appearance_p4_native"):
        desktop.index("fn ensure_p4_native_full_pack_supported")
    ]

    assert '"topic": "asset/begin"' not in native_sync
    assert "ensure_p4_native_full_pack_supported(&assets)?" in native_sync
    assert native_sync.index("wait_asset_ack_timeout") < native_sync.index(
        "ensure_p4_native_full_pack_supported(&assets)?"
    ) < native_sync.index("begin native USB bulk")
    assert "pet_p4_raw_assets_invalidate" in native
    assert '#include "pet_p4_raw_assets.h"' in native


def test_p4_desktop_serializes_every_appearance_bulk_path():
    desktop = read_usb_serial_contract()
    serial_sync = desktop[
        desktop.index("pub fn sync_appearance_p4<F>"):
        desktop.index("fn plan_incremental_appearance_sync")
    ]
    native_only_sync = desktop[
        desktop.index("pub fn sync_appearance_p4_native_only<F>"):
        desktop.index("fn plan_incremental_appearance_sync")
    ]

    assert "asset_transfer_guard" in serial_sync
    assert "asset_transfer_guard" in native_only_sync

def test_p4_pre_marker_pack_migration_preserves_the_builtin_slot():
    serial = read("main/pet_p4_protocol.c")
    native = read("main/pet_p4_usb_native.c")
    protocol = read("protocol.md")

    assert "!read_ready_slot_pack_id(g_asset_active_slot" in serial
    assert "pet_p4_asset_slot_has_pack_id(g_asset_active_slot)" in serial
    assert "pet_p4_asset_mark_slot_ready(g_asset_active_slot)" in serial
    assert "migrating active pre-marker appearance slot" in serial
    for source in (serial, native):
        assert "!clear_slot_files(0) || !clear_slot_files(1)" not in source
    assert "bool pet_p4_asset_slot_has_pack_id(int slot)" in serial
    assert "never erases protected" in protocol
    assert "slot `0`" in protocol


def test_p4_appearance_slots_can_reactivate_an_exact_cached_pack():
    source = read("main/pet_p4_protocol.c")
    desktop = read_usb_serial_contract()
    protocol = read("protocol.md")

    assert '"appearanceSlotReuse", true' in source
    assert '"slotReuse", true' in source
    assert '"builtinSlot", 0' in source
    assert '"customSlot", PET_P4_RAW_APPEARANCE_SLOT' in source
    assert '"builtinProtected", true' in source
    assert "return PET_P4_RAW_APPEARANCE_SLOT" in source
    assert 'strcmp(topic, "asset/slot-query") == 0' in source
    assert 'strcmp(topic, "asset/activate") == 0' in source
    assert "read_slot_pack_id" in source
    assert "appearance pack is not present in requested slot" in source
    assert "pet_p4_asset_set_active_slot(slot)" in source
    assert '"packId": pack_id' in desktop
    assert '"topic": "asset/slot-query"' in desktop
    assert '"topic": "asset/activate"' in desktop
    assert "try_activate_cached_p4_pack" in desktop
    assert '"packId"' in protocol
    assert "`asset/slot-query`" in protocol
    assert "`asset/activate`" in protocol


def test_p4_appearance_slots_require_transaction_ready_markers():
    source = read("main/pet_p4_protocol.c")
    header = read("main/pet_p4_protocol.h")
    native = read("main/pet_p4_usb_native.c")
    commit_body = source[
        source.index('strcmp(topic, "asset/commit") == 0'):
        source.index('strcmp(topic, "asset/patch-commit") == 0')
    ]

    assert 'PET_P4_SPIFFS_PREFIX "/s%d_ready"' in source
    assert "read_ready_slot_pack_id" in source
    assert "strcmp(marker_pack_id, manifest_pack_id)" in source
    assert "bool pet_p4_asset_mark_slot_ready(int slot)" in source
    assert "pet_p4_asset_mark_slot_ready" in header
    assert commit_body.index("pet_p4_raw_assets_commit") < commit_body.index("pet_p4_asset_mark_slot_ready")
    assert commit_body.index("pet_p4_asset_mark_slot_ready") < commit_body.index("pet_p4_asset_set_active_slot")
    native_commit = native[native.index("static void handle_commit") :]
    assert native_commit.index("pet_p4_asset_mark_slot_ready") < native_commit.index("pet_p4_asset_set_active_slot")


def test_p4_asset_chunks_carry_decoded_size_guard():
    firmware = read("main/pet_p4_protocol.c")
    usb_source = read_usb_serial_contract()

    assert 'json_u64(payload, "size", 0)' in firmware
    assert "decoded_len != expected_decoded_len" in firmware
    assert '"chunk size mismatch expected=%llu actual=%u"' in firmware
    assert '"size": decoded_size' in usb_source
    assert "chunk.len()" in usb_source


def test_p4_hardware_inputs_are_debounced_persistent_and_configurable():
    cmake = read("main/CMakeLists.txt")
    core = read_required("main/pet_p4_input_core.c")
    core_header = read_required("main/pet_p4_input_core.h")
    input_source = read_required("main/pet_p4_input.c")
    protocol = read("main/pet_p4_protocol.c")
    main = read("main/pet_p4_main.c")

    assert '"pet_p4_input.c"' in cmake
    assert '"pet_p4_input_core.c"' in cmake
    assert "PET_P4_INPUT_SW1_GPIO GPIO_NUM_50" in input_source
    assert "PET_P4_INPUT_SW2_GPIO GPIO_NUM_49" in input_source
    assert "PET_P4_INPUT_SW3_GPIO GPIO_NUM_5" in input_source
    assert "PET_P4_INPUT_ENCODER_PRESS_GPIO GPIO_NUM_4" in input_source
    assert "PET_P4_INPUT_ENCODER_B_GPIO GPIO_NUM_3" in input_source
    assert "PET_P4_INPUT_ENCODER_A_GPIO GPIO_NUM_2" in input_source
    assert "PET_P4_INPUT_JOYSTICK_X_GPIO GPIO_NUM_21" in input_source
    assert "PET_P4_INPUT_JOYSTICK_Y_GPIO GPIO_NUM_20" in input_source
    assert "PET_P4_INPUT_JOYSTICK_X_CHANNEL ADC1_GPIO21_CHANNEL" in input_source
    assert "PET_P4_INPUT_JOYSTICK_Y_CHANNEL ADC1_GPIO20_CHANNEL" in input_source
    assert '"esp_adc"' in cmake or "esp_adc" in cmake
    assert "PET_P4_INPUT_SAMPLE_MS 5" in input_source
    assert "PET_P4_INPUT_DEBOUNCE_MS 25" in input_source
    assert "PET_P4_INPUT_LONG_PRESS_MS 700" in input_source
    assert "PET_P4_INPUT_QUEUE_LENGTH 32" in input_source
    assert ".pull_up_en = GPIO_PULLUP_ENABLE" in input_source
    assert "vTaskDelayUntil" in input_source
    assert "PET_P4_BUTTON_EVENT_LONG_RELEASE" in core_header
    assert "events |= PET_P4_BUTTON_EVENT_LONG_RELEASE" in core
    assert "PET_P4_INPUT_GESTURE_HOLD_START" in input_source
    assert "PET_P4_INPUT_GESTURE_HOLD_END" in input_source
    assert '"button.sw1.hold", "voice_ptt"' in input_source
    assert '"button.sw2.short_press", "component_center"' in input_source
    assert '"button.sw1.short_press", "disabled"' in input_source
    assert '"button.sw3.short_press", "page_back"' in input_source
    assert '"button.sw3.long_press", "disabled"' in input_source
    assert '"button.encoder.short_press", "page_enter"' in input_source
    assert '"button.encoder.long_press", "disabled"' in input_source
    assert '"button.encoder.hold", "disabled"' in input_source
    assert '"knob.rotate_cw", "session_next"' in input_source
    assert '"knob.rotate_ccw", "session_previous"' in input_source
    assert '"joystick.up", "disabled"' in input_source
    assert '"joystick.down", "disabled"' in input_source
    assert 'strcmp(binding->action, "session_next") == 0' in input_source
    assert 'strcmp(binding->action, "session_previous") == 0' in input_source
    assert "state->current_session_index = selected + 1" in input_source
    assert 'cJSON_AddStringToObject(payload, "sessionId", selected->id)' in input_source
    assert 'cJSON_AddNumberToObject(payload, "sessionCount", state->session_queue_count)' in input_source
    assert "migrate_v2_config" in input_source
    assert "migrate_v3_config" in input_source
    assert "migrate_v4_config" in input_source
    assert "migrate_input_config" in input_source
    assert "PET_P4_INPUT_CONFIG_VERSION 5" in read_required("main/pet_p4_input.h")
    assert "center_binding" not in input_source
    assert '"page_toggle"' in input_source
    assert '"page_enter"' in input_source
    assert '"page_back"' in input_source
    assert 'strcmp(binding->action, "page_toggle") == 0' in input_source
    assert 'strcmp(binding->action, "page_enter") == 0' in input_source
    assert 'strcmp(binding->action, "page_back") == 0' in input_source
    assert 'strcmp(state->screen_page, "components") == 0' in input_source
    assert "PET_P4_INPUT_GESTURE_ROTATE, (int) direction" in input_source
    assert "PET_P4_INPUT_GESTURE_DIRECTION" in input_source
    assert "pet_p4_joystick_decoder_update" in input_source
    assert "bounded_direction_threshold" in core
    assert "activation_left" in core_header
    assert "activation_right" in core_header
    assert "activation_up" in core_header
    assert "activation_down" in core_header
    assert 'event_name = "joystick.up"' in input_source
    assert 'event_name = "joystick.down"' in input_source
    assert "catalog_delta = -1" in input_source
    assert "catalog_delta = 1" in input_source
    assert "pet_p4_miniapp_catalog_move(catalog_delta)" in input_source
    assert "-(int) direction" not in input_source
    assert "pet_p4_miniapp_catalog_move(event.delta)" in input_source
    assert "pet_p4_miniapp_catalog_move(-event.delta)" not in input_source
    assert "pet_p4_miniapp_catalog_activate_selected" in input_source
    assert "PET_P4_MINIAPP_CATALOG_MAX 16" in read_required("main/pet_p4_miniapp.h")
    assert "MINIAPP_CATALOG_PATH" in read_required("main/pet_p4_miniapp.c")
    renderer = read_required("main/pet_p4_renderer.c")
    assert 'draw_text_line("组件中心"' in renderer
    assert '"已安装 %u 个"' in renderer
    assert '"摇杆四向选择 · 中按进入 · SW3返回"' in renderer
    assert '"SW3短按返回"' in renderer
    assert "dispatch_component_binding_event" in input_source
    assert "pet_p4_miniapp_resolve_input" in input_source
    assert "pet_p4_miniapp_has_input(long_event_name)" in input_source
    assert "nvs_set_blob" in input_source
    assert "nvs_get_blob" in input_source
    assert 'strcmp(topic, "input/config") == 0' in protocol
    assert 'strcmp(topic, "input/config-query") == 0' in protocol
    assert "pet_p4_input_send_config_state" in input_source
    assert '"input/config-state"' in input_source
    assert 'cJSON_AddItemToObject(config, "bindings", bindings)' in input_source
    assert 'strcmp(topic, "control/command") == 0' in protocol
    assert '"input/config-ack"' in input_source
    assert '"button-config-ack"' in input_source
    assert '"input/event"' in input_source
    assert 'cJSON_AddBoolToObject(capabilities, "inputs", true)' in protocol
    assert 'cJSON_AddBoolToObject(capabilities, "inputConfig", true)' in protocol
    assert "pet_p4_input_config_t *next = heap_caps_malloc(" in input_source
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in input_source
    assert "if (!next) next = malloc(sizeof(*next))" in input_source
    assert "pet_p4_input_config_t next;" not in input_source
    assert "PET_P4_TRANSPORT_RX_TASK_STACK_BYTES 8192" in main
    assert "pet_p4_input_init()" in main
    assert "pet_p4_input_process(&g_state" in main


def test_p4_device_ptt_stream_is_validated_and_relayed_by_pc():
    cmake = read("main/CMakeLists.txt")
    audio = read_required("main/pet_p4_audio.c")
    audio_header = read_required("main/pet_p4_audio.h")
    protocol = read("main/pet_p4_protocol.c")
    assets = read("main/pet_p4_assets.c")
    assets_header = read("main/pet_p4_assets.h")
    diagnostics = read("main/pet_p4_diagnostics.c")
    input_source = read("main/pet_p4_input.c")
    main = read("main/pet_p4_main.c")
    native_usb = read("main/pet_p4_usb_native.c")
    desktop = read_workspace("ref/src-tauri/src/usb_audio.rs")
    desktop_lib = read_workspace("ref/src-tauri/src/lib.rs")
    usb_serial = read_usb_serial_contract()

    assert '"pet_p4_audio.c"' in cmake
    assert "PET_P4_AUDIO_SAMPLE_RATE 16000" in audio_header
    assert "PET_P4_AUDIO_FRAME_MS 20" in audio_header
    assert "bsp_audio_codec_microphone_init" in audio
    assert "bsp_audio_codec_es8311_duplex_init" in audio
    assert "ESP_CODEC_DEV_WORK_MODE_BOTH" in read_required(
        "components/esp32_p4_wifi6_touch_lcd_4_3/esp32_p4_wifi6_touch_lcd_4_3.c"
    )
    assert "esp_codec_dev_read" in audio
    assert "sizeof(encoded), &encoded_len" in audio
    assert 'send_topic("audio/begin"' in audio
    assert 'cJSON_AddBoolToObject(payload, "sessionQueueEmpty", session_queue_empty)' in audio
    assert "g_capture_session_queue_empty" in audio
    assert 'send_topic("audio/chunk"' in audio
    assert 'send_topic("audio/end"' in audio
    assert "PET_P4_AUDIO_MAX_CAPTURE_MS 30000ULL" in audio
    assert 'strcmp(binding->action, "voice_ptt") == 0' in input_source
    assert 'strcmp(gesture, "hold_start") == 0' in input_source
    assert 'strcmp(gesture, "hold_end") == 0' in input_source
    assert "pet_p4_audio_capture_start(state && state->session_queue_count == 0)" in input_source
    assert "pet_p4_audio_init" in main
    assert "g_transport_mutex" in main
    assert 'strcmp(topic, "audio/control") == 0' in protocol
    assert 'strcmp(topic, "audio/query") == 0' in protocol
    assert "g_tx_mutex" in native_usb
    assert "native_write_all(header" in native_usb
    assert "audio/begin" in desktop
    assert "audio/chunk" in desktop
    assert "audio/end" in desktop
    assert "audio frame checksum mismatch" in desktop
    assert "127.0.0.1" in desktop
    assert "usb-audio-stream" in desktop_lib
    assert "usb-audio-result" in desktop_lib
    assert "audio_begin_session_queue_empty(&payload)" in desktop_lib
    assert '"sessionQueueEmpty": context.session_queue_empty_at_start' in desktop_lib
    assert "bsp_audio_codec_speaker_init" in audio
    assert "esp_codec_dev_write" in audio
    assert "wav_seek_pcm" in audio
    assert "pet_p4_behavior_select" in audio
    assert "xQueueOverwrite" in audio
    assert "pet_p4_audio_process(&g_state" in main
    assert '"audioPlayback", audio_playback_ready' in protocol
    assert 'cJSON_CreateString("p4-pcm-wav-v1")' in protocol
    assert '"audioPlaybackReady", pet_p4_audio_playback_ready()' in diagnostics
    assert "char audio_path[PET_P4_ASSET_PATH_MAX]" in assets_header
    assert 'strcmp(key, "audioPath") == 0' in assets
    assert '"p4/audio/{}.wav"' in usb_serial
    assert "validate_p4_audio_wav" in usb_serial
    assert 'kind: "p4-audio"' in usb_serial


def test_p4_gt911_touch_and_local_lifecycle_are_product_integrated():
    cmake = read("main/CMakeLists.txt")
    touch = read_required("main/pet_p4_touch.c")
    core = read_required("main/pet_p4_touch_core.c")
    protocol = read("main/pet_p4_protocol.c")
    protocol_header = read("main/pet_p4_protocol.h")
    renderer = read("main/pet_p4_renderer.c")
    main = read("main/pet_p4_main.c")
    diagnostics = read("main/pet_p4_diagnostics.c")

    assert '"pet_p4_touch.c"' in cmake
    assert '"pet_p4_touch_core.c"' in cmake
    assert "esp_lcd_touch_new_i2c_gt911" in touch
    assert ".rst_gpio_num = GPIO_NUM_NC" in touch
    assert "esp_lcd_touch_get_data" in touch
    assert "pet_p4_touch_panel_to_ui" in core
    assert "*ui_x = panel_y" in core
    assert "*ui_y = PET_P4_PANEL_WIDTH - 1 - panel_x" in core
    assert '"screen.region.tap"' in touch
    assert '"screen.region.long_press"' in touch
    assert "pet_p4_miniapp_dispatch_input" in touch
    assert "pet_p4_state_request_touch" in touch
    assert "PET_P4_DONE_HOLD_MS 60000ULL" in protocol_header
    assert "pet_p4_state_process" in protocol
    assert '"sessionQueueCount"' in diagnostics
    assert '"sessionQueueIds"' in diagnostics
    assert '"currentSessionId"' in diagnostics
    assert '"retainedSessionCount"' in diagnostics
    assert "pet_p4_state_effective_lifecycle" in renderer
    assert 'if (!lifecycle_is(lifecycle, "idle")) return false;' in protocol
    assert '&& lifecycle_is(lifecycle, "idle")) {' in protocol
    assert "behavior->welcome_until_ms = 0" in read_required("main/pet_p4_behavior.c")
    assert "draw_touch_feedback" in renderer
    assert "draw_page_indicator" in renderer
    assert "#define PET_P4_PAGE_INDICATOR_Y 16" in renderer
    assert "int y = PET_P4_PAGE_INDICATOR_Y;" in renderer
    assert "PET_P4_PAGE_INDICATOR_TITLE_GAP" in renderer
    assert 'cJSON_AddBoolToObject(capabilities, "touch", touch_ready)' in protocol
    assert "pet_p4_touch_init()" in main
    assert "pet_p4_touch_process(&g_state" in main
    assert '"touchReady", pet_p4_touch_ready()' in diagnostics
    assert '"touchDroppedEvents", pet_p4_touch_dropped_events()' in diagnostics
    assert re.search(
        r"if \(err != ESP_OK\)[\s\S]*?vTaskDelayUntil\([\s\S]*?continue;[\s\S]*?pet_p4_touch_decoder_update",
        touch,
    )


def test_windows_tooling_forces_utf8_and_ascii_paths():
    shared = read_workspace("scripts/windows-utf8.ps1")
    p4_tool = read_required("tools/p4.ps1")
    migration = read_required("tools/migrate_to_ab.ps1")
    dev_launcher = read_workspace("scripts/dev-manager.ps1")
    open_manager = read_workspace("scripts/open-manager.ps1")
    root_readme = read_workspace("README.md")
    runtime_readme = read("README.md")
    gitignore = read_workspace(".gitignore")

    assert '$env:PYTHONUTF8 = "1"' in shared
    assert '$env:PYTHONIOENCODING = "utf-8"' in shared
    assert 'chcp.com 65001' in shared
    assert '$env:TEMP = $tempRoot' in shared
    assert '$env:TMP = $tempRoot' in shared
    assert 'Test-HachimoAsciiPath' in shared
    assert 'PLATFORMIO_CORE_DIR' in shared
    assert 'Set-HachimoUserToolEnvironment' in shared
    assert "Get-HachimoDefaultToolingRoot" in shared
    assert '.hachimo-tooling' in shared
    assert '$resolved.Length -gt 48' in shared
    assert 'Join-Path $resolvedRoot ".tooling"' not in shared

    assert 'Get-DevicePartitionLayout' in p4_tool
    assert 'Assert-CurrentAbLayout' in p4_tool
    assert 'Get-HachimoEsptoolPath' in p4_tool
    assert 'image-info' in p4_tool
    assert 'Read-P4HelloVersion' in p4_tool
    assert p4_tool.count("4000000") >= 2
    assert "3000000" not in p4_tool
    assert '[System.IO.File]::ReadAllBytes($lockPath)' in p4_tool
    assert '[System.IO.File]::WriteAllBytes($lockPath, $originalLock)' in p4_tool
    assert 'factory-flash' in p4_tool
    assert '$FactoryReset' in p4_tool
    assert '0x820000' not in p4_tool.split('write-flash', 1)[1]

    assert '$env:USERPROFILE' not in migration
    assert 'Resolve-HachimoPlatformIoCoreDir' in migration
    assert 'Get-HachimoEsptoolPath' in migration
    assert 'Join-Path $PSScriptRoot "p4.ps1"' in migration
    assert 'build -PlatformIoCoreDir $coreDir' in migration
    assert 'Initialize-HachimoUtf8Environment' in dev_launcher
    assert 'Initialize-HachimoUtf8Environment' in open_manager

    assert '.\\tools\\p4.ps1 build' in runtime_readme
    assert '.\\tools\\p4.ps1 flash -Port COM5' in runtime_readme
    assert '.\\tools\\p4.ps1 factory-flash -Port COM5 -FactoryReset' in runtime_readme
    assert '$env:USERPROFILE\\.platformio' not in runtime_readme
    assert '.\\tools\\p4.ps1 build' in root_readme
    assert '$env:USERPROFILE\\.platformio' not in root_readme
    assert '.tooling/' in gitignore

if __name__ == "__main__":
    tests = [
        test_protocol_doc_declares_usb_only_p4_runtime,
        test_windows_tooling_forces_utf8_and_ascii_paths,
        test_p4_native_usb_vendor_bulk_contract_is_declared,
        test_pc_p4_sync_prefers_native_usb_bulk_transport,
        test_firmware_contract_accepts_p4_assets_and_rejects_linux_mp4_ota,
        test_p4_bounded_miniapp_contract_is_explicit_and_heap_safe,
        test_p4_token_widget_uses_bounded_stats_instead_of_host_readers,
        test_builtin_tool_widgets_declare_bounded_completion_cycles,
        test_builtin_tool_widgets_use_three_keys_and_fit_p4_limits,
        test_p4_widget_delete_clears_persistence_and_returns_to_main,
        test_p4_widget_inventory_is_request_matched_and_capacity_bounded,
        test_p4_component_packages_commit_through_validated_ab_catalog_snapshots,
        test_builtin_pixel_games_use_bounded_native_game_presets,
        test_generic_scene_runtime_is_shared_by_games_and_tools,
        test_p4_identity_and_protocol_nack_are_explicit,
        test_p4_diagnostics_are_persisted_and_asset_safe,
        test_platformio_board_metadata_matches_actual_p4_hardware,
        test_firmware_contract_accepts_speech_card_and_stats_snapshot,
        test_p4_conversation_queue_is_synced_and_rendered_with_pixel_ellipsis,
        test_p4_stats_model_feeds_widgets_without_a_fixed_stats_page,
        test_view_model_is_part_of_p4_component,
        test_p4_runtime_supports_native_usb_and_usb_uart_bridge,
        test_p4_protocol_replies_are_routed_back_to_the_command_origin,
        test_p4_runtime_serializes_state_and_defers_native_usb_callback_work,
        test_p4_protocol_executor_and_lcd_handoff_do_not_share_mutable_render_state,
        test_p4_normal_boot_never_auto_formats_spiffs,
        test_pc_uses_high_baud_for_p4_ch343_usb_uart,
        test_p4_runtime_initializes_waveshare_lcd_bsp,
        test_p4_lcd_matches_wlk2802_st7701s_panel_geometry,
        test_p4_lcd_write_only_command_link_cannot_wait_for_ack_or_id,
        test_p4_asset_commit_loads_manifest_state,
        test_p4_runtime_renders_pet_frames_to_lcd,
        test_p4_behavior_rotates_native_families_without_per_frame_manifest_parsing,
        test_p4_mjpeg_prefers_hardware_decoder_with_software_fallback,
        test_p4_h264_uses_the_v9_single_slice_full_duration_aspect_fit_contract,
        test_saved_and_codex_imported_appearances_are_prepared_before_sync,
        test_p4_15fps_h264_assets_use_rgb565_and_ppa_render_scheduler,
        test_p4_rgb565_output_uses_matching_rgb_panel_order,
        test_p4_renderer_keeps_screen_visible_when_assets_are_unusable,
        test_p4_renderer_boot_diagnostic_is_visible_on_black_screen_failures,
        test_p4_flash_layout_allocates_dual_10m_appearance_slots_on_32m_flash,
        test_p4_ab_firmware_ota_is_verified_acknowledged_and_exposed_by_pc,
        test_pc_and_p4_build_identity_share_protocol_schema_and_diagnostics,
        test_p4_runtime_pauses_rendering_during_asset_transfer,
        test_p4_asset_checksum_failure_cleans_staged_file,
        test_p4_asset_chunks_are_acknowledged_for_pacing,
        test_p4_asset_chunks_keep_staged_file_open_between_acks,
        test_p4_asset_transfer_prepares_spiffs_and_raw_slot_without_rereads,
        test_p4_raw_asset_chunks_bypass_base64_with_bounded_ram_and_checksum_ack,
        test_p4_serial_asset_sync_uses_replaceable_slot_until_commit,
        test_p4_inactive_slot_cleanup_restarts_after_each_spiffs_delete,
        test_p4_native_usb_does_not_leave_a_serial_raw_transfer_open,
        test_p4_pre_marker_pack_migration_preserves_the_builtin_slot,
        test_p4_appearance_slots_can_reactivate_an_exact_cached_pack,
        test_p4_appearance_slots_require_transaction_ready_markers,
        test_p4_asset_chunks_carry_decoded_size_guard,
        test_p4_hardware_inputs_are_debounced_persistent_and_configurable,
        test_p4_device_ptt_stream_is_validated_and_relayed_by_pc,
        test_p4_gt911_touch_and_local_lifecycle_are_product_integrated,
    ]
    for test in tests:
        test()
        print(f"ok {test.__name__}")
