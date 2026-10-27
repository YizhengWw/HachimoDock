"""Fault-inject production AFE lifecycle; DSP stubs do not prove acoustic quality."""
from pathlib import Path
import subprocess
import tempfile
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('revision', [100, 300])
def test_afe_fault_recovery_runtime(revision):
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "afe-lifecycle-test"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        f"-DCONFIG_ESP32P4_REV_MIN_FULL={revision}",
                        "-I", str(ROOT / "tests/stubs/audio_frontend"),
                        str(ROOT / "tests/p4_audio_frontend_runtime_test.c"),
                        "-lm", "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        # Each init allocation site (including partial DSP/buffer allocation).
        for allocation in range(10):
            subprocess.run([str(exe), str(allocation)], check=True)
        for chunk in (256, 512):
            subprocess.run([str(exe), "cadence", str(chunk)], check=True)
