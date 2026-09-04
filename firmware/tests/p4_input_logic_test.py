"""Compile and run the heap-free P4 button/rotary/joystick decoders on the host.

Input: the platform-independent decoder C source and its assertion executable.
Output: a host-compiler regression check for adaptive four-direction joystick decoding.
Position: pytest wrapper for P4 physical-input logic.
Sync: update with pet_p4_input_core.c/.h and p4_input_logic_test.c.
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


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
