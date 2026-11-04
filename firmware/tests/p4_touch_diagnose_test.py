"""Offline touch bring-up classifier: every init stage of pet_p4_touch_init maps to one category."""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from touch_diagnose import classify, find_touch_health  # noqa: E402

OK, TO, NF = "ESP_OK", "ESP_ERR_TIMEOUT", "ESP_ERR_NOT_FOUND"


def health(**fields):
    base = {"stage": "ready", "controller": "unidentified", "chipId": 0, "vendorId": 0, "error": OK, "address": 0,
            "primaryProbe": NF, "backupProbe": NF, "probe0x15": "ESP_ERR_INVALID_STATE", "probe0x38": "ESP_ERR_INVALID_STATE",
            "probe0x10": "ESP_ERR_INVALID_STATE", "probe0x18": "ESP_ERR_INVALID_STATE", "samples": 0, "contacts": 0,
            "pollErrors": 0, "consecutiveErrors": 0}
    return {**base, **fields}


def test_stages_map_to_the_firmware_init_sequence():
    source = (ROOT / "main/pet_p4_touch.c").read_text()
    for stage in ("i2c_init", "probe", "identify_register_touch", "panel_io", "controller", "queue", "task", "ready"):
        assert f'g_init_stage = "{stage}"' in source, stage
    assert classify(health(stage="not_started"))["category"] == "display"
    assert classify(health(stage="i2c_init", error="ESP_FAIL"))["category"] == "bus"
    assert classify(health(stage="probe", primaryProbe=TO, backupProbe=NF, error=TO))["category"] == "bus"
    r134_like = classify(health(stage="probe", error=NF, probe0x15=NF, probe0x38=NF, probe0x10=OK, probe0x18=OK))
    assert r134_like["category"] == "hardware" and "0x10/0x18" in r134_like["verdict"]
    dead_bus = classify(health(stage="probe", error=NF, probe0x15=NF, probe0x38=NF, probe0x10=NF, probe0x18=NF))
    assert dead_bus["category"] == "hardware" and "整条总线" in dead_bus["verdict"]
    unknown_chip = classify(health(stage="identify_register_touch", error="ESP_ERR_NOT_SUPPORTED", address=0x38, chipId=0x5a))
    assert unknown_chip["category"] == "driver" and "0x5a" in unknown_chip["verdict"]
    assert classify(health(stage="controller", error="ESP_FAIL"))["category"] == "driver"
    assert classify(health(stage="task", error="ESP_ERR_NO_MEM"))["category"] == "memory"
    assert classify(health(stage="ready", samples=0))["category"] == "runtime"
    assert classify(health(stage="ready", samples=100, pollErrors=40, consecutiveErrors=5))["category"] == "runtime"
    assert classify(health(stage="ready", controller="CST826", samples=500))["category"] == "healthy"
    assert classify(health(stage="ready", controller="GT911", samples=500, contacts=12))["category"] == "healthy"
    assert classify(health(stage="later"))["category"] == "unknown"


def test_cli_reads_a_status_line_and_prints_json(tmp_path):
    line = json.dumps({"topic": "diagnostics/status", "payload": {"runtime": {"touchReady": False, "touchHealth": health(
        stage="probe", error=NF, probe0x15=NF, probe0x38=NF, probe0x10=OK, probe0x18=OK)}}})
    capture = tmp_path / "status.jsonl"
    capture.write_text("garbage line\n" + line + "\n")
    out = subprocess.run([sys.executable, "-B", str(ROOT / "tools/touch_diagnose.py"), str(capture), "--json"], capture_output=True, text=True, check=True)
    assert json.loads(out.stdout)["category"] == "hardware"
    assert find_touch_health({"a": [{"b": {"touchHealth": {"stage": "ready"}}}]}) == {"stage": "ready"}
    missing = subprocess.run([sys.executable, "-B", str(ROOT / "tools/touch_diagnose.py"), "-"], input="{}", capture_output=True, text=True)
    assert missing.returncode == 2
