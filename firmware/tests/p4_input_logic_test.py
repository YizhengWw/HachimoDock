"""Compile and run the heap-free P4 button/rotary/joystick decoders on the host.

Input: the platform-independent decoder C source and its assertion executable.
Output: regression checks for startup calibration and four-direction joystick decoding.
Position: pytest wrapper for P4 physical-input logic.
Sync: update with pet_p4_input_core.c/.h and p4_input_logic_test.c.
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def test_invalid_startup_calibration_disarms_only_joystick():
    source = (ROOT / "main" / "pet_p4_input.c").read_text()
    assert "const bool joystick_calibrated = calibrate_joystick_center(" in source
    assert "atomic_store_explicit(&g_joystick_ready, joystick_calibrated," in source
    assert "joystick_calibrated ? pet_p4_joystick_decoder_update(" in source
    assert ") : PET_P4_JOYSTICK_CENTER;" in source
    assert "PET_P4_INPUT_JOYSTICK_CENTER_DEFAULT" not in source


def test_p4_input_decoders_cover_four_direction_joystick():
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "p4-input-test"
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "main"),
                str(ROOT / "main" / "pet_p4_input_core.c"),
                str(ROOT / "tests" / "p4_input_logic_test.c"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
