#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import os
import json
import time
from pathlib import Path


def touch(path: Path) -> None:
    path.write_bytes(b"")


def write_fake_tool(path: Path, body: str) -> None:
    path.write_text(body)
    path.chmod(0o755)


def create_clips(root: Path) -> None:
    root.mkdir()
    for name in [
        "welcome.mp4",
        "idle.playing.mp4",
        "idle.wandering.mp4",
        "working.typing.mp4",
        "working.browsing.mp4",
        "working.thinking.mp4",
        "waiting_user.mp4",
        "done.mp4",
        "error.mp4",
        "touch.lick.mp4",
        "touch.what.mp4",
        "unknown.wave.mp4",
    ]:
        touch(root / name)
    for name in [
        "done.wav",
        "error.wav",
    ]:
        touch(root / name)


def create_single_working_clips(root: Path) -> None:
    root.mkdir()
    for name in [
        "idle.playing.mp4",
        "working.mp4",
    ]:
        touch(root / name)


def run_persistent_player_regression(script: Path, base_env: dict[str, str]) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        clips = tmp_path / "terrier-clips"
        runtime = tmp_path / "runtime"
        tools = tmp_path / "tools"
        log_path = tmp_path / "tplayer.log"
        create_clips(clips)
        runtime.mkdir()
        tools.mkdir()
        (runtime / ".current-state").write_text("working\n")
        (runtime / ".current-event").write_text("UserPromptSubmit\n")
        write_fake_tool(
            tools / "hexdump",
            "#!/bin/sh\nprintf '1'\n",
        )
        write_fake_tool(
            tools / "tplayerdemo",
            f"""#!/bin/sh
echo "start args=$*" >> {log_path}
if [ "$#" -gt 0 ]; then
  sleep 30
  exit 0
fi
while IFS= read -r line; do
  echo "$line" >> {log_path}
done
""",
        )
        env = base_env.copy()
        env.update(
            {
                "PATH": f"{tools}{os.pathsep}{env.get('PATH', '')}",
                "PET_CLAW_RUNTIME_ROOT": str(runtime),
                "PET_CLAW_CLIPS_DIR": str(clips),
                "PET_CLAW_FB_DEV": "/dev/null",
                "PET_CLAW_FB_DISABLE_CACHE": "1",
                "PET_CLAW_FB_FAKE_DURATION_SECONDS": "0.2",
                "PET_CLAW_FB_WORKING_MAX_LOOPS": "2",
                "PET_CLAW_FB_DEBUG_OVERLAY": "1",
                "PET_CLAW_FB_CLIP_MAX_SECONDS": "30",
                "PET_CLAW_FB_TPLAYER_READY_SECONDS": "0",
            }
        )
        proc = subprocess.Popen(
            ["sh", str(script)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        try:
            deadline = time.time() + 6.0
            observed_loop = 0
            while time.time() < deadline:
                debug_file = runtime / ".debug-screen-state.json"
                if debug_file.exists():
                    try:
                        payload = json.loads(debug_file.read_text())
                        observed_loop = max(observed_loop, int(payload.get("loopRepeatCount", 0)))
                    except (ValueError, OSError):
                        pass
                if observed_loop >= 2:
                    break
                time.sleep(0.1)
            log_text = log_path.read_text() if log_path.exists() else ""
            if observed_loop < 2:
                status = proc.poll()
                proc.terminate()
                try:
                    stdout, stderr = proc.communicate(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    stdout, stderr = proc.communicate(timeout=2)
                return False, (
                    "loop did not advance while fake tplayer stayed alive; "
                    f"observed_loop={observed_loop}; status={status}; "
                    f"log={log_text!r}; stdout={stdout!r}; stderr={stderr!r}"
                )
            if "set loop:0" not in log_text or "play url:" not in log_text:
                return False, f"persistent tplayer commands missing; log={log_text!r}"
            return True, ""
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)


def run_state_speech_sync_regression(script: Path, base_env: dict[str, str]) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        clips = tmp_path / "terrier-clips"
        runtime = tmp_path / "runtime"
        tools = tmp_path / "tools"
        log_path = tmp_path / "tplayer.log"
        create_clips(clips)
        runtime.mkdir()
        tools.mkdir()
        (runtime / ".current-state").write_text("working\n")
        (runtime / ".current-event").write_text("UserPromptSubmit\n")
        write_fake_tool(
            tools / "hexdump",
            "#!/bin/sh\nprintf '1'\n",
        )
        write_fake_tool(
            tools / "tplayerdemo",
            f"""#!/bin/sh
echo "start args=$*" >> {log_path}
while IFS= read -r line; do
  echo "$line" >> {log_path}
done
""",
        )
        env = base_env.copy()
        env.update(
            {
                "PATH": f"{tools}{os.pathsep}{env.get('PATH', '')}",
                "PET_CLAW_RUNTIME_ROOT": str(runtime),
                "PET_CLAW_CLIPS_DIR": str(clips),
                "PET_CLAW_FB_DEV": "/dev/null",
                "PET_CLAW_FB_DISABLE_CACHE": "1",
                "PET_CLAW_FB_FAKE_DURATION_SECONDS": "1.0",
                "PET_CLAW_FB_WORKING_MAX_LOOPS": "1",
                "PET_CLAW_FB_DEBUG_OVERLAY": "1",
                "PET_CLAW_FB_CLIP_MAX_SECONDS": "30",
                "PET_CLAW_FB_TPLAYER_READY_SECONDS": "0",
            }
        )
        proc = subprocess.Popen(
            ["sh", str(script)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        try:
            deadline = time.time() + 6.0
            while time.time() < deadline:
                debug_file = runtime / ".debug-screen-state.json"
                speech_file = runtime / ".current-speech"
                if debug_file.exists() and speech_file.exists():
                    try:
                        payload = json.loads(debug_file.read_text())
                        speech = speech_file.read_text()
                    except (ValueError, OSError):
                        time.sleep(0.05)
                        continue
                    if payload.get("displayedState") == "working" and speech == "努力工作中":
                        break
                time.sleep(0.05)
            else:
                stdout, stderr = proc.communicate(timeout=0.1) if proc.poll() is not None else ("", "")
                speech = (runtime / ".current-speech").read_text() if (runtime / ".current-speech").exists() else ""
                return False, (
                    "fb-display did not write working speech on actual state entry; "
                    f"speech={speech!r}; stdout={stdout!r}; stderr={stderr!r}"
                )

            (runtime / ".current-state").write_text("done\n")
            (runtime / ".current-event").write_text("TaskComplete\n")

            deadline = time.time() + 6.0
            while time.time() < deadline:
                debug_file = runtime / ".debug-screen-state.json"
                speech_file = runtime / ".current-speech"
                if debug_file.exists() and speech_file.exists():
                    try:
                        payload = json.loads(debug_file.read_text())
                        speech = speech_file.read_text()
                    except (ValueError, OSError):
                        time.sleep(0.05)
                        continue
                    if payload.get("displayedState") == "done" and speech == "搞定啦！":
                        return True, ""
                time.sleep(0.05)
            speech = (runtime / ".current-speech").read_text() if (runtime / ".current-speech").exists() else ""
            return False, f"fb-display did not sync done speech with done clip; speech={speech!r}"
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)


def run_multi_speech_bubble_regression(script: Path, base_env: dict[str, str]) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        runtime = tmp_path / "runtime"
        runtime.mkdir()
        font = tmp_path / "font.ttf"
        font.write_text("fake-font")
        (runtime / ".current-state").write_text("working\n")
        (runtime / ".current-speech").write_text(
            "Codex 会话: 正在部署设备端\nClaude 会话: 我也在处理另一个任务\n",
            encoding="utf-8",
        )
        env = base_env.copy()
        env.update(
            {
                "PET_CLAW_FB_FONTFILE": str(font),
                "PET_CLAW_FB_STATUS_FONTFILE": str(font),
            }
        )
        result = subprocess.run(
            ["sh", str(script), "--speech-filter", str(runtime), "working", "320", "240"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
            env=env,
        )
        if result.returncode != 0:
            return False, f"speech-filter failed: stdout={result.stdout!r}; stderr={result.stderr!r}"
        filter_text = result.stdout
        if not (runtime / ".current-speech-render.0").exists() or not (runtime / ".current-speech-render.1").exists():
            return False, "multi speech render files were not created"
        if ".current-speech-render.0" not in filter_text or ".current-speech-render.1" not in filter_text:
            return False, f"multi speech drawtext files missing from filter: {filter_text!r}"
        if filter_text.count("drawtext=fontfile=") < 2:
            return False, f"expected at least two speech drawtext filters: {filter_text!r}"
        return True, ""


def run_state_audio_cue_regression(script: Path, base_env: dict[str, str]) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        clips = tmp_path / "terrier-clips"
        runtime = tmp_path / "runtime"
        tools = tmp_path / "tools"
        tplayer_log = tmp_path / "tplayer.log"
        audio_log = tmp_path / "audio.log"
        create_clips(clips)
        runtime.mkdir()
        tools.mkdir()
        (runtime / ".current-state").write_text("done\n")
        (runtime / ".current-event").write_text("TaskComplete\n")
        write_fake_tool(
            tools / "hexdump",
            "#!/bin/sh\nprintf '1'\n",
        )
        write_fake_tool(
            tools / "tplayerdemo",
            f"""#!/bin/sh
echo "start args=$*" >> {tplayer_log}
while IFS= read -r line; do
  echo "$line" >> {tplayer_log}
done
""",
        )
        write_fake_tool(
            tools / "aplay",
            f"""#!/bin/sh
echo "$*" >> {audio_log}
exit 0
""",
        )
        env = base_env.copy()
        env.update(
            {
                "PATH": f"{tools}{os.pathsep}{env.get('PATH', '')}",
                "PET_CLAW_RUNTIME_ROOT": str(runtime),
                "PET_CLAW_CLIPS_DIR": str(clips),
                "PET_CLAW_FB_DEV": "/dev/null",
                "PET_CLAW_FB_FAKE_DURATION_SECONDS": "0.4",
                "PET_CLAW_FB_DONE_MAX_LOOPS": "1",
                "PET_CLAW_FB_DEBUG_OVERLAY": "1",
                "PET_CLAW_FB_CLIP_MAX_SECONDS": "30",
                "PET_CLAW_FB_TPLAYER_READY_SECONDS": "0",
            }
        )
        proc = subprocess.Popen(
            ["sh", str(script)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        try:
            deadline = time.time() + 6.0
            while time.time() < deadline:
                if audio_log.exists() and "done.wav" in audio_log.read_text():
                    return True, ""
                time.sleep(0.05)
            stdout, stderr = proc.communicate(timeout=0.1) if proc.poll() is not None else ("", "")
            audio_text = audio_log.read_text() if audio_log.exists() else ""
            return False, (
                "fb-display did not play the done WAV cue when entering the done clip; "
                f"audio={audio_text!r}; stdout={stdout!r}; stderr={stderr!r}"
            )
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    script = repo / "fb-display.sh"
    script_text = script.read_text()
    if 'fb-rawvideo-blit.py" $FB_DEV 480 320' in script_text:
        sys.stderr.write("ffmpeg fallback still blits only the top 480x320 half of fb0\n")
        return 1
    if "FB_WIDTH" not in script_text or "FB_HEIGHT" not in script_text:
        sys.stderr.write("ffmpeg fallback does not use framebuffer geometry\n")
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        clips = Path(tmp) / "terrier-clips"
        create_clips(clips)

        result = subprocess.run(
            ["sh", str(script), "--self-test", str(clips)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )

        fake_tools = Path(tmp) / "fake-tools"
        fake_tools.mkdir()
        write_fake_tool(fake_tools / "od", "#!/bin/sh\nexit 127\n")
        write_fake_tool(fake_tools / "mktemp", "#!/bin/sh\nexit 127\n")
        write_fake_tool(fake_tools / "hexdump", "#!/bin/sh\necho 1\n")
        compat_env = os.environ.copy()
        compat_env["PATH"] = f"{fake_tools}{os.pathsep}{compat_env.get('PATH', '')}"
        compat_result = subprocess.run(
            ["sh", str(script), "--self-test", str(clips)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
            env=compat_env,
        )

        single_working_clips = Path(tmp) / "single-working-clips"
        create_single_working_clips(single_working_clips)
        single_working_result = subprocess.run(
            ["sh", str(script), "--self-test", str(single_working_clips)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )

        runtime = Path(tmp) / "runtime"
        runtime.mkdir()
        (runtime / ".current-state").write_text("working\n")
        (runtime / ".current-event").write_text("UserPromptSubmit\n")
        debug_result = subprocess.run(
            ["sh", str(script), "--debug-text", str(runtime), "working", "working.typing.mp4", "2", "5"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        duration_file = Path(tmp) / "durations.tsv"
        duration_file.write_text("working.typing.mp4\t7.500\n")
        duration_env = os.environ.copy()
        duration_env["PET_CLAW_FB_DURATION_FILE"] = str(duration_file)
        duration_result = subprocess.run(
            ["sh", str(script), "--duration", str(clips / "working.typing.mp4")],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
            env=duration_env,
        )

        persistent_ok, persistent_error = run_persistent_player_regression(script, os.environ.copy())
        speech_sync_ok, speech_sync_error = run_state_speech_sync_regression(script, os.environ.copy())
        multi_speech_ok, multi_speech_error = run_multi_speech_bubble_regression(script, os.environ.copy())
        audio_cue_ok, audio_cue_error = run_state_audio_cue_regression(script, os.environ.copy())

    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode
    if compat_result.returncode != 0:
        sys.stderr.write(compat_result.stdout)
        sys.stderr.write(compat_result.stderr)
        return compat_result.returncode
    if single_working_result.returncode != 0:
        sys.stderr.write(single_working_result.stdout)
        sys.stderr.write(single_working_result.stderr)
        return single_working_result.returncode
    if debug_result.returncode != 0:
        sys.stderr.write(debug_result.stdout)
        sys.stderr.write(debug_result.stderr)
        return debug_result.returncode
    if duration_result.returncode != 0:
        sys.stderr.write(duration_result.stdout)
        sys.stderr.write(duration_result.stderr)
        return duration_result.returncode
    if "7.420" not in duration_result.stdout:
        sys.stderr.write(f"duration file was not used; stdout={duration_result.stdout!r}")
        return 1
    if not persistent_ok:
        sys.stderr.write(persistent_error)
        sys.stderr.write("\n")
        return 1
    if not speech_sync_ok:
        sys.stderr.write(speech_sync_error)
        sys.stderr.write("\n")
        return 1
    if not audio_cue_ok:
        sys.stderr.write(audio_cue_error)
        sys.stderr.write("\n")
        return 1
    if not multi_speech_ok:
        sys.stderr.write(multi_speech_error)
        sys.stderr.write("\n")
        return 1

    expected = [
        "state idle count=2 max=5",
        "state working count=3 max=5",
        "state touch count=2 max=1",
        "canonical tool_running=working",
        "canonical notification=waiting_user",
        "canonical touch.lick=idle",
        "pick touch state=touch",
        "touch_allowed waiting_user=no",
        "touch_allowed error=no",
        "touch_allowed working=yes",
    ]
    missing = [line for line in expected if line not in result.stdout]
    if missing:
        sys.stderr.write("missing expected self-test output:\n")
        for line in missing:
            sys.stderr.write(f"  {line}\n")
        sys.stderr.write("--- stdout ---\n")
        sys.stderr.write(result.stdout)
        sys.stderr.write("--- stderr ---\n")
        sys.stderr.write(result.stderr)
        return 1
    compat_expected = [
        "state working count=3 max=5",
        "rand sample=1",
        "pick working name=working.thinking.mp4",
        "pick touch name=touch.what.mp4",
    ]
    compat_missing = [line for line in compat_expected if line not in compat_result.stdout]
    if compat_missing:
        sys.stderr.write("missing expected compatibility output:\n")
        for line in compat_missing:
            sys.stderr.write(f"  {line}\n")
        sys.stderr.write("--- stdout ---\n")
        sys.stderr.write(compat_result.stdout)
        sys.stderr.write("--- stderr ---\n")
        sys.stderr.write(compat_result.stderr)
        return 1
    single_working_expected = [
        "state working count=1 max=5",
        "pick working name=working.mp4",
    ]
    single_working_missing = [line for line in single_working_expected if line not in single_working_result.stdout]
    if single_working_missing:
        sys.stderr.write("missing expected single-working output:\n")
        for line in single_working_missing:
            sys.stderr.write(f"  {line}\n")
        sys.stderr.write("--- stdout ---\n")
        sys.stderr.write(single_working_result.stdout)
        sys.stderr.write("--- stderr ---\n")
        sys.stderr.write(single_working_result.stderr)
        return 1
    debug_expected = [
        "screen state=working clip=working.typing.mp4 loop=2/5",
        "session state=working event=UserPromptSubmit",
    ]
    debug_missing = [line for line in debug_expected if line not in debug_result.stdout]
    if debug_missing:
        sys.stderr.write("missing expected debug output:\n")
        for line in debug_missing:
            sys.stderr.write(f"  {line}\n")
        sys.stderr.write("--- stdout ---\n")
        sys.stderr.write(debug_result.stdout)
        sys.stderr.write("--- stderr ---\n")
        sys.stderr.write(debug_result.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
