"""PC validator must reject exactly what firmware parse_dashboard/parse_value rejects.
Mirrors pet_p4_miniapp.c: leaves are literal/var/fmt_mmss/fmt_hms, switch branches are
leaves (no nesting), progress is pct_of/of_max/label; everything else fails on-device with
"dashboard value rule is unsupported". No device, no user records."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc/skills/petui/scripts"))
from validate_generated_widget import evaluate_dashboard_value, validate_dashboard  # noqa: E402

BASE = {
    "title": "T", "eyebrow": "E", "headline": "H", "metricLabel": "L", "metricValue": "1",
    "metricUnit": "", "badge": "", "note": "", "footer": "F", "visualStyle": "clean",
    "visualPalette": "ocean", "visualLayout": "tool", "visualSprite": "gauge",
}
VARS = {"n": {"type": "int", "init": 3}, "s": {"type": "string", "init": "x"}}
STATES = ["idle", "run"]
PAGES = ["home", "garden"]


def runtime_errors(**slots):
    errors = []
    validate_dashboard(dict(BASE, **slots), "runtime/widget.json.dashboard", "tool",
                       runtime_dashboard=True, errors=errors, variables=VARS,
                       state_ids=STATES, page_ids=PAGES)
    return errors


def test_device_grammar_is_accepted():
    assert runtime_errors(metricValue={"var": "n"}) == []
    assert runtime_errors(metricValue={"fmt_mmss": "n"}, note={"fmt_hms": "n"}) == []
    assert runtime_errors(headline={"switch_page": {"home": "A", "garden": {"var": "n"}}}) == []
    assert runtime_errors(metricValue={"switch_state": {"idle": {"fmt_mmss": "n"}, "run": "B"}}) == []
    assert runtime_errors(progress={"pct_of": "n", "of_max": "n", "label": "p"}) == []
    assert runtime_errors(progress="25%") == []


def test_shapes_rejected_by_firmware_are_rejected_here():
    nested = runtime_errors(headline={"switch_page": {"home": {"switch_state": {"idle": "A"}}}})
    assert any("嵌套" in e and "dashboard value rule is unsupported" in e for e in nested)
    assert any("dashboard value rule is unsupported" in e for e in runtime_errors(metricValue={"text": "x"}))
    assert any("dashboard value rule is unsupported" in e for e in runtime_errors(note={}))
    assert any("dashboard value rule is unsupported" in e for e in runtime_errors(badge={"var": "n", "suffix": "个"}))
    assert any("dashboard value rule is unsupported" in e for e in runtime_errors(progress={"value": 0, "label": "p"}))
    assert any("只能包含 pct_of" in e for e in runtime_errors(progress={"pct_of": "n", "of_max": "n", "extra": 1}))


def test_references_and_types_follow_firmware():
    assert any("未知变量" in e for e in runtime_errors(metricValue={"var": "missing"}))
    assert any("需要 int 变量" in e for e in runtime_errors(metricValue={"fmt_mmss": "s"}))
    assert any("需要 int 变量" in e for e in runtime_errors(progress={"pct_of": "s", "of_max": "n"}))
    assert any("未知 state" in e for e in runtime_errors(note={"switch_state": {"nope": "A"}}))
    assert any("未知 page" in e for e in runtime_errors(note={"switch_page": {"nope": "A"}}))
    too_many = {f"s{i}": "x" for i in range(7)}
    assert any("最多 6 个分支" in e for e in runtime_errors(note={"switch_state": too_many}))


def test_initial_preview_does_not_resolve_nested_switch():
    variables = {"n": 3}
    assert evaluate_dashboard_value({"switch_page": {"home": {"var": "n"}}}, state="idle", page="home", variables=variables) == 3
    assert evaluate_dashboard_value({"switch_page": {"home": {"switch_state": {"idle": "A"}}}}, state="idle", page="home", variables=variables) is None
    assert evaluate_dashboard_value({"text": "x"}, state="idle", page="home", variables=variables) is None
    assert evaluate_dashboard_value({"fmt_mmss": "n"}, state="idle", page="home", variables=variables) == "00:03"


def test_negative_screen_preview_keeps_static_progress():
    errors = []
    validate_dashboard(dict(BASE, progress={"value": 40, "label": "p"}), "negative-screen.json.dashboard",
                       "tool", runtime_dashboard=False, errors=errors)
    assert errors == []
