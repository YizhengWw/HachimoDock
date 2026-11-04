"""Production native diagnostic probe with host-only platform stubs; no hardware."""
from pathlib import Path
import json
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
STUBS = ROOT / "tests/program_probe_stubs"


def build(tmp, word_stack=False):
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(STUBS), "-I", str(ROOT / "main")]
    if word_stack:
        flags += ["-DPROBE_STUB_WORD_STACK"]
    core = tmp / "core.o"
    subprocess.run(["cc", *flags, "-Dpet_program_run=probe_real_run",
                    "-Dpet_program_begin=probe_real_begin", "-Dpet_program_step=probe_real_step", "-c",
                    "-Dpet_program_commit=probe_real_commit",
                    str(ROOT / "main/pet_p4_program_core.c"), "-o", str(core)], check=True)
    exe = tmp / "probe"
    subprocess.run(["cc", *flags, str(ROOT / "main/pet_p4_resources.c"),
                    str(ROOT / "main/pet_p4_resources_device.c"),
                    str(ROOT / "main/pet_p4_program_probe.c"),
                    str(ROOT / "tests/p4_program_probe_test.c"), str(core), "-o", str(exe)], check=True)
    return exe


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    return build(tmp_path_factory.mktemp("program-probe"))


def invoke(probe, *arguments):
    result = subprocess.run([str(probe), *arguments], check=True, text=True, capture_output=True)
    envelopes = [json.loads(line) for line in result.stdout.splitlines()]
    assert all(set(item) == {"topic", "payload"} for item in envelopes)
    return [{"topic": item["topic"], **item["payload"]} for item in envelopes]


def test_three_fixed_cases_real_broker_and_explicit_reentry(probe):
    success, throttled, reentered = invoke(probe, "success")
    assert success["ok"] and reentered["ok"]
    assert success["topic"] == "debug/program_probe_result"
    assert success["cancelBeforeCommitRejected"] and success["staleOwnerRejected"]
    assert success["stateBytes"] == 432 and success["workspaceBytes"] == 552
    assert success["taskStackMinBytes"] == 2048
    assert success["totalWallUs"] >= 540000
    assert success["quantumFuel"] == 64 and success["quantumMaxUs"] == 690
    assert success["quanta"] == 1052 and success['guardBudgetUs']==750
    assert [c["kind"] for c in success["cases"]] == ["filter_map_reduce", "backedge_4096", "validate_256", "backedge_4096_deadline"]
    assert [c["usedFuel"] for c in success["cases"]] == [89, 4096, 0, 4096]
    assert [c["result"] for c in success["cases"]] == ["committed", "fuel_exhausted", "valid", "fuel_exhausted"]
    for case, slices, active in zip(success["cases"], [2, 64, 1], [80, 1930, 40]):
        assert case["samples"] == 8 and case["minUs"] == case["maxUs"] == active
        assert case["sumUs"] == 8 * active
        assert case["quanta"] == 8 * slices and case["quantumMaxUs"] == 40
        assert case["wallMinUs"] == case["wallMaxUs"] >= 1000 * slices
        assert case["wallSumUs"] == case["wallMinUs"] * 8
    assert throttled["error"] == "rate_limited" and not throttled["ok"]
    guarded=success['cases'][3];assert guarded['samples']==8 and guarded['quanta']==512 and guarded['quantumMaxUs']==690
    assert_quantum_consistency(success)


def assert_quantum_consistency(response):
    quantum = response["slowestQuantum"]
    if not response["quanta"]:
        assert quantum is None
        return
    assert quantum["wallUs"] == response["quantumMaxUs"]
    assert quantum['endedUs']-quantum['startedUs']==quantum['wallUs']
    assert sum(quantum[field] for field in ["beginUs", "stepUs", "commitUs", "freshCurrentUs", "stackScanUs"]) == quantum["wallUs"]
    assert quantum["sample"] >= 1 and quantum["slice"] >= 1


def test_slowest_quantum_is_one_actual_slice_not_independent_phase_maxima(probe):
    response, throttle = invoke(probe, "phase_peaks")
    assert response["ok"] and throttle["error"] == "rate_limited"
    assert_quantum_consistency(response)
    assert {k:v for k,v in response["slowestQuantum"].items() if k not in ('startedUs','endedUs')} == {
        "kind": "filter_map_reduce", "sample": 1, "slice": 1,
        "beginUs": 10, "stepUs": 1010, "commitUs": 0, "freshCurrentUs": 10,
        "stackScanUs": 10, "wallUs": 1040,
    }  # A different slice has 910us stack scan, which must NOT be mixed here.


