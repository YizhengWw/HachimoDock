"""Real production cache/worker functions with pthread cancellation and broker.

No device I/O: raw storage, FreeRTOS notifications, and audio observation are
bounded fakes. The state machine, scanner, and physical resource accounting are
compiled unchanged from production with ASan/UBSan.
"""
from pathlib import Path
import subprocess
import tempfile

from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def test_production_preparer_concurrency_failures_and_generation_switches():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    begin = source.index("typedef struct {\n  uint8_t *bytes;\n  size_t size;")
    end = source.index("typedef struct {\n  uint32_t samples;", begin)
    declarations = source[begin:end]
    begin = source.index("static bool asset_prepare_audio_busy(void)")
    end = source.index("static bool decode_jpeg_frame_hardware_into(", begin)
    production = source[begin:end] + "\n" + function(source, "h264_stream_has_single_slice_access_units")
    harness = (ROOT / "tests/p4_asset_prepare_test.c").read_text()
    harness = harness.replace("/* PRODUCTION_DECLARATIONS */", declarations)
    harness = harness.replace("/* PRODUCTION_FUNCTIONS */", production)
    with tempfile.TemporaryDirectory() as temp:
        src, binary = Path(temp) / "prepare.c", Path(temp) / "prepare"
        src.write_text(harness)
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
            str(src), str(ROOT / "main/pet_p4_resources.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)


def test_render_lane_never_waits_or_reads_pending_assets():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    load = function(source, "load_asset_stream_cache")
    for forbidden in ("fopen(", "fread(", "heap_caps_malloc(", "vTaskDelay(", "ulTaskNotifyTake(", "read_range("):
        assert forbidden not in load
    assert "xTaskNotifyGive" in load
    render = function(source, "render_asset_pet_frame")
    assert render.index("if (g_asset_cache_pending) return false;") < render.index("note_asset_decode_failure();")
    h264 = function(source, "render_h264_asset_frame")
    assert "h264_stream_has_single_slice_access_units" not in h264
    assert "stream->single_slice_valid" in h264
    cancel = function(source, "pet_p4_renderer_cancel_asset_preparation")
    for forbidden in ("heap_caps_free", "device_finish", "reset_h264", "vTaskDelay"):
        assert forbidden not in cancel
    for name in ("pet_p4_renderer_render_transfer_status", "pet_p4_renderer_reset_playback"):
        assert "cancel_pending_asset_prepares" in function(source, name)
    frame = function(source, "pet_p4_renderer_render")
    assert 'renderer_cancel_inactive_prepares(page);' in frame
    inactive = function(source, 'renderer_cancel_inactive_prepares')
    assert 'if(!strcmp(page,"main"))return;' in inactive
    assert 'cancel_pending_asset_prepares();' in inactive
    assert 'release_inactive_asset_prepares();' in inactive
    assert "if (!drew_asset && !g_asset_cache_pending" in frame
    assert "draw_session_queue(state, now_ms)" in frame
    assert "draw_connection_banner(state, now_ms)" in frame
    assert "rotate_landscape_to_panel()" in frame
