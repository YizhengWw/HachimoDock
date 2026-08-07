from __future__ import annotations

import copy
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path


WORKSPACE_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR_PATH = (
    WORKSPACE_ROOT / "skills" / "petui" / "scripts" / "validate_generated_widget.py"
)
SPEC = importlib.util.spec_from_file_location("petui_validator", VALIDATOR_PATH)
assert SPEC and SPEC.loader
VALIDATOR = importlib.util.module_from_spec(SPEC)
sys.dont_write_bytecode = True
SPEC.loader.exec_module(VALIDATOR)

SMOKE_PATH = (
    WORKSPACE_ROOT / "skills" / "petui" / "scripts" / "smoke_test_widget_game.py"
)
sys.path.insert(0, str(SMOKE_PATH.parent))
from smoke_test_widget_game import run_smoke_test  # noqa: E402


def rgba_png(width: int, height: int) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
        )

    rows = b"".join(b"\0" + bytes((255, 128, 32, 255)) * width for _ in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def shooter_fixture() -> dict[str, object]:
    dashboard = {
        "title": "校验战机",
        "headline": "按SW1开始",
        "visualStyle": "clean",
        "visualPalette": "ocean",
        "visualLayout": "arcade",
        "visualSprite": "bolt",
    }
    return {
        "component.json": {
            "id": "validator-shooter",
            "name": "校验战机",
            "kind": "game",
            "version": "1.0.0",
            "description": "左右移动战机射击敌机并避免撞机。",
        },
        "negative-screen.json": {"dashboard": dashboard},
        "buttons.json": [
            {
                "action": "play.start",
                "control": "SW1",
                "event": "button.sw1.short_press",
                "label": "开始或重开",
            },
            {
                "action": "play.left",
                "control": "前方摇杆",
                "event": "knob.rotate_ccw",
                "label": "左移",
            },
            {
                "action": "play.right",
                "control": "前方摇杆",
                "event": "knob.rotate_cw",
                "label": "右移",
            },
        ],
        "runtime/widget.json": {
            "schema_version": 1,
            "engine": "p4-bounded-runtime-v3",
            "vars": {"score": {"type": "int", "init": 0}},
            "states": ["ready", "playing", "result"],
            "initial_state": "ready",
            "transitions": [
                {
                    "from": "*",
                    "on": "play.start",
                    "to": "playing",
                    "set": {"score": 0},
                },
                {"from": "playing", "on": "play.left"},
                {"from": "playing", "on": "play.right"},
            ],
            "tick": [],
            "scene": {
                "tick_ms": 140,
                "active_state": "playing",
                "result_state": "result",
                "score_var": "score",
                "grid": {"width": 8, "height": 8},
                "entities": [
                    {
                        "id": "player",
                        "x": 3,
                        "y": 6,
                        "width": 2,
                        "height": 2,
                        "tone": 4,
                        "shape": "player-ship",
                        "bounds": "clamp",
                    },
                    {
                        "id": "bullet",
                        "x": 4,
                        "y": 5,
                        "tone": 3,
                        "shape": "bullet",
                        "vy": -1,
                        "bounds": "hide",
                    },
                    {
                        "id": "enemy",
                        "x": 3,
                        "y": 0,
                        "tone": 2,
                        "shape": "enemy-ship",
                        "vy": 1,
                        "bounds": "hide",
                    },
                ],
                "rules": [
                    {"on": "play.start", "do": [{"op": "restart"}]},
                    {
                        "on": "play.left",
                        "do": [{"op": "move", "entity": "player", "dx": -1, "dy": 0}],
                    },
                    {
                        "on": "play.right",
                        "do": [{"op": "move", "entity": "player", "dx": 1, "dy": 0}],
                    },
                    {
                        "on": "edge",
                        "entity": "bullet",
                        "edge": "top",
                        "do": [
                            {
                                "op": "place",
                                "entity": "bullet",
                                "source": "player",
                                "dx": 1,
                                "dy": -1,
                            }
                        ],
                    },
                    {
                        "on": "edge",
                        "entity": "enemy",
                        "edge": "bottom",
                        "do": [
                            {"op": "score", "add": -1},
                            {"op": "place", "entity": "enemy", "x": [0, 7], "y": 0},
                        ],
                    },
                    {
                        "on": "collision",
                        "entity": "bullet",
                        "with": "enemy",
                        "do": [
                            {"op": "score", "add": 1},
                            {"op": "place", "entity": "enemy", "x": [0, 7], "y": 0},
                        ],
                    },
                    {
                        "on": "collision",
                        "entity": "player",
                        "with": "enemy",
                        "do": [{"op": "stop"}],
                    },
                ],
            },
            "dashboard": dashboard,
        },
        "share.json": {"title": "校验战机"},
    }


def write_fixture(root: Path, values: dict[str, object]) -> None:
    (root / "assets").mkdir()
    (root / "runtime").mkdir()
    for relative, value in values.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(value, bytes):
            path.write_bytes(value)
        else:
            path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


class ClaimedMechanicsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.values = shooter_fixture()

    def validate_values(self, values: dict[str, object]) -> list[str]:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, values)
            return VALIDATOR.validate_widget(root)

    def test_shooter_fixture_has_real_mechanics(self) -> None:
        self.assertEqual(self.validate_values(self.values), [])

    def test_four_direction_joystick_events_are_supported(self) -> None:
        values = copy.deepcopy(self.values)
        buttons = values["buttons.json"]
        assert isinstance(buttons, list)
        buttons.extend(
            [
                {
                    "action": "play.up",
                    "control": "前方摇杆",
                    "event": "joystick.up",
                    "label": "上移",
                },
                {
                    "action": "play.down",
                    "control": "前方摇杆",
                    "event": "joystick.down",
                    "label": "下移",
                },
            ]
        )
        runtime = values["runtime/widget.json"]
        assert isinstance(runtime, dict)
        transitions = runtime["transitions"]
        assert isinstance(transitions, list)
        transitions.extend(
            [
                {"from": "playing", "on": "play.up"},
                {"from": "playing", "on": "play.down"},
            ]
        )
        scene = runtime["scene"]
        assert isinstance(scene, dict)
        rules = scene["rules"]
        assert isinstance(rules, list)
        rules.extend(
            [
                {
                    "on": "play.up",
                    "do": [{"op": "move", "entity": "player", "dx": 0, "dy": -1}],
                },
                {
                    "on": "play.down",
                    "do": [{"op": "move", "entity": "player", "dx": 0, "dy": 1}],
                },
            ]
        )
        self.assertEqual(self.validate_values(values), [])

    def test_default_global_sw3_exit_is_reserved_from_components(self) -> None:
        values = copy.deepcopy(self.values)
        buttons = values["buttons.json"]
        assert isinstance(buttons, list)
        buttons[0]["control"] = "SW3"
        buttons[0]["event"] = "button.sw3.short_press"
        errors = self.validate_values(values)
        self.assertTrue(any("默认全局退出键" in error for error in errors), errors)

    def test_component_system_navigation_action_is_rejected(self) -> None:
        values = copy.deepcopy(self.values)
        buttons = values["buttons.json"]
        assert isinstance(buttons, list)
        buttons[0]["action"] = "page_main"
        errors = self.validate_values(values)
        self.assertTrue(any("不得定义系统导航" in error for error in errors), errors)

    def test_text_only_shooter_is_rejected(self) -> None:
        values = copy.deepcopy(self.values)
        runtime = values["runtime/widget.json"]
        assert isinstance(runtime, dict)
        runtime.pop("scene")
        errors = self.validate_values(values)
        self.assertTrue(
            any("必须声明真实 scene" in error for error in errors), errors
        )

    def test_shooter_without_projectile_collision_is_rejected(self) -> None:
        values = copy.deepcopy(self.values)
        runtime = values["runtime/widget.json"]
        assert isinstance(runtime, dict)
        scene = runtime["scene"]
        assert isinstance(scene, dict)
        rules = scene["rules"]
        assert isinstance(rules, list)
        scene["rules"] = [
            rule
            for rule in rules
            if not (
                isinstance(rule, dict)
                and rule.get("on") == "collision"
                and rule.get("entity") == "bullet"
            )
        ]
        errors = self.validate_values(values)
        self.assertTrue(
            any("子弹与敌人的 collision" in error for error in errors), errors
        )

    def test_unsupported_var_field_is_rejected_before_publish(self) -> None:
        values = copy.deepcopy(self.values)
        runtime = values["runtime/widget.json"]
        assert isinstance(runtime, dict)
        variables = runtime["vars"]
        assert isinstance(variables, dict)
        score = variables["score"]
        assert isinstance(score, dict)
        score["min"] = 0
        errors = self.validate_values(values)
        self.assertTrue(
            any("runtime/widget.json.vars.score" in error and "min" in error for error in errors),
            errors,
        )

    def test_unknown_scene_shape_is_rejected_before_publish(self) -> None:
        values = copy.deepcopy(self.values)
        runtime = values["runtime/widget.json"]
        assert isinstance(runtime, dict)
        scene = runtime["scene"]
        assert isinstance(scene, dict)
        entities = scene["entities"]
        assert isinstance(entities, list)
        entities[0]["shape"] = "copied-game-sprite"
        errors = self.validate_values(values)
        self.assertTrue(any("shape" in error for error in errors), errors)

    def test_v4_sprite_sheet_is_validated_with_exact_dimensions(self) -> None:
        values = copy.deepcopy(self.values)
        runtime = values["runtime/widget.json"]
        assert isinstance(runtime, dict)
        runtime["engine"] = "p4-bounded-runtime-v4"
        scene = runtime["scene"]
        assert isinstance(scene, dict)
        scene["sprites"] = [
            {
                "id": "hero",
                "asset": "assets/hero.png",
                "frame_width": 8,
                "frame_height": 8,
                "frames": 2,
                "fps": 8,
            }
        ]
        entities = scene["entities"]
        assert isinstance(entities, list)
        entities[0]["sprite"] = "hero"
        values["assets/hero.png"] = rgba_png(16, 8)
        self.assertEqual(self.validate_values(values), [])

        values["assets/hero.png"] = rgba_png(8, 8)
        errors = self.validate_values(values)
        self.assertTrue(any("PNG 尺寸应为 16x8" in error for error in errors), errors)


