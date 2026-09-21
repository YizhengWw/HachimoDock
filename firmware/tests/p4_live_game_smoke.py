#!/usr/bin/env python3
"""Exercise an already-installed P4 declarative component over USB JSONL.

This is an opt-in hardware smoke test, not part of pytest. Close Pet Manager
before running so the script owns the serial port. Legacy game presets have
known default actions; generic scenes and tools provide their actions explicitly.
"""

from __future__ import annotations

import argparse
import json
import time

import serial


def read_message(port: serial.Serial, topic: str, timeout_s: float = 5.0) -> dict:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", errors="ignore").strip()
        start = text.find("{")
        if start < 0:
            continue
        try:
            message = json.loads(text[start:])
        except json.JSONDecodeError:
            continue
        if message.get("topic") == topic:
            return message.get("payload") or {}
    raise TimeoutError(f"timed out waiting for {topic}")


def send_message(port: serial.Serial, topic: str, payload: dict | None = None) -> None:
    message = {"topic": topic, "payload": payload or {}}
    port.write(json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n")
    port.flush()


def wait_for_p4(port: serial.Serial, timeout_s: float = 12.0) -> dict:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        send_message(port, "bind", {"desktopDeviceId": "p4-live-widget-smoke"})
        port.write(
            b'{"v":1,"type":"hello","desktopDeviceId":"p4-live-widget-smoke","namespace":"desk"}\n'
        )
        port.flush()
        try:
            payload = read_message(port, "hello", timeout_s=0.7)
        except TimeoutError:
            continue
        if payload.get("runtime") == "esp-p4":
            return payload
    raise TimeoutError("timed out waiting for an ESP-P4 hello")


def query_widget(port: serial.Serial) -> dict:
    port.reset_input_buffer()
    send_message(port, "miniapp/query")
    payload = read_message(port, "miniapp/state")
    if not payload.get("active"):
        raise AssertionError("no active mini-app")
    return payload


def dispatch(port: serial.Serial, action: str) -> None:
    port.reset_input_buffer()
    send_message(
        port,
        "miniapp/event",
        {
            "action": action,
            "tsMs": int(time.monotonic() * 1000),
            "requestId": f"live-{time.monotonic_ns()}",
        },
    )
    ack = read_message(port, "protocol/ack")
    if not ack.get("ok"):
        raise AssertionError(f"action {action} was rejected: {ack}")


def revision_of(payload: dict) -> int:
    return int(payload.get("gameRevision") or payload.get("revision") or 0)


def run(
    port_name: str,
    baud: int,
    start_action: str | None,
    control_action: str | None,
    expected_state: str,
    tick_wait: float | None,
) -> dict:
    port = serial.Serial()
    port.port = port_name
    port.baudrate = baud
    port.timeout = 0.15
    port.write_timeout = 2
    port.dtr = False
    port.rts = False
    port.open()
    try:
        hello = wait_for_p4(port)
        initial = query_widget(port)
        game_type = initial.get("gameType", "")
        start_actions = {
            "blocks": "blocks.start",
            "snake": "snake.start",
            "flappy": "flappy.flap",
        }
        resolved_start = start_action or start_actions.get(game_type)
        if not resolved_start:
            raise AssertionError("generic scenes and tools require --start-action")
        dispatch(port, resolved_start)
        started = query_widget(port)
        if started.get("state") != expected_state:
            raise AssertionError(f"component did not enter {expected_state}: {started}")
        if game_type and not started.get("gameRunning"):
            raise AssertionError(f"game scene did not start: {started}")

        resolved_tick_wait = tick_wait if tick_wait is not None else (1.1 if game_type == "blocks" else 0.55)
        time.sleep(resolved_tick_wait)
        moved = query_widget(port)
        if revision_of(moved) <= revision_of(started):
            raise AssertionError(f"component revision did not advance: {started} -> {moved}")

        control_actions = {
            "blocks": "blocks.drop",
            "snake": "snake.turn_left",
            "flappy": "flappy.flap",
        }
        resolved_control = control_action or control_actions.get(game_type)
        if not resolved_control:
            raise AssertionError("generic scenes and tools require --control-action")
        dispatch(port, resolved_control)
        time.sleep(0.3)
        controlled = query_widget(port)
        if revision_of(controlled) <= revision_of(moved):
            raise AssertionError(f"component control did not update its revision: {moved} -> {controlled}")
        if game_type == "blocks" and controlled.get("gameScore", 0) <= moved.get("gameScore", 0):
            raise AssertionError(f"hard drop did not score: {moved} -> {controlled}")

        return {
            "boardDeviceId": hello.get("boardDeviceId"),
            "widgetRuntime": (hello.get("capabilities") or {}).get("widgetRuntime"),
            "widgetScene": (hello.get("capabilities") or {}).get("widgetScene"),
            "widgetId": controlled.get("widgetId"),
            "gameType": game_type or None,
            "grid": [controlled.get("gameGridWidth"), controlled.get("gameGridHeight")],
            "revision": [
                revision_of(initial),
                revision_of(started),
                revision_of(moved),
                revision_of(controlled),
            ],
            "score": controlled.get("gameScore"),
            "running": controlled.get("gameRunning"),
            "state": controlled.get("state"),
        }
    finally:
        port.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=4_000_000)
    parser.add_argument("--start-action")
    parser.add_argument("--control-action")
    parser.add_argument("--expected-state", default="playing")
    parser.add_argument("--tick-wait", type=float)
    args = parser.parse_args()
    print(json.dumps(
        run(
            args.port,
            args.baud,
            args.start_action,
            args.control_action,
            args.expected_state,
            args.tick_wait,
        ),
        ensure_ascii=False,
        indent=2,
    ))


if __name__ == "__main__":
    main()