@pytest.mark.parametrize("phase", range(5))
@pytest.mark.parametrize("mode", ["negative_phase", "backwards_phase"])
def test_each_invalid_phase_clock_aborts_without_more_work_or_losing_commit(probe, phase, mode):
    response, throttle = invoke(probe, mode, str(phase))
    assert not response["ok"] and response["error"] == "clock_error"
    assert throttle["error"] == "rate_limited"
    assert response["cases"][0]["samples"] == 0
    if phase == 2:
        assert response["cases"][0]["result"] == "committed"
        assert response["cases"][0]["usedFuel"] == 89
    assert_quantum_consistency(response)


def test_quantum_json_preserves_large_unsigned_values_within_reply_bound(probe):
    response, = invoke(probe, "json_boundary")
    assert response["ok"]
    assert_quantum_consistency(response)
    assert response["slowestQuantum"]["stepUs"] == 2**32 - 1 + 10
    assert response["quantumMaxUs"] == 2**32 - 1 + 40


@pytest.mark.parametrize("flag", range(4))
@pytest.mark.parametrize("mode", ["busy", "mid_audio", "between_slices"])
def test_every_fresh_audio_flag_blocks_or_cancels_without_releasing_active_work(probe, flag, mode):
    results = invoke(probe, mode, str(flag))
    assert results[0]["error"] == "audio_busy" and not results[0]["ok"]
    assert results[1]["error"] == "rate_limited"
    assert all(c["samples"] == 0 for c in results[0]["cases"])
    if mode == "mid_audio":
        assert results[2]["ok"]  # Only a later explicit request can run again.


@pytest.mark.parametrize("mode", ["concurrent", "send_reentry"])
def test_concurrent_and_callback_reentry_do_not_overwrite_or_broadcast(probe, mode):
    success, busy, throttle = invoke(probe, mode)
    assert success["ok"] and busy["error"] == "busy" and throttle["error"] == "rate_limited"


def test_real_broker_capacity_is_not_stolen_or_released(probe):
    response, = invoke(probe, "resource_busy")
    assert response["error"] == "resource_busy"


def test_cancel_between_ready_and_commit_preserves_source(probe):
    cancelled, throttle = invoke(probe, "before_commit")
    assert cancelled["error"] == "audio_busy" and throttle["error"] == "rate_limited"
    assert cancelled["cases"][0]["samples"] == 0


@pytest.mark.parametrize("flag", range(4))
@pytest.mark.parametrize("mode", ["after_commit", "after_commit_yield"])
def test_audio_after_commit_retains_real_outcome_without_retry_or_other_owner_mutation(probe, flag, mode):
    cancelled, throttle = invoke(probe, mode, str(flag))
    assert not cancelled["ok"] and cancelled["error"] == "audio_busy"
    assert throttle["error"] == "rate_limited"
    first, *remaining = cancelled["cases"]
    # No fully verified measurement is added, but committed work cannot become
    # not_run or be retried merely because the enclosing diagnostic was aborted.
    assert first["result"] == "committed" and first["usedFuel"] == 89
    assert first["samples"] == 0 and first["quanta"] == 2
    assert all(c["samples"] == 0 and c["result"] == "not_run" and c["quanta"] == 0 for c in remaining)


def test_clock_failure_releases_and_null_callback_does_no_work(probe):
    failure, throttle = invoke(probe, "clock")
    assert failure["error"] == "clock_error" and throttle["error"] == "rate_limited"
    assert invoke(probe, "null") == []


def test_stack_watermark_units_are_bytes_not_assumed_32bit_words(tmp_path):
    word_probe = build(tmp_path, word_stack=True)
    success, _, _ = invoke(word_probe, "success")
    assert success["taskStackMinBytes"] == 2048 * 4


def test_protocol_entry_exact_payload_caller_route_and_unlocked_readonly_gates(tmp_path):
    main = (ROOT / "main/pet_p4_main.c").read_text()
    from p4_render_trace_test import function
    (tmp_path / "program_probe_entry.inc").write_text(function(main,"maybe_handle_program_probe_line"))
    cjson = ROOT / "managed_components/espressif__cjson/cJSON"
    exe = tmp_path / "probe-entry"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-deprecated-declarations",
                    "-fsanitize=address,undefined", "-I", str(tmp_path), "-I", str(ROOT / "main"),
                    "-I", str(cjson), str(ROOT / "tests/p4_program_probe_entry_test.c"),
                    str(cjson / "cJSON.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
