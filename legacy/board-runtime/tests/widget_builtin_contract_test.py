#!/usr/bin/env python3
"""[Input] Built-in .clawpkg packages and the Linux board widget runtime.
[Output] Contract coverage for supported screen/physical inputs, real finite-state
game rounds, dynamic rendering, and bounded tick when/then completion.
[Pos] board-runtime host contract test.
[Sync] If this file changes, update board-runtime/README.md when behavior changes.
"""

import importlib.util
import json
import pathlib
import re
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILTINS = ROOT / "builtin-clawpkgs"
REF_BUILTINS = ROOT.parent / "ref" / "builtin-clawpkgs"
RUNTIME_PATH = ROOT / "board-widget-runtime.py"

spec = importlib.util.spec_from_file_location("board_widget_runtime", RUNTIME_PATH)
runtime_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime_mod)


class BuiltinWidgetContractTest(unittest.TestCase):
    def _runtime_for(self, widget_id):
        widget_path = BUILTINS / widget_id / "runtime" / "widget.json"
        widget_spec = json.loads(widget_path.read_text(encoding="utf-8"))
        runtime = runtime_mod.WidgetRuntime(pathlib.Path("/tmp"))
        runtime.spec = widget_spec
        runtime.state = widget_spec["initial_state"]
        runtime.page = widget_spec.get("initial_page")
        runtime.vars = {
            name: declaration.get("init")
            for name, declaration in widget_spec["vars"].items()
        }
        return runtime

    def test_builtin_manifests_declare_game_or_tool_kind(self):
        expected = {
            "block-combo": "game",
            "snake-turn": "game",
            "ten-second-tap": "game",
            "slack-off-countdown": "tool",
            "tomato-clock": "tool",
            "drink-reminder": "tool",
            "token-usage": "tool",
        }
        for widget_id, kind in expected.items():
            with self.subTest(widget=widget_id):
                manifest = json.loads(
                    (BUILTINS / widget_id / "component.json").read_text(encoding="utf-8")
                )
                self.assertEqual(manifest["kind"], kind)

    def test_tool_widget_mirrors_are_byte_identical(self):
        for widget_id in (
            "slack-off-countdown",
            "tomato-clock",
            "drink-reminder",
            "token-usage",
        ):
            with self.subTest(widget=widget_id):
                board_root = BUILTINS / widget_id
                ref_root = REF_BUILTINS / widget_id
                board_files = {
                    path.relative_to(board_root)
                    for path in board_root.rglob("*")
                    if path.is_file()
                }
                ref_files = {
                    path.relative_to(ref_root)
                    for path in ref_root.rglob("*")
                    if path.is_file()
                }
                self.assertEqual(board_files, ref_files)
                for relative_path in board_files:
                    self.assertEqual(
                        (board_root / relative_path).read_bytes(),
                        (ref_root / relative_path).read_bytes(),
                        str(relative_path),
                    )

    def test_tool_widgets_use_sw1_sw2_sw3_short_press_contract(self):
        expected = {
            "slack-off-countdown": {
                "SW1": "clock.pause_resume",
                "SW2": "clock.switch_view",
                "SW3": "clock.reset_countdown",
            },
            "tomato-clock": {
                "SW1": "tomato.start_pause",
                "SW2": "tomato.skip_phase",
                "SW3": "tomato.reset_phase",
            },
            "drink-reminder": {
                "SW1": "reminder.acknowledge",
                "SW2": "reminder.pause_resume",
                "SW3": "reminder.switch_view",
            },
            "token-usage": {
                "SW1": "stats.show_total",
                "SW2": "stats.show_input",
                "SW3": "stats.show_output",
            },
        }
        for widget_id, action_by_control in expected.items():
            with self.subTest(widget=widget_id):
                buttons = json.loads(
                    (BUILTINS / widget_id / "buttons.json").read_text(encoding="utf-8")
                )
                sw_bindings = {
                    binding["control"]: binding
                    for binding in buttons
                    if binding["control"].startswith("SW")
                }
                self.assertEqual(set(sw_bindings), {"SW1", "SW2", "SW3"})
                for control, action in action_by_control.items():
                    self.assertEqual(sw_bindings[control]["action"], action)
                    self.assertEqual(
                        sw_bindings[control]["event"],
                        f"button.{control.lower()}.short_press",
                    )
                self.assertIn(
                    {
                        "control": "前方旋钮",
                        "event": "button.encoder.long_press",
                        "action": "page_main",
                        "label": "返回桌宠",
                    },
                    buttons,
                )

    def test_tool_widgets_use_black_pixel_tool_layout_with_semantic_icons(self):
        initial_visuals = {
            "slack-off-countdown": ("candy", "tool", "coffee"),
            "tomato-clock": ("sunset", "tool", "timer"),
            "drink-reminder": ("mint", "tool", "droplet"),
            "token-usage": ("arcade", "tool", "gauge"),
        }
        for widget_id, (palette, layout, sprite) in initial_visuals.items():
            with self.subTest(widget=widget_id):
                preview = json.loads(
                    (BUILTINS / widget_id / "negative-screen.json").read_text(
                        encoding="utf-8"
                    )
                )["dashboard"]
                self.assertEqual(preview["visualStyle"], "pixel")
                self.assertEqual(preview["visualPalette"], palette)
                self.assertEqual(preview["visualLayout"], layout)
                self.assertEqual(preview["visualSprite"], sprite)

                payload = self._runtime_for(widget_id).render_payload()
                self.assertIn("visualStyle=pixel", payload)
                self.assertIn(f"visualPalette={palette}", payload)
                self.assertIn(f"visualLayout={layout}", payload)
                self.assertIn(f"visualSprite={sprite}", payload)

        slack = self._runtime_for("slack-off-countdown")
        self.assertTrue(slack.dispatch_action("clock.pause_resume"))
        self.assertIn("visualSprite=timer", slack.render_payload())

        tomato = self._runtime_for("tomato-clock")
        self.assertTrue(tomato.dispatch_action("tomato.start_pause"))
        self.assertIn("visualSprite=star", tomato.render_payload())

        drink = self._runtime_for("drink-reminder")
        drink.state = "due"
        self.assertIn("visualSprite=droplet", drink.render_payload())

        token = self._runtime_for("token-usage")
        self.assertTrue(token.dispatch_action("stats.show_input"))
        self.assertIn("visualPalette=mint", token.render_payload())
        self.assertIn("visualSprite=bolt", token.render_payload())

    def test_builtins_use_supported_negative_screen_controls(self):
        allowed_controls = {
            "屏幕区域": {"screen.region.tap", "screen.region.long_press"},
            "SW1": {"button.sw1.short_press", "button.sw1.long_press"},
            "SW2": {"button.sw2.short_press", "button.sw2.long_press"},
            "SW3": {"button.sw3.short_press", "button.sw3.long_press"},
            "前方旋钮": {
                "button.encoder.short_press",
                "button.encoder.long_press",
                "knob.rotate_cw",
                "knob.rotate_ccw",
            },
        }
        for widget_dir in BUILTINS.iterdir():
            if not widget_dir.is_dir():
                continue
            with self.subTest(widget=widget_dir.name):
                buttons = json.loads((widget_dir / "buttons.json").read_text(encoding="utf-8"))
                for binding in buttons:
                    self.assertIn(binding["control"], allowed_controls)
                    self.assertIn(binding["event"], allowed_controls[binding["control"]])
                    self.assertNotIn("button.primary", binding["event"])
                actions = [binding["action"] for binding in buttons]
                self.assertEqual(
                    len(actions),
                    len(set(actions)),
                    "actions must be unique so each row can be remapped independently",
                )

                widget = json.loads((widget_dir / "runtime" / "widget.json").read_text(encoding="utf-8"))
                if "game" in widget:
                    with self.assertRaisesRegex(
                        runtime_mod.ValidationError,
                        "requires ESP32-P4 p4-bounded-v2",
                    ):
                        runtime_mod.validate_widget(widget)
                else:
                    runtime_mod.validate_widget(widget)
                runtime_mod.validate_buttons(buttons, widget.get("transitions", []))

                self.assertTrue(
                    widget.get("tick") or widget.get("readers") or widget.get("initial_state") not in {"idle", "waiting"},
                    "widget must have a useful default state or automatic data source",
                )

    def test_p4_native_games_declare_real_engines_instead_of_fake_state_rounds(self):
        cases = {
            "block-combo": {
                "type": "blocks",
                "tick_ms": 480,
                "actions": {"start", "left", "right", "rotate", "drop"},
            },
            "snake-turn": {
                "type": "snake",
                "tick_ms": 220,
                "actions": {"start", "left", "right"},
            },
        }
        for widget_id, expected in cases.items():
            with self.subTest(widget=widget_id):
                widget = json.loads(
                    (BUILTINS / widget_id / "runtime" / "widget.json").read_text(encoding="utf-8")
                )
                game = widget["game"]
                self.assertEqual(game["type"], expected["type"])
                self.assertEqual(game["tick_ms"], expected["tick_ms"])
                self.assertEqual(set(game["actions"]), expected["actions"])
                self.assertEqual(game["playing_state"], "playing")
                self.assertEqual(game["result_state"], "result")
                self.assertEqual(game["score_var"], "score")
                with self.assertRaisesRegex(
                    runtime_mod.ValidationError,
                    "requires ESP32-P4 p4-bounded-v2",
                ):
                    runtime_mod.validate_widget(widget)

    def test_ten_second_tap_keeps_bounded_finite_state_scoring(self):
        spec = json.loads(
            (BUILTINS / "ten-second-tap" / "runtime" / "widget.json").read_text(encoding="utf-8")
        )
        runtime = runtime_mod.WidgetRuntime(pathlib.Path("/tmp"))
        runtime.spec = spec
        runtime.state = spec["initial_state"]
        runtime.vars = {
            name: declaration.get("init")
            for name, declaration in spec["vars"].items()
        }
        self.assertTrue(runtime.dispatch_action("tap.sw1_score"))
        self.assertEqual(runtime.state, "playing")
        self.assertEqual(runtime.vars["score"], 1)
        for action, expected_score in (("tap.sw2_score", 2), ("tap.sw3_score", 3)):
            self.assertTrue(runtime.dispatch_action(action))
            self.assertEqual(runtime.state, "playing")
            self.assertEqual(runtime.vars["score"], expected_score)

        buttons = json.loads(
            (BUILTINS / "ten-second-tap" / "buttons.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            {binding["control"] for binding in buttons if binding["control"].startswith("SW")},
            {"SW1", "SW2", "SW3"},
        )

    def test_tomato_clock_tracks_phase_progress_and_three_key_actions(self):
        runtime = self._runtime_for("tomato-clock")
        self.assertIn("metricValue=25:00", runtime.render_payload())
        self.assertIn("progress=100:本轮剩余", runtime.render_payload())

        self.assertTrue(runtime.dispatch_action("tomato.start_pause"))
        self.assertEqual(runtime.state, "focus_paused")
        self.assertTrue(runtime.dispatch_action("tomato.skip_phase"))
        self.assertEqual(runtime.state, "rest")
        self.assertEqual(runtime.vars["remaining_s"], 300)
        self.assertEqual(runtime.vars["phase_total_s"], 300)
        self.assertEqual(runtime.vars["completed_sessions"], 1)

        runtime.vars["remaining_s"] = 123
        self.assertTrue(runtime.dispatch_action("tomato.reset_phase"))
        self.assertEqual(runtime.state, "rest")
        self.assertEqual(runtime.vars["remaining_s"], 300)
        self.assertEqual(runtime.vars["phase_total_s"], 300)

        runtime.state = "focus"
        runtime.vars["remaining_s"] = 1
        self.assertTrue(runtime._apply_tick_rule(runtime.spec["tick"][0]))
        self.assertEqual(runtime.state, "rest")
        self.assertEqual(runtime.vars["remaining_s"], 300)
        self.assertEqual(runtime.vars["phase_total_s"], 300)
        self.assertEqual(runtime.vars["completed_sessions"], 2)

    def test_drink_reminder_counts_down_to_due_and_switches_today_view(self):
        runtime = self._runtime_for("drink-reminder")
        self.assertEqual(runtime.state, "counting")
        self.assertEqual(runtime.page, "next")
        self.assertIn("metricValue=60", runtime.render_payload())

        self.assertTrue(runtime.dispatch_action("reminder.acknowledge"))
        self.assertEqual(runtime.vars["drink_count_today"], 1)
        self.assertTrue(runtime.dispatch_action("reminder.switch_view"))
        self.assertEqual(runtime.page, "today")
        self.assertIn("metricValue=1", runtime.render_payload())
        self.assertIn("metricUnit=次", runtime.render_payload())

        self.assertTrue(runtime.dispatch_action("reminder.pause_resume"))
        self.assertEqual(runtime.state, "paused")
        self.assertTrue(runtime.dispatch_action("reminder.pause_resume"))
        self.assertEqual(runtime.state, "counting")

        runtime.page = "next"
        runtime.vars["remaining_min"] = 1
        self.assertTrue(runtime._apply_tick_rule(runtime.spec["tick"][0]))
        self.assertEqual(runtime.state, "due")
        self.assertEqual(runtime.vars["remaining_min"], 0)
        self.assertIn("headline=该喝水啦", runtime.render_payload())

    def test_token_dashboard_reads_stable_fields_and_switches_three_pages(self):
        runtime = self._runtime_for("token-usage")
        source = "\n".join([
            "STATS_DASHBOARD_V1",
            "agent=Codex",
            "headline=Widget smoke test",
            "tokenTotal=18.4K",
            "tokenInput=13K",
            "tokenOutput=2.4K",
            "tokenCache=缓存 3K",
        ])
        for reader in runtime.spec["readers"].values():
            match = re.search(reader["field_pattern"], source)
            self.assertIsNotNone(match)
            runtime.vars[reader["into"]] = match.group(1)

        total = runtime.render_payload()
        self.assertIn("title=Token 仪表盘", total)
        self.assertIn("metricLabel=总 Token", total)
        self.assertIn("metricValue=18.4K", total)
        self.assertIn("note=缓存 3K", total)
        self.assertIn("visualLayout=tool", total)

        self.assertTrue(runtime.dispatch_action("stats.show_input"))
        self.assertEqual(runtime.page, "input")
        self.assertIn("metricValue=13K", runtime.render_payload())
        self.assertTrue(runtime.dispatch_action("stats.show_output"))
        self.assertEqual(runtime.page, "output")
        self.assertIn("metricValue=2.4K", runtime.render_payload())
        self.assertTrue(runtime.dispatch_action("stats.show_total"))
        self.assertEqual(runtime.page, "total")

    def test_slack_countdown_pauses_switches_view_resets_and_stops_at_zero(self):
        runtime = self._runtime_for("slack-off-countdown")
        self.assertIn("metricValue=133", runtime.render_payload())
        self.assertIn("progress=100:今日剩余", runtime.render_payload())

        self.assertTrue(runtime.dispatch_action("clock.pause_resume"))
        self.assertEqual(runtime.state, "paused")
        self.assertEqual(runtime.spec["tick"][0]["while_state"], "showing")
        self.assertEqual(runtime.vars["minutes_left"], 133)

        self.assertTrue(runtime.dispatch_action("clock.switch_view"))
        self.assertEqual(runtime.page, "offhour")
        self.assertIn("metricValue=18", runtime.render_payload())

        runtime.vars["minutes_left"] = 1
        self.assertTrue(runtime.dispatch_action("clock.pause_resume"))
        self.assertEqual(runtime.state, "showing")
        self.assertTrue(runtime._apply_tick_rule(runtime.spec["tick"][0]))
        self.assertEqual(runtime.state, "done")
        self.assertEqual(runtime.vars["minutes_left"], 0)

        self.assertTrue(runtime.dispatch_action("clock.reset_countdown"))
        self.assertEqual(runtime.state, "showing")
        self.assertEqual(runtime.vars["minutes_left"], 133)

    def test_linux_widget_delete_is_dispatched_and_acknowledged_transactionally(self):
        source = (ROOT / "src" / "board_server.c").read_text(encoding="utf-8")
        self.assertIn('strcmp(virtual_topic, "widget/delete") == 0', source)
        self.assertIn("br_handle_widget_delete(server, payload);", source)
        self.assertIn("br_widget_send_ack(", source)
        self.assertIn('"delete",\n    true,', source)
        self.assertIn('br_atomic_write_text(active_widget_path, "")', source)
        self.assertIn('br_atomic_write_text(screen_page_path, "main")', source)
        self.assertIn("rename(target, deleting_target)", source)
        self.assertIn("rename(previous, deleting_previous)", source)
        self.assertIn("br_widget_lock(server)", source)
        self.assertIn("unlink(state_path)", source)

    def test_linux_widget_chunks_are_integrity_checked_and_ack_identity_matches_host(self):
        source = (ROOT / "src" / "board_server.c").read_text(encoding="utf-8")
        self.assertIn("br_widget_send_ack_ex(", source)
        self.assertIn('"decodedSize"', source)
        self.assertIn("br_asset_checksum_hex_valid(expected_checksum)", source)
        self.assertIn("br_asset_decode_b64_file(target, out_path)", source)
        self.assertIn(
            "br_asset_file_stats_checksum(out_path, &actual_size, actual_checksum)",
            source,
        )
        self.assertRegex(
            source,
            r'br_widget_send_ack_ex\(\s*server,\s*tid,\s*"chunk",\s*true,'
            r'[\s\S]*?rel,\s*true,\s*index_val,\s*true,\s*actual_size,',
        )

    def test_nested_switch_page_slots_render_vars(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "widgets" / "slack-off-countdown" / "runtime").mkdir(parents=True)
            for name in ("runtime/widget.json", "buttons.json"):
                src = BUILTINS / "slack-off-countdown" / name
                dst = root / "widgets" / "slack-off-countdown" / name
                dst.write_text(src.read_text(encoding="utf-8"), encoding="utf-8")

            widget = runtime_mod.WidgetRuntime(root)
            widget.load("slack-off-countdown")
            payload = widget.render_payload()
            self.assertIn("metricValue=133", payload)
            self.assertNotIn("{'var'", payload)

    def test_reserved_page_main_action_returns_to_pet_screen(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            widget = runtime_mod.WidgetRuntime(root)
            widget.spec = {"transitions": []}
            widget.state = "playing"

            self.assertTrue(widget.dispatch_action("page_main"))
            self.assertEqual((root / ".screen-page").read_text(encoding="utf-8"), "main\n")

    def test_tick_completion_supports_bounded_game_countdowns(self):
        spec = {
            "schema_version": 1,
            "vars": {"remaining_s": {"type": "int", "init": 1}},
            "states": ["ready", "playing", "result"],
            "initial_state": "playing",
            "transitions": [],
            "tick": [{
                "every_ms": 1000,
                "while_state": "playing",
                "inc": {"remaining_s": -1},
                "when": {"var": "remaining_s", "lte": 0},
                "then": {"to": "result", "set": {"remaining_s": 0}},
            }],
            "dashboard": {"metricValue": {"var": "remaining_s"}},
        }
        runtime_mod.validate_widget(spec)
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            runtime = runtime_mod.WidgetRuntime(root)
            runtime.spec = spec
            runtime.state = "playing"
            runtime.vars = {"remaining_s": 1}
            tick = spec["tick"][0]
            changed = runtime._apply_tick_rule(tick)
            self.assertTrue(changed)
            self.assertEqual(runtime.state, "result")
            self.assertEqual(runtime.vars["remaining_s"], 0)

    def test_pixel_visual_presets_are_bounded_and_follow_game_state(self):
        spec = {
            "schema_version": 1,
            "vars": {"score": {"type": "int", "init": 0}},
            "states": ["ready", "playing", "result"],
            "initial_state": "ready",
            "transitions": [],
            "dashboard": {
                "title": "点击挑战",
                "metricValue": {"var": "score"},
                "visualStyle": "pixel",
                "visualPalette": "candy",
                "visualLayout": "arcade",
                "visualSprite": {
                    "switch_state": {
                        "ready": "target",
                        "playing": "mole-center",
                        "result": "trophy",
                    },
                },
            },
        }
        runtime_mod.validate_widget(spec)
        with tempfile.TemporaryDirectory() as tmp:
            widget = runtime_mod.WidgetRuntime(pathlib.Path(tmp))
            widget.spec = spec
            widget.state = "playing"
            widget.vars = {"score": 7}
            payload = widget.render_payload()
            self.assertIn("visualStyle=pixel", payload)
            self.assertIn("visualPalette=candy", payload)
            self.assertIn("visualSprite=mole-center", payload)

        invalid = {**spec, "dashboard": {**spec["dashboard"], "visualPalette": "user-css"}}
        with self.assertRaises(runtime_mod.ValidationError):
            runtime_mod.validate_widget(invalid)


if __name__ == "__main__":
    unittest.main()
