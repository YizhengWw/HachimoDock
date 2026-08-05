from __future__ import annotations

import copy
import importlib.util
import json
import sys
import tempfile
import unittest
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


def shooter_fixture() -> dict[str, object]:
    dashboard = {
        "title": "校验战机",
        "headline": "按摇杆中键开始",
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
                "action": "play.left",
                "control": "SW1",
                "event": "button.sw1.short_press",
                "label": "左移",
            },
            {
                "action": "play.right",
                "control": "SW2",
                "event": "button.sw2.short_press",
                "label": "右移",
            },
            {
                "action": "play.start",
                "control": "前方摇杆",
                "event": "button.encoder.short_press",
                "label": "开始或重开",
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


class ClaimedMechanicsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.values = shooter_fixture()

    def validate_values(self, values: dict[str, object]) -> list[str]:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "assets").mkdir()
            (root / "runtime").mkdir()
            for relative, value in values.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(
                    json.dumps(value, ensure_ascii=False), encoding="utf-8"
                )
            return VALIDATOR.validate_widget(root)

    def test_shooter_fixture_has_real_mechanics(self) -> None:
        self.assertEqual(self.validate_values(self.values), [])

    def test_four_direction_joystick_events_are_supported(self) -> None:
        values = copy.deepcopy(self.values)
        buttons = values["buttons.json"]
        assert isinstance(buttons, list)
        buttons[0]["control"] = "前方摇杆"
        buttons[0]["event"] = "joystick.up"
        buttons[1]["control"] = "前方摇杆"
        buttons[1]["event"] = "joystick.down"
        self.assertEqual(self.validate_values(values), [])

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


if __name__ == "__main__":
    unittest.main()
