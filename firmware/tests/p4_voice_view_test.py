"""Compile the real view model and guard Session-independent recording rendering."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def test_voice_view_lifecycle():
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "p4-voice-view-test"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "main"),
            str(ROOT / "main/pet_p4_view.c"),
            str(ROOT / "tests/p4_voice_view_test.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_queue_reset_does_not_end_physical_voice_hold():
    protocol = (ROOT / "main/pet_p4_protocol.c").read_text()
    reset = protocol.split("static void restore_idle_session_view(", 1)[1].split(
        "static void mark_current_session_terminal(", 1)[0]
    assert "session_voice_active = false" not in reset
    source = (ROOT / "main/pet_p4_input.c").read_text()
    clear = source.split('strcmp(binding->action, "session_clear") == 0', 1)[1].split(
        'strcmp(binding->action, "session_next")', 1)[0]
    assert "session_voice_active = false" not in clear
    assert 'strcmp(gesture, "hold_end") == 0' in source
    assert "state->session_voice_active = false" in source


def test_recording_row_is_shared_without_duplicate_idle_bubble():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    assert "if (view && view->show_voice_overlay) draw_standalone_voice_overlay();" in source
    assert "else if (!show_session_queue) draw_bubble(view, now_ms);" in source
    assert "draw_voice_input_row(x, y + 18)" in source
    assert "draw_voice_input_row(x, y + 62)" in source
    assert source.count('draw_text_line_vcenter("LISTENING"') == 1
