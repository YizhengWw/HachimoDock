"""Opt-in StopWatch hardware verification console; no flashing or backups.

Injects synthetic session cards, polls bounded diagnostics and records JSONL.
An optional control JSON file accepts {"pattern":"colorbars|touch|pet"} and/or
{"vibrate":120}. Each file replacement is one explicit diagnostic request.
Physical pixel quality and vibration still require human confirmation.
"""
import argparse
import json
import pathlib
import time
import uuid


def run(args):
    import serial
    args.output.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(None, 115200, timeout=0.03)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    started = time.monotonic()
    active = False
    boot = None
    candidate = None
    probe_id = uuid.uuid4().hex
    pattern = args.pattern
    # An old actuator command is evidence, not a new request to replay on restart.
    control_seen = args.control.stat().st_mtime_ns if args.control and args.control.exists() else 0
    buffer = b""
    last = {}
    with args.output.open("a", encoding="utf-8") as log:
        def record(direction, value):
            log.write(json.dumps({"elapsedMs": round((time.monotonic()-started)*1000),
                                  "direction": direction, "message": value},
                                 ensure_ascii=False) + "\n")
            log.flush()

        def send(topic, payload):
            value = {"topic": topic, "payload": payload}
            port.write((json.dumps(value, ensure_ascii=False) + "\n").encode())
            record("tx", value)

        def control(**values):
            send("diagnostics/control", {"requestId": uuid.uuid4().hex,
                                         "boardDeviceId": args.expected_id, **values})

        def fixture():
            send("session/current", {"agentId": "codex", "sessionId": "auto",
                 "displayEnabled": True, "activeSessionIds": ["hil-a", "hil-b", "hil-c"],
                 "sessions": [{"id": "hil-"+name, "title": "硬件验证 "+name.upper(),
                               "content": "黄键下一项，蓝键上一项；左右滑动",
                               "state": "working", "transitionRevision": 1}
                              for name in ["a", "b", "c"]]})

        try:
            while time.monotonic()-started < args.duration:
                now = time.monotonic()
                if not active and now-last.get("hello", 0) > 2:
                    send("hello", {"desktopDeviceId": "stopwatch-hil"})
                    send("diagnostics/query", {"requestId": probe_id})
                    last["hello"] = now
                if active:
                    if now-last.get("fixture", 0) > 4:
                        fixture()
                        last["fixture"] = now
                    if now-last.get("diagnostics", 0) > 0.15:
                        send("diagnostics/query", {"requestId": uuid.uuid4().hex})
                        last["diagnostics"] = now
                    if now-last.get("pattern", 0) > 20:
                        control(action="display", pattern=pattern)
                        last["pattern"] = now
                    if args.control and args.control.exists():
                        stamp = args.control.stat().st_mtime_ns
                        if stamp != control_seen:
                            try:
                                value = json.loads(args.control.read_text())
                            except (ValueError, OSError):
                                value = {}
                            if value.get("pattern") in ["pet", "touch", "colorbars"]:
                                pattern = value["pattern"]
                                control(action="display", pattern=pattern)
                            duration = value.get("vibrate")
                            if type(duration) is int and 20 <= duration <= 300:
                                control(action="vibrate", durationMs=duration)
                            control_seen = stamp
                buffer += port.read(8192)
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    try:
                        value = json.loads(line)
                    except (ValueError, UnicodeError):
                        record("boot", line.decode(errors="replace"))
                        continue
                    record("rx", value)
                    topic = value.get("topic")
                    payload = value.get("payload", {})
                    if topic == "hello":
                        if payload.get("boardDeviceId") != args.expected_id:
                            raise RuntimeError("Connected board identity changed")
                        caps = payload.get("capabilities", {})
                        if caps.get("diagnosticsControl") != "stopwatch-hil-v1":
                            continue
                        current_boot = caps.get("debugBootId")
                        if boot and current_boot != boot:
                            raise RuntimeError("Board rebooted during hardware test")
                        candidate = payload
                    elif topic == "input/event":
                        print(json.dumps(value, ensure_ascii=False), flush=True)
                    elif topic == "diagnostics/status":
                        if not active and payload.get("requestId") == probe_id and candidate:
                            boot = candidate["capabilities"]["debugBootId"]
                            if payload.get("debugBootId") != boot:
                                continue
                            active = True
                            print(json.dumps({"ready": True, "buildId": candidate.get("buildId"),
                                              "boot": boot}, ensure_ascii=False), flush=True)
                        if now-last.get("print", 0) > 1:
                            print(json.dumps({"inputs": payload.get("inputs"), "runtime": payload.get("runtime"),
                                  "display": payload.get("display"), "board": payload.get("board")},
                                  ensure_ascii=False), flush=True)
                            last["print"] = now
                    elif topic == "protocol/ack" and payload.get("requestTopic") == "diagnostics/control":
                        print(json.dumps(value, ensure_ascii=False), flush=True)
                if not active and time.monotonic()-started > 20:
                    raise RuntimeError("No compatible StopWatch diagnostic handshake")
        except KeyboardInterrupt:
            print("Hardware verification console stopped; restoring pet view", flush=True)
        finally:
            if active:
                control(action="display", pattern="pet")
                send("session/current", {"agentId": "codex", "sessions": [],
                     "activeSessionIds": [], "displayEnabled": False})
                send("state/codex", {"state": "idle"})
            port.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--expected-id", required=True)
    parser.add_argument("--duration", type=int, default=180)
    parser.add_argument("--pattern", choices=["pet", "colorbars", "touch"], default="pet")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--control", type=pathlib.Path)
    run(parser.parse_args())
