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
            str(ROOT / "main/pet_p4_conversation.c"),
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


def test_conversation_captions_follow_audio_and_isolate_agent_state():
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "p4-conversation-test"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "main"),
            str(ROOT / "main/pet_p4_view.c"),
            str(ROOT / "main/pet_p4_conversation.c"),
            str(ROOT / "tests/p4_conversation_test.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_realtime_renderer_and_input_never_use_agent_queue():
    renderer = (ROOT / "main/pet_p4_renderer.c").read_text()
    assert "show_session_queue = !realtime && state && state->session_queue_count > 0" in renderer
    assert "if (!realtime) draw_session_queue(state, now_ms);" in renderer
    assert "realtime ? rgb565(42, 119, 225)" in renderer
    inputs = (ROOT / "main/pet_p4_input.c").read_text()
    dispatch = inputs.split("static void dispatch_binding_event(", 1)[1]
    assert dispatch.index("pet_p4_conversation_active(state)") < dispatch.index("active_global_exit_binding(state, event_name)")
    touch = (ROOT / "main/pet_p4_touch.c").read_text()
    assert "pet_p4_conversation_move(state" in touch
    main = (ROOT / "main/pet_p4_main.c").read_text()
    view_log = main.split('"view page=', 1)[1].split("last_logged_update =", 1)[0]
    assert "view.title" not in view_log
    assert "view.body" not in view_log


def test_recording_row_is_shared_without_duplicate_idle_bubble():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    assert "if (view && view->show_voice_overlay) draw_standalone_voice_overlay();" in source
    assert "else if (!show_session_queue) draw_bubble(view, now_ms);" in source
    assert "draw_voice_input_row(x, y + 18)" in source
    assert "draw_voice_input_row(x, y + 62)" in source
    assert source.count('draw_text_line_vcenter("LISTENING"') == 1
