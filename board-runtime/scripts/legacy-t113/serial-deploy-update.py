#!/usr/bin/env python3
"""Deploy the current board-runtime update over the serial console.

This intentionally sends files one-by-one.  The target board has little free
RAM, so a single tarball decode can be killed by OOM.
"""
import base64
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REMOTE_ROOT = os.environ.get("REMOTE_ROOT", "/mnt/UDISK/board-runtime")
SERIAL_EXEC = ROOT / "scripts" / "serial-exec.py"


def run_serial(command: str, timeout: int = 60) -> str:
    env = os.environ.copy()
    env["SERIAL_TIMEOUT"] = str(timeout)
    result = subprocess.run(
        [sys.executable, str(SERIAL_EXEC), command],
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    sys.stdout.write(result.stdout)
    sys.stdout.flush()
    if result.returncode != 0:
        raise RuntimeError(f"serial command failed: {command}")
    return result.stdout


def send_file(local: Path, remote: str, executable: bool = False) -> None:
    data = local.read_bytes()
    digest = hashlib.md5(data).hexdigest()
    encoded = base64.b64encode(data).decode("ascii")
    with tempfile.NamedTemporaryFile("w", delete=False, prefix="serial-file-", suffix=".b64") as fh:
        temp_name = fh.name
        for index in range(0, len(encoded), 76):
            fh.write(encoded[index:index + 76] + "\n")
    try:
        print(f"[deploy] send {local} -> {remote} ({len(data)} bytes md5={digest})", flush=True)
        env = os.environ.copy()
        env["SERIAL_TIMEOUT"] = "300"
        env["SERIAL_SEND_CHUNK"] = "1024"
        subprocess.run(
            [sys.executable, str(SERIAL_EXEC), "--send-file", temp_name, f"{REMOTE_ROOT}/.upload.b64"],
            cwd=ROOT,
            env=env,
            check=True,
        )
    finally:
        os.unlink(temp_name)

    mode = "755" if executable else "644"
    decode = (
        "python3 -c 'import base64; "
        f"data=base64.b64decode(open(\"{REMOTE_ROOT}/.upload.b64\",\"rb\").read()); "
        f"open(\"{remote}\",\"wb\").write(data)'"
        f"; rm -f {REMOTE_ROOT}/.upload.b64"
        f"; chmod {mode} {remote}"
        f"; md5sum {remote}"
    )
    output = run_serial(decode, timeout=60)
    if digest not in output:
        output = run_serial(f"md5sum {remote}", timeout=30)
        if digest not in output:
            raise RuntimeError(f"md5 mismatch after sending {local}")


def main() -> int:
    runtime_only = "--runtime-only" in sys.argv[1:]
    runtime_files = [
        ("build-armhf/board-server", "board-server", True),
        ("build-armhf/board-touch-input", "board-touch-input", True),
        ("build-armhf/board-rotary-input", "board-rotary-input", True),
        ("build-armhf/fb-speech-overlay", "fb-speech-overlay", True),
        ("fb-display.sh", "fb-display.sh", True),
        ("board-audio-bridge.sh", "board-audio-bridge.sh", True),
        ("board-sound.sh", "board-sound.sh", True),
        ("terrier-clips-durations.tsv", "terrier-clips-durations.tsv", False),
        ("board-runtime.init", "board-runtime.init", True),
        ("usb-gadget-setup.sh", "usb-gadget-setup.sh", True),
        ("install.sh", "install.sh", True),
        ("README.md", "README.md", False),
        ("DEPLOY.md", "DEPLOY.md", False),
    ]
    terrier_dir = ROOT.parent / "terrier-clips"
    if not runtime_only and not terrier_dir.is_dir():
        raise RuntimeError(f"missing {terrier_dir}")

    clean_command = (
        f"rm -rf {REMOTE_ROOT}/assets/pets {REMOTE_ROOT}/.upload.b64; "
        if runtime_only
        else f"rm -rf {REMOTE_ROOT}/assets/pets {REMOTE_ROOT}/terrier-clips {REMOTE_ROOT}/.upload.b64; "
    )
    prepare_command = (
        "echo PREPARE; "
        "/etc/init.d/board-runtime stop 2>/dev/null || true; "
        "killall fb-display.sh board-server board-touch-input board-rotary-input fb-speech-overlay tplayerdemo 2>/dev/null || true; "
        + clean_command +
        f"mkdir -p {REMOTE_ROOT}/terrier-clips; "
        "df -h /mnt/UDISK"
    )
    run_serial(
        prepare_command,
        timeout=60,
    )

    for local_rel, remote_name, executable in runtime_files:
        send_file(ROOT / local_rel, f"{REMOTE_ROOT}/{remote_name}", executable)

    if not runtime_only:
        for local in sorted(terrier_dir.glob("*.mp4")):
            send_file(local, f"{REMOTE_ROOT}/terrier-clips/{local.name}", False)

    run_serial(
        f"cp {REMOTE_ROOT}/board-runtime.init /etc/init.d/board-runtime; "
        "chmod +x /etc/init.d/board-runtime; "
        "/etc/init.d/board-runtime enable 2>/dev/null || true; "
        "/etc/init.d/board-runtime start; "
        "sleep 2; "
        "echo VIDEO_COUNT; "
        f"ls {REMOTE_ROOT}/terrier-clips/*.mp4 | wc -l; "
        "echo PROCS; "
        "ps w | grep -E 'board|fb-' | grep -v grep || true; "
        "echo DISK; df -h /mnt/UDISK",
        timeout=90,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
