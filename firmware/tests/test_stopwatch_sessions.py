import pathlib
import shutil
import subprocess
import sys


def test_stopwatch_session_contract(tmp_path):
    root = pathlib.Path(__file__).resolve().parents[1]
    compiler = shutil.which("clang") or shutil.which("cc")
    assert compiler, "A C compiler is required"
    binary = tmp_path / "stopwatch-sessions"
    # Apple's bundled ASan can deadlock during dyld initialization on recent
    # macOS; Linux CI runs both sanitizers, macOS keeps UBSan enabled.
    sanitizers = "undefined" if sys.platform == "darwin" else "address,undefined"
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
        "-fsanitize=" + sanitizers, "-g",
        "-DCJSON_NESTING_LIMIT=32",
        "-I" + str(root / "main"), "-I" + str(root / "tests/vendor/cjson"),
        str(root / "main/pet_device_sessions.c"),
        str(root / "tests/vendor/cjson/cJSON.c"),
        str(root / "tests/stopwatch_sessions_test.c"), "-lm", "-o", str(binary),
    ], check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=10)
