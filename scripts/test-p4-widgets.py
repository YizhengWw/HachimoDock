#!/usr/bin/env python3
"""
[Input] Built-in .clawpkg directories and an exclusively available ESP32-P4 USB serial port.
[Output] Per-widget install/action/tick assertions plus RGB565 screenshot PNGs and a JSON summary.
[Pos] Repeatable hardware smoke test for every built-in negative-screen component.
"""

from __future__ import annotations

import argparse
import base64
import json
import struct
import tempfile
import time
import zlib
from pathlib import Path

import serial
from serial.tools import list_ports


ROOT = Path(__file__).resolve().parents[1]
BUILTINS = ROOT / "ref" / "builtin-clawpkgs"
SUPPORTED_USB_VIDS = {0x1A86, 0x303A, 0x10C4, 0x0403}
BAUD = 4_000_000
WIDGET_CHUNK_MAX_ATTEMPTS = 3
WIDGET_COMMIT_ACK_TIMEOUT = 15.0


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def fnv1a64(data: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def write_rgb565le_png(path: Path, raw: bytes, width: int, height: int) -> None:
    require(len(raw) == width * height * 2, "screenshot byte count does not match dimensions")
    scanlines = bytearray()
    for y in range(height):
        scanlines.append(0)
        row = y * width * 2
        for x in range(width):
            offset = row + x * 2
            pixel = raw[offset] | (raw[offset + 1] << 8)
            red = ((pixel >> 11) & 0x1F) * 255 // 31
            green = ((pixel >> 5) & 0x3F) * 255 // 63
            blue = (pixel & 0x1F) * 255 // 31
            scanlines.extend((red, green, blue))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(bytes(scanlines), 9))
        + png_chunk(b"IEND", b"")
    )


def find_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    candidates = []
    for port in list_ports.comports():
        description = (port.description or "").lower()
        device = port.device.lower()
        if (
            port.vid in SUPPORTED_USB_VIDS
            or "usb" in description
            or "usbserial" in device
            or "usbmodem" in device
            or "ttyacm" in device
        ):
            candidates.append(port.device)
    if len(candidates) != 1:
        raise RuntimeError(f"expected one supported USB serial device, found {candidates}")
    return candidates[0]