class GameplaySmokeTests(unittest.TestCase):
    def smoke_values(self, values: dict[str, object]) -> dict[str, object]:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, values)
            return run_smoke_test(root)

    def test_complete_shooter_loop_is_playable(self) -> None:
        values = shooter_fixture()
        runtime = values["runtime/widget.json"]
        assert isinstance(runtime, dict)
        runtime["engine"] = "p4-bounded-runtime-v4"
        result = self.smoke_values(values)
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["scoreReached"], result)
        self.assertTrue(result["resultReached"], result)
        self.assertTrue(result["restartPath"], result)

    def test_velocity_only_input_fails_immediate_feedback_gate(self) -> None:
        values = shooter_fixture()
        runtime = values["runtime/widget.json"]
        assert isinstance(runtime, dict)
        runtime["engine"] = "p4-bounded-runtime-v4"
        buttons = values["buttons.json"]
        assert isinstance(buttons, list)
        buttons.append(
            {
                "action": "play.jump",
                "control": "前方摇杆",
                "event": "joystick.up",
                "label": "跳跃",
            }
        )
        transitions = runtime["transitions"]
        assert isinstance(transitions, list)
        transitions.append({"from": "playing", "on": "play.jump"})
        scene = runtime["scene"]
        assert isinstance(scene, dict)
        rules = scene["rules"]
        assert isinstance(rules, list)
        rules.append(
            {
                "on": "play.jump",
                "do": [
                    {"op": "velocity", "entity": "player", "vx": 0, "vy": -2}
                ],
            }
        )
        result = self.smoke_values(values)
        self.assertFalse(result["ok"], result)
        self.assertTrue(
            any("即时可见反馈" in error and "play.jump" in error for error in result["errors"]),
            result,
        )

    def test_unreachable_score_rule_fails_real_play_gate(self) -> None:
        values = shooter_fixture()
        runtime = values["runtime/widget.json"]
        assert isinstance(runtime, dict)
        runtime["engine"] = "p4-bounded-runtime-v4"
        scene = runtime["scene"]
        assert isinstance(scene, dict)
        entities = scene["entities"]
        assert isinstance(entities, list)
        bullet = next(entity for entity in entities if entity["id"] == "bullet")
        enemy = next(entity for entity in entities if entity["id"] == "enemy")
        bullet["active"] = False
        enemy["active"] = False
        result = self.smoke_values(values)
        self.assertFalse(result["ok"], result)
        self.assertTrue(any("得分规则" in error for error in result["errors"]), result)


if __name__ == "__main__":
    unittest.main()
