#!/usr/bin/env python3
"""Push-to-talk voice input service for board-runtime.

Flow:
1) board-rotary-input writes .voice-button-state with "down"/"up".
2) This daemon starts/stops arecord on down/up.
3) Audio is transcribed to text (SpeechRecognition Google Web API by default,
   or fixed text / custom command fallback).
4) The transcribed text is posted to local board-server /input/action, which
   publishes MQTT input/action for desktop bridge ingestion.
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional
from urllib import error as urlerror
from urllib import request as urlrequest


LOG = logging.getLogger("board-voice-ptt")


def env_text(name: str, fallback: str = "") -> str:
    value = os.getenv(name, "")
    value = value.strip()
    return value if value else fallback


def env_int(name: str, fallback: int, minimum: Optional[int] = None) -> int:
    raw = os.getenv(name, "").strip()
    if not raw:
        value = fallback
    else:
        try:
            value = int(raw)
        except ValueError:
            value = fallback
    if minimum is not None and value < minimum:
        return minimum
    return value


def env_float(name: str, fallback: float, minimum: Optional[float] = None) -> float:
    raw = os.getenv(name, "").strip()
    if not raw:
        value = fallback
    else:
        try:
            value = float(raw)
        except ValueError:
            value = fallback
    if minimum is not None and value < minimum:
        return minimum
    return value


class VoicePttService:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.button_state_path = Path(
            env_text("PET_VOICE_BUTTON_PATH", str(root / ".voice-button-state"))
        )
        self.wav_path = Path(
            env_text("PET_VOICE_WAV_PATH", str(root / ".voice-input.wav"))
        )
        self.inject_url = env_text("PET_VOICE_INJECT_URL", "http://127.0.0.1/input/action")
        self.inject_timeout_s = env_float("PET_VOICE_INJECT_TIMEOUT_S", 8.0, 1.0)

        self.arecord_bin = env_text("PET_VOICE_ARECORD_BIN", "arecord")
        self.arecord_device = env_text("PET_VOICE_ARECORD_DEVICE", "default")
        self.sample_rate = env_int("PET_VOICE_SAMPLE_RATE", 16000, 8000)
        self.channels = env_int("PET_VOICE_CHANNELS", 1, 1)
        self.min_record_ms = env_int("PET_VOICE_MIN_RECORD_MS", 300, 50)
        self.max_record_s = env_int("PET_VOICE_MAX_RECORD_SECONDS", 25, 3)
        self.poll_interval_s = env_float("PET_VOICE_POLL_SECONDS", 0.08, 0.02)

        self.mode = env_text("PET_VOICE_STT_MODE", "speech_recognition").lower()
        self.fixed_text = env_text("PET_VOICE_FIXED_TEXT", "")
        self.stt_language = env_text("PET_VOICE_STT_LANGUAGE", "zh-CN")
        self.stt_cmd = env_text("PET_VOICE_STT_CMD", "")

        self.action_view = env_text("PET_VOICE_INPUT_VIEW", "voice_input")
        self.action_type = env_text("PET_VOICE_INPUT_TYPE", "long_press")

        self._record_proc: Optional[subprocess.Popen] = None
        self._record_start_monotonic = 0.0
        self._last_marker_text = ""
        self._last_mtime_ns = -1

    def _read_button_event(self) -> Optional[str]:
        try:
            stat = self.button_state_path.stat()
        except FileNotFoundError:
            return None
        except OSError as exc:
            LOG.warning("stat %s failed: %s", self.button_state_path, exc)
            return None

        if stat.st_mtime_ns == self._last_mtime_ns:
            return None
        self._last_mtime_ns = stat.st_mtime_ns

        try:
            marker = self.button_state_path.read_text(encoding="utf-8", errors="ignore").strip()
        except OSError as exc:
            LOG.warning("read %s failed: %s", self.button_state_path, exc)
            return None
        if not marker or marker == self._last_marker_text:
            return None
        self._last_marker_text = marker

        parts = marker.split()
        state = parts[-1].lower() if parts else ""
        if state in ("down", "up"):
            return state
        return None

    def _prime_marker(self) -> None:
        if not self.button_state_path.exists():
            return
        try:
            self._last_marker_text = self.button_state_path.read_text(
                encoding="utf-8",
                errors="ignore",
            ).strip()
            self._last_mtime_ns = self.button_state_path.stat().st_mtime_ns
        except OSError:
            self._last_marker_text = ""
            self._last_mtime_ns = -1

    def _start_record(self) -> None:
        if self._record_proc is not None:
            return
        self.wav_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            self.wav_path.unlink()
        except FileNotFoundError:
            pass
        except OSError as exc:
            LOG.warning("remove old wav failed: %s", exc)

        cmd = [
            self.arecord_bin,
            "-q",
            "-D",
            self.arecord_device,
            "-c",
            str(self.channels),
            "-r",
            str(self.sample_rate),
            "-f",
            "S16_LE",
            "-t",
            "wav",
            str(self.wav_path),
        ]
        LOG.info("record start: %s", " ".join(cmd))
        try:
            self._record_proc = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            self._record_start_monotonic = time.monotonic()
        except OSError as exc:
            self._record_proc = None
            LOG.error("failed to start arecord: %s", exc)

    def _stop_record(self) -> Optional[int]:
        if self._record_proc is None:
            return None

        duration_ms = int((time.monotonic() - self._record_start_monotonic) * 1000)
        proc = self._record_proc
        self._record_proc = None
        try:
            proc.terminate()
            proc.wait(timeout=2.5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=1.0)
        except OSError:
            pass

        err_tail = ""
        if proc.stderr is not None:
            try:
                err_tail = (proc.stderr.read() or "").strip()[-400:]
            except OSError:
                err_tail = ""
        if proc.returncode not in (0, -15):
            LOG.warning("arecord exit=%s stderr=%s", proc.returncode, err_tail)
        LOG.info("record stop duration=%dms file=%s", duration_ms, self.wav_path)
        return duration_ms

    def _transcribe(self) -> str:
        if self.fixed_text:
            return self.fixed_text
        if self.stt_cmd:
            return self._transcribe_via_command()
        if self.mode == "speech_recognition":
            return self._transcribe_via_speech_recognition()
        LOG.warning("unknown PET_VOICE_STT_MODE=%s, no transcript", self.mode)
        return ""

    def _transcribe_via_command(self) -> str:
        cmd = self.stt_cmd.replace("{wav}", str(self.wav_path))
        LOG.info("stt command: %s", cmd)
        try:
            result = subprocess.run(
                cmd,
                shell=True,
                check=False,
                capture_output=True,
                text=True,
            )
        except OSError as exc:
            LOG.error("stt command failed: %s", exc)
            return ""
        if result.returncode != 0:
            LOG.warning("stt command exit=%d stderr=%s", result.returncode, (result.stderr or "").strip()[-300:])
            return ""
        return (result.stdout or "").strip()

    def _transcribe_via_speech_recognition(self) -> str:
        try:
            import speech_recognition as sr
        except Exception as exc:
            LOG.error("speech_recognition not available: %s", exc)
            return ""

        recognizer = sr.Recognizer()
        try:
            with sr.AudioFile(str(self.wav_path)) as source:
                audio = recognizer.record(source)
            text = recognizer.recognize_google(audio, language=self.stt_language)
            return text.strip()
        except sr.UnknownValueError:
            LOG.info("stt could not recognize speech")
            return ""
        except sr.RequestError as exc:
            LOG.error("stt request failed: %s", exc)
            return ""
        except Exception as exc:
            LOG.error("stt failed: %s", exc)
            return ""

    def _post_input_action(self, text: str) -> bool:
        payload = {
            "type": self.action_type,
            "view": self.action_view,
            "state": text,
            "targetState": "voice_transcribed",
            "activeDetailId": "voice_ptt",
        }
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urlrequest.Request(
            self.inject_url,
            data=data,
            headers={"Content-Type": "application/json; charset=utf-8"},
            method="POST",
        )
        try:
            with urlrequest.urlopen(req, timeout=self.inject_timeout_s) as resp:
                body = resp.read().decode("utf-8", errors="ignore")
                if 200 <= resp.status < 300:
                    LOG.info("input/action posted ok status=%s", resp.status)
                    return True
                LOG.warning("input/action http=%s body=%s", resp.status, body[-300:])
                return False
        except urlerror.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="ignore") if hasattr(exc, "read") else str(exc)
            LOG.warning("input/action http error=%s body=%s", exc.code, detail[-300:])
            return False
        except Exception as exc:
            LOG.error("input/action request failed: %s", exc)
            return False

    def run(self) -> int:
        LOG.info(
            "start root=%s button=%s wav=%s inject=%s stt_mode=%s",
            self.root,
            self.button_state_path,
            self.wav_path,
            self.inject_url,
            self.mode,
        )
        self._prime_marker()

        running = True

        def _stop(_sig, _frame):
            nonlocal running
            running = False

        signal.signal(signal.SIGINT, _stop)
        signal.signal(signal.SIGTERM, _stop)

        while running:
            event = self._read_button_event()
            if event == "down":
                self._start_record()
            elif event == "up":
                duration_ms = self._stop_record()
                if duration_ms is None:
                    continue
                if duration_ms < self.min_record_ms:
                    LOG.info("skip short recording duration=%dms", duration_ms)
                    continue
                if not self.wav_path.exists():
                    LOG.warning("recorded wav missing: %s", self.wav_path)
                    continue
                text = self._transcribe()
                text = text.strip()
                if not text:
                    LOG.info("empty transcript, skip inject")
                    continue
                LOG.info("transcript: %s", text)
                self._post_input_action(text)

            if (
                self._record_proc is not None
                and (time.monotonic() - self._record_start_monotonic) > self.max_record_s
            ):
                LOG.warning("record timeout reached (%ss), auto-stop", self.max_record_s)
                duration_ms = self._stop_record()
                if duration_ms is not None and duration_ms >= self.min_record_ms:
                    text = self._transcribe().strip()
                    if text:
                        self._post_input_action(text)

            time.sleep(self.poll_interval_s)

        # Graceful shutdown: stop active recording, but do not inject on exit.
        if self._record_proc is not None:
            self._stop_record()
        LOG.info("stopped")
        return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Push-to-talk recorder for board runtime")
    parser.add_argument("root", nargs="?", default="/opt/board-runtime", help="board runtime root path")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    level = env_text("BOARD_VOICE_PTT_LOG", "INFO").upper()
    logging.basicConfig(
        level=getattr(logging, level, logging.INFO),
        format="[voice-ptt] %(asctime)s %(levelname)s %(message)s",
    )
    args = parse_args(argv)
    service = VoicePttService(Path(args.root))
    return service.run()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