class P4Client:
    def __init__(self, port: str, baud: int) -> None:
        self.port = port
        self.serial = serial.Serial(port, baud, timeout=0.12, write_timeout=2)
        self.serial.dtr = True
        self.serial.rts = True
        self.desktop_id = f"widget-smoke-{int(time.time())}"
        self.last_heartbeat = 0.0
        self.hello = self._handshake()

    def close(self) -> None:
        self.serial.close()

    def send(self, topic: str, payload: dict) -> None:
        message = json.dumps(
            {"topic": topic, "payload": payload},
            ensure_ascii=False,
            separators=(",", ":"),
        )
        self.serial.write((message + "\n").encode("utf-8"))
        self.serial.flush()

    def heartbeat(self, force: bool = False) -> None:
        now = time.monotonic()
        if force or now - self.last_heartbeat >= 1.5:
            self.send("system/heartbeat", {"desktopDeviceId": self.desktop_id})
            self.last_heartbeat = now

    def send_handshake(self) -> None:
        self.send("bind", {"desktopDeviceId": self.desktop_id})
        legacy_hello = {
            "v": 1,
            "type": "hello",
            "desktopDeviceId": self.desktop_id,
            "namespace": "desk",
        }
        self.serial.write(
            (json.dumps(legacy_hello, separators=(",", ":")) + "\n").encode("utf-8")
        )
        self.serial.flush()

    def read_message(self, deadline: float) -> dict | None:
        while time.monotonic() < deadline:
            self.heartbeat()
            raw = self.serial.readline()
            if not raw:
                continue
            try:
                message = json.loads(raw.decode("utf-8", "replace").strip())
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if isinstance(message, dict) and isinstance(message.get("topic"), str):
                return message
        return None

    def wait_for(self, topic: str, predicate=lambda payload: True, timeout: float = 5.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            message = self.read_message(deadline)
            if message is None:
                break
            payload = message.get("payload") or {}
            if message.get("topic") == topic and predicate(payload):
                return payload
        raise TimeoutError(f"timed out waiting for {topic}")

    def _handshake(self) -> dict:
        deadline = time.monotonic() + 12
        next_probe = 0.0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_probe:
                self.send_handshake()
                self.heartbeat(force=True)
                next_probe = now + 1.0
            message = self.read_message(deadline)
            if message and message.get("topic") == "hello":
                payload = message.get("payload") or {}
                if payload.get("runtime") == "esp-p4":
                    return payload
        raise TimeoutError("ESP32-P4 hello was not received")

    def sleep(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.heartbeat()
            time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))

    def install(self, package: Path, runtime_transform=None) -> dict:
        manifest = json.loads((package / "component.json").read_text(encoding="utf-8"))
        widget_id = manifest["id"]
        transfer_id = f"smoke-{widget_id}-{int(time.time() * 1000)}"
        files = []
        for path in sorted(package.rglob("*")):
            if not path.is_file() or path.name == ".keep":
                continue
            relative = path.relative_to(package).as_posix()
            data = path.read_bytes()
            if relative == "runtime/widget.json":
                runtime = json.loads(data.decode("utf-8"))
                if widget_id == "token-usage":
                    runtime.pop("readers", None)
                    runtime.pop("fetchers", None)
                if runtime_transform:
                    runtime_transform(runtime)
                data = json.dumps(runtime, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            files.append((relative, data))

        self.send("widget/begin", {"transferId": transfer_id, "widgetId": widget_id})
        begin = self.wait_for(
            "widget-install-ack",
            lambda p: p.get("transferId") == transfer_id and p.get("phase") == "begin",
        )
        require(begin.get("ok") is True, f"{widget_id} begin rejected: {begin}")

        for relative, data in files:
            payload = {
                "transferId": transfer_id,
                "path": relative,
                "data": base64.b64encode(data).decode("ascii"),
                "index": "0",
                "decodedSize": len(data),
                "checksum": fnv1a64(data),
            }
            last_error = "no acknowledgement"
            for attempt in range(1, WIDGET_CHUNK_MAX_ATTEMPTS + 1):
                self.send("widget/chunk", payload)
                try:
                    chunk = self.wait_for(
                        "widget-install-ack",
                        lambda p: p.get("transferId") == transfer_id and p.get("phase") == "chunk",
                        timeout=2.5,
                    )
                except TimeoutError as error:
                    last_error = str(error)
                else:
                    if chunk.get("ok") is True:
                        break
                    last_error = str(chunk)
                if attempt < WIDGET_CHUNK_MAX_ATTEMPTS:
                    self.sleep(0.1)
            else:
                raise AssertionError(
                    f"{widget_id} chunk {relative} failed after "
                    f"{WIDGET_CHUNK_MAX_ATTEMPTS} attempts: {last_error}"
                )

        self.send("widget/commit", {"transferId": transfer_id, "widgetId": widget_id})
        commit = self.wait_for(
            "widget-install-ack",
            lambda p: p.get("transferId") == transfer_id and p.get("phase") == "commit",
            timeout=WIDGET_COMMIT_ACK_TIMEOUT,
        )
        require(commit.get("ok") is True, f"{widget_id} commit rejected: {commit}")
        view = self.query()
        require(view.get("widgetId") == widget_id, f"{widget_id} did not become active: {view}")
        return view

    def query(self) -> dict:
        self.send("miniapp/query", {})
        return self.wait_for("miniapp/state")

    def action(self, action: str) -> dict:
        request_id = f"event-{int(time.time() * 1000)}"
        self.send(
            "miniapp/event",
            {"action": action, "tsMs": int(time.monotonic() * 1000), "requestId": request_id},
        )
        ack = self.wait_for(
            "protocol/ack",
            lambda p: p.get("requestId") == request_id,
        )
        require(ack.get("ok") is True, f"action {action} rejected: {ack}")
        return self.query()

    def set_page(self, page: str) -> None:
        request_id = f"page-{int(time.time() * 1000)}"
        self.send("control/screen-page", {"page": page, "requestId": request_id})
        ack = self.wait_for(
            "protocol/ack",
            lambda p: p.get("requestId") == request_id,
        )
        require(ack.get("ok") is True, f"screen page {page} rejected: {ack}")
        self.sleep(0.2)

    def update_stats(self) -> dict:
        self.send(
            "stats/update",
            {
                "source": "codex",
                "state": "working",
                "sessionTitle": "Widget smoke test",
                "tokenUsage": {
                    "totalTokens": 18432,
                    "inputTokens": 13000,
                    "outputTokens": 2432,
                    "cachedInputTokens": 3000,
                },
                "tsMs": int(time.time() * 1000),
            },
        )
        self.sleep(0.2)
        return self.query()

    def screenshot(self, output: Path) -> dict:
        self.send("debug/screenshot", {})
        begin = self.wait_for("debug/screenshot_begin", timeout=8)
        screenshot_id = begin["id"]
        chunks = {}
        deadline = time.monotonic() + 15
        end = None
        while time.monotonic() < deadline:
            message = self.read_message(deadline)
            if not message:
                break
            payload = message.get("payload") or {}
            if payload.get("id") != screenshot_id:
                continue
            if message.get("topic") == "debug/screenshot_chunk":
                chunks[int(payload["index"])] = base64.b64decode(payload["data"])
            elif message.get("topic") == "debug/screenshot_end":
                end = payload
                break
        require(end is not None, "screenshot did not finish")
        require(len(chunks) == int(end["chunks"]), f"screenshot chunks are incomplete: {end}")
        raw = b"".join(chunks[index] for index in range(len(chunks)))
        write_rgb565le_png(output, raw, int(begin["width"]), int(begin["height"]))
        require(int(end.get("nonBlack", 0)) > 0, "screenshot is blank")
        return {"path": str(output), "checksum": end.get("checksum"), "nonBlack": end.get("nonBlack")}


def accelerated(runtime: dict) -> None:
    for tick in runtime.get("tick", []):
        tick["every_ms"] = 100


def test_drink(client: P4Client, package: Path, output: Path) -> dict:
    initial = client.install(package)
    require(
        initial["state"] == "counting"
        and initial["page"] == "next"
        and initial["metricValue"] == "60",
        f"bad drink initial view: {initial}",
    )
    client.set_page("app")
    shot = client.screenshot(output / "drink-reminder.png")
    acknowledged = client.action("reminder.acknowledge")
    require(
        acknowledged["state"] == "counting" and acknowledged["metricValue"] == "60",
        f"drink acknowledge failed: {acknowledged}",
    )
    today = client.action("reminder.switch_view")
    require(
        today["page"] == "today" and today["metricValue"] == "1",
        f"drink today view failed: {today}",
    )
    paused = client.action("reminder.pause_resume")
    require(paused["state"] == "paused", f"drink pause failed: {paused}")
    resumed = client.action("reminder.pause_resume")
    require(resumed["state"] == "counting", f"drink resume failed: {resumed}")
    next_view = client.action("reminder.switch_view")
    require(next_view["page"] == "next", f"drink next view failed: {next_view}")

    def boundary(runtime: dict) -> None:
        accelerated(runtime)
        runtime["vars"]["remaining_min"]["init"] = 2

    client.install(package, boundary)
    client.sleep(0.35)
    due = client.query()
    require(
        due["state"] == "due" and due["metricValue"] == "0",
        f"drink reminder did not become due: {due}",
    )
    due_shot = client.screenshot(output / "drink-reminder-due.png")
    reset = client.action("reminder.acknowledge")
    require(
        reset["state"] == "counting" and reset["metricValue"] == "60",
        f"drink due acknowledge failed: {reset}",
    )
    return {"initial": initial, "due": due, "screenshot": shot, "dueScreenshot": due_shot}


def test_falling_catch(client: P4Client, package: Path, output: Path) -> dict:
    initial = client.install(package)
    require(initial["state"] == "ready" and initial["metricValue"] == "0", f"bad catch initial view: {initial}")
    client.set_page("app")
    ready_shot = client.screenshot(output / "falling-catch-ready.png")
    playing = client.action("catch.start")
    require(playing["state"] == "playing", f"catch did not start: {playing}")
    before_move = client.screenshot(output / "falling-catch-playing.png")
    require(client.action("catch.left")["state"] == "playing", "catch left action stopped the round")
    require(client.action("catch.right")["state"] == "playing", "catch right action stopped the round")
    client.sleep(0.2)
    after_move = client.screenshot(output / "falling-catch-moved.png")
    require(
        before_move.get("checksum") != after_move.get("checksum"),
        "catch scene did not change after movement and scene ticks",
    )

    def boundary(runtime: dict) -> None:
        accelerated(runtime)
        runtime["vars"]["remaining_s"]["init"] = 2
        runtime["vars"]["round_s"]["init"] = 2
        runtime["scene"]["tick_ms"] = 80

    client.install(package, boundary)
    client.action("catch.start")
    client.sleep(0.35)
    result = client.query()
    require(result["state"] == "result", f"catch round did not finish: {result}")
    result_shot = client.screenshot(output / "falling-catch-result.png")
    return {
        "initial": initial,
        "result": result,
        "readyScreenshot": ready_shot,
        "playingScreenshot": before_move,
        "movedScreenshot": after_move,
        "resultScreenshot": result_shot,
    }


def test_tomato(client: P4Client, package: Path, output: Path) -> dict:
    initial = client.install(package)
    require(initial["state"] == "focus" and initial["metricValue"] == "25:00", f"bad tomato initial view: {initial}")
    client.set_page("app")
    shot = client.screenshot(output / "tomato-clock.png")
    client.sleep(1.25)
    ticking = client.query()
    require(ticking["state"] == "focus" and ticking["metricValue"] < "25:00", f"tomato timer did not advance: {ticking}")
    paused = client.action("tomato.start_pause")
    require(paused["state"] == "focus_paused", f"tomato focus pause failed: {paused}")
    paused_value = paused["metricValue"]
    client.sleep(1.2)
    require(client.query()["metricValue"] == paused_value, "tomato timer advanced while paused")
    require(client.action("tomato.start_pause")["state"] == "focus", "tomato focus resume failed")
    skipped = client.action("tomato.skip_phase")
    require(
        skipped["state"] == "rest"
        and skipped["metricValue"] == "05:00"
        and skipped["badge"] == "1",
        f"tomato skip focus failed: {skipped}",
    )
    client.sleep(1.1)
    reset_rest = client.action("tomato.reset_phase")
    require(
        reset_rest["state"] == "rest" and reset_rest["metricValue"] == "05:00",
        f"tomato reset rest failed: {reset_rest}",
    )
    focus_again = client.action("tomato.skip_phase")
    require(
        focus_again["state"] == "focus" and focus_again["metricValue"] == "25:00",
        f"tomato skip rest failed: {focus_again}",
    )

    def boundary(runtime: dict) -> None:
        accelerated(runtime)
        runtime["vars"]["remaining_s"]["init"] = 2

    client.install(package, boundary)
    client.sleep(0.35)
    resting = client.query()
    require(resting["state"] == "rest" and resting["badge"] == "1", f"tomato did not enter rest: {resting}")
    rest_shot = client.screenshot(output / "tomato-clock-rest.png")
    rest_paused = client.action("tomato.start_pause")
    require(rest_paused["state"] == "rest_paused", f"tomato rest pause failed: {rest_paused}")
    rest_value = rest_paused["metricValue"]
    client.sleep(0.35)
    require(client.query()["metricValue"] == rest_value, "tomato rest timer advanced while paused")
    require(client.action("tomato.start_pause")["state"] == "rest", "tomato rest resume failed")
    client.sleep(0.15)
    reset = client.action("tomato.reset_phase")
    require(reset["state"] == "rest" and reset["metricValue"] == "05:00", f"tomato reset failed: {reset}")
    return {"initial": initial, "resting": resting, "screenshot": shot, "restScreenshot": rest_shot}


def test_token(client: P4Client, package: Path, output: Path) -> dict:
    initial = client.install(package)
    require(
        initial["state"] == "live" and initial["page"] == "total",
        f"bad token initial view: {initial}",
    )
    live = client.update_stats()
    require(live["metricValue"] == "18.4K", f"token total was not synchronized: {live}")
    require(live["eyebrow"] == "codex" and live["headline"] == "Widget smoke test", f"token labels were not synchronized: {live}")
    require(live["note"] == "缓存 3K", f"token cache was not synchronized: {live}")
    input_view = client.action("stats.show_input")
    require(
        input_view["page"] == "input" and input_view["metricValue"] == "13K",
        f"token input view failed: {input_view}",
    )
    output_view = client.action("stats.show_output")
    require(
        output_view["page"] == "output" and output_view["metricValue"] == "2.4K",
        f"token output view failed: {output_view}",
    )
    total_view = client.action("stats.show_total")
    require(
        total_view["page"] == "total" and total_view["metricValue"] == "18.4K",
        f"token total view restore failed: {total_view}",
    )
    client.set_page("app")
    shot = client.screenshot(output / "token-usage.png")
    return {
        "initial": initial,
        "live": live,
        "input": input_view,
        "output": output_view,
        "screenshot": shot,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Smoke-test every built-in ESP32-P4 negative-screen widget")
    parser.add_argument("--port", help="USB serial port; omitted means auto-detect exactly one supported board")
    parser.add_argument("--baud", type=int, default=BAUD)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(tempfile.gettempdir()) / "claw-pet-widget-smoke",
        help="directory for screenshots and summary JSON",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    port = find_port(args.port)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    client = P4Client(port, args.baud)
    original = client.query()
    results = {}
    tests = [
        ("falling-catch", test_falling_catch),
        ("drink-reminder", test_drink),
        ("tomato-clock", test_tomato),
        ("token-usage", test_token),
    ]
    try:
        for widget_id, test in tests:
            print(f"[widget-smoke] testing {widget_id}", flush=True)
            results[widget_id] = test(client, BUILTINS / widget_id, output)
            print(f"[widget-smoke] passed {widget_id}", flush=True)
    finally:
        original_id = original.get("widgetId") if original.get("active") else None
        original_path = BUILTINS / str(original_id)
        if original_id and original_path.is_dir():
            client.install(original_path)
        client.set_page("main")
        client.close()

    summary = {
        "ok": True,
        "port": port,
        "boardDeviceId": client.hello.get("boardDeviceId"),
        "firmware": client.hello.get("fw"),
        "output": str(output),
        "components": results,
    }
    summary_path = output / "summary.json"
    summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"ok": True, "summary": str(summary_path)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
