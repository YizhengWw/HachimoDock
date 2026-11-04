"""Opt-in real TX observation with bounded asynchronous host driver fixtures."""
from pathlib import Path
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-pthread"]


def test_bounded_ring_session_budget_hash_and_worst_case_json(tmp_path):
    executable = tmp_path / "audit-core"
    subprocess.run(["cc", *FLAGS, "-I", str(ROOT / "main"),
                    str(ROOT / "main/pet_p4_transport_audit.c"),
                    str(ROOT / "tests/p4_transport_audit_test.c"), "-o", str(executable)], check=True)
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    decoded = json.loads(result.stdout)
    assert decoded["topic"] == "debug/transport_audit_result"
    assert len(decoded["payload"]["records"]) == 16
    assert decoded["payload"]["records"][0]["bodyBytes"] == 2**32 - 1


def test_production_tx_audit_faults_concurrency_and_strict_unlocked_handler(tmp_path):
    source = (ROOT / "main/pet_p4_main.c").read_text()
    queued = '#include "pet_p4_transport_audit.h"\nstatic pet_transport_audit_t g_transport_audit;' + source.split("static pet_transport_audit_t g_transport_audit;", 1)[1].split("/* Screenshot-only checked writes.", 1)[0]
    checked = '#include "pet_p4_screenshot_write.h"\nstatic pet_screenshot_tx_observation_t g_screenshot_tx;\n' + "static bool screenshot_write_all" + source.split("static bool screenshot_write_all", 1)[1].split("static bool send_screenshot_chunk(", 1)[0]
    (tmp_path / "production_transport.inc").write_text(queued + checked)
    handler = "static EXT_RAM_BSS_ATTR pet_transport_audit_snapshot_t g_transport_audit_snapshot;" + source.split("static EXT_RAM_BSS_ATTR pet_transport_audit_snapshot_t g_transport_audit_snapshot;", 1)[1].split("static bool enqueue_protocol_line(", 1)[0]
    (tmp_path / "production_audit_handler.inc").write_text(handler)
    cjson = ROOT / "tests/vendor/cjson"
    core = tmp_path / "audit.o"
    subprocess.run(["cc", *FLAGS, "-I", str(ROOT / "main"),
                    "-Dpet_transport_audit_fingerprint=real_audit_fingerprint", "-c",
                    str(ROOT / "main/pet_p4_transport_audit.c"), "-o", str(core)], check=True)
    executable = tmp_path / "audit-tx"
    subprocess.run(["cc", *FLAGS, "-I", str(ROOT / "main"), "-I", str(tmp_path),
                    "-I", str(cjson), "-Wno-error=deprecated-declarations",
                    str(ROOT / "tests/p4_transport_audit_tx_test.c"), str(core),
                    str(cjson / "cJSON.c"), "-o", str(executable)], check=True)
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=30)
    decoded = json.loads(result.stdout)
    assert decoded["payload"]["active"] is False
    assert decoded["payload"]["seen"] == 1
    record, = decoded["payload"]["records"]
    assert record["driverObserved"] and record["newlineAccepted"] == 1
    assert not record["invalidReturn"]


def test_worker_routes_audit_before_general_state_lock_and_has_no_sensitive_record_fields():
    source = (ROOT / "main/pet_p4_main.c").read_text()
    worker = source.split("static void protocol_worker_task(", 1)[1].split("static bool consume_byte(", 1)[0]
    assert worker.index("maybe_handle_transport_audit(") < worker.index("xSemaphoreTake(g_state_mutex")
    assert "static EXT_RAM_BSS_ATTR pet_transport_audit_snapshot_t g_transport_audit_snapshot;" in source
    assert "static EXT_RAM_BSS_ATTR char g_transport_audit_reply[" in source
    header = (ROOT / "main/pet_p4_transport_audit.h").read_text()
    record = header.split("uint32_t sequence", 1)[1].split("pet_transport_audit_record_t;", 1)[0]
    assert "char " not in record and "char*" not in record and "char *" not in record
    code = (ROOT / "main/pet_p4_transport_audit.c").read_text()
    for forbidden in ("malloc(", "fopen(", "esp_", "uart_", "vTaskDelay", "Semaphore"):
        assert forbidden not in code
