"""Atomic reservation and production audio control checks, with no hardware IO."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
         "-pthread", "-fsanitize=address,undefined"]


def compile_and_run(tmp_path, test, *extra):
    executable = tmp_path / test
    subprocess.run(["cc", *FLAGS, "-I", str(tmp_path), "-I", str(ROOT / "main"),
                    "-I", str(ROOT / "tests/program_probe_stubs"),
                    str(ROOT / "main/pet_p4_resources.c"), str(ROOT / "main/pet_p4_resources_device.c"),
                    str(ROOT / f"tests/{test}.c"), *extra, "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=20)


def test_real_broker_atomic_priority_reserved_capacity_and_physical_completion(tmp_path):
    compile_and_run(tmp_path, "p4_realtime_resources_test")


def test_actual_audio_control_overlap_aba_and_write_stop_completion(tmp_path):
    source = (ROOT / "main/pet_p4_audio.c").read_text()
    sections = {
        "production_audio_resources.inc": ("/* Pending requests and physical workers", "static atomic_uint g_capture_tx_last_us"),
        "production_stream_buffered.inc": ("static size_t stream_buffered(", "static bool playback_gates_capture("),
        "production_stream_drain.inc": ("static void stream_silence_step(", "static atomic_bool g_instrument_enabled"),
        "production_audio_control.inc": ("esp_err_t pet_p4_audio_set_enabled(", None),
        "production_capture_task.inc": ("static void audio_task(", "esp_err_t pet_p4_audio_init("),
        "production_audio_begin.inc": ("static void send_audio_begin(", "static bool send_audio_chunk("),
    }
    for name, (start, end) in sections.items():
        section = source[source.index(start):]
        if end:
            section = section[:section.index(end)]
        (tmp_path / name).write_text(section)
    # Exercise the actual begin serializer, not a stub that hides owner races.
    cjson = ROOT / "tests/vendor/cjson"
    compile_and_run(tmp_path, "p4_audio_realtime_test", "-I", str(cjson), str(cjson / "cJSON.c"))


def test_physical_capture_file_cues_and_fx_are_scoped_without_dsp_or_priority_changes():
    source = (ROOT / "main/pet_p4_audio.c").read_text()
    capture = source.split("static void audio_task(", 1)[1].split("esp_err_t pet_p4_audio_init(", 1)[0]
    assert capture.index("PET_AUDIO_REALTIME_SCOPE(capture_work)") < capture.index("capture_worker_take(&capture_work,") < capture.index("esp_codec_dev_read(")
    assert capture.index("capture_worker_done()") > capture.index("send_audio_end(")
    playback = source.split("static void playback_task(", 1)[1].split("static void audio_task(", 1)[0]
    assert playback.index("PET_AUDIO_REALTIME_SCOPE(file_playback_work)") < playback.index("fopen(") < playback.index("esp_codec_dev_write(")
    for function, scope in (("fruit_drain_step", "fruit_work"), ("instrument_drain_step", "instrument_work")):
        block = source.split(f"static bool {function}(", 1)[1].split("\n}\n", 1)[0]
        assert block.index(f"pet_resources_device_realtime_begin(&{scope})") < block.index("esp_codec_dev_write(")
    assert 'audio_task, "pet_p4_audio", 8192, NULL, 18, &g_audio_task, 0' in source
    assert 'playback_task, "pet_p4_playback", 8192, NULL, 18, &g_playback_task, 0' in source
    assert "PET_P4_AUDIO_STREAM_WRITE_BYTES 640" in source
