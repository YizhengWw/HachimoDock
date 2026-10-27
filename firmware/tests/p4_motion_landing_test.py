"""Regression for visible foot contact and exact time-of-impact landings."""
from pathlib import Path
import subprocess


def test_swept_foot_contact(tmp_path):
    source = Path(__file__).with_suffix(".c")
    binary = tmp_path / "landing"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", str(source), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
