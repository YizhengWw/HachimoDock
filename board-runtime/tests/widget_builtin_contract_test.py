#!/usr/bin/env python3
import importlib.util
import json
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILTINS = ROOT / "builtin-clawpkgs"
RUNTIME_PATH = ROOT / "board-widget-runtime.py"

spec = importlib.util.spec_from_file_location("board_widget_runtime", RUNTIME_PATH)
runtime_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime_mod)


class BuiltinWidgetContractTest(unittest.TestCase):
    def test_builtins_use_passive_negative_screen_controls(self):
        for widget_dir in BUILTINS.iterdir():
            if not widget_dir.is_dir():
                continue
            with self.subTest(widget=widget_dir.name):
                buttons = json.loads((widget_dir / "buttons.json").read_text(encoding="utf-8"))
                for binding in buttons:
                    self.assertEqual(binding["control"], "屏幕区域")
                    self.assertIn(binding["event"], {"screen.region.tap", "screen.region.long_press"})
                    self.assertNotIn("button.primary", binding["event"])
                    self.assertNotIn("knob.rotate", binding["event"])

                widget = json.loads((widget_dir / "runtime" / "widget.json").read_text(encoding="utf-8"))
                runtime_mod.validate_widget(widget)
                runtime_mod.validate_buttons(buttons, widget.get("transitions", []))

                self.assertTrue(
                    widget.get("tick") or widget.get("readers") or widget.get("initial_state") not in {"idle", "waiting"},
                    "widget must have a useful default state or automatic data source",
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


if __name__ == "__main__":
    unittest.main()
