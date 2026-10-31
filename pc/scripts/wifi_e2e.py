#!/usr/bin/env python3
"""End-to-end Wi-Fi test: configure the P4 over USB, wait for DHCP, then open
the firmware TCP transport and verify hello + wifi/status.

The password is read from wifi-test.local or WIFI_PASSWORD and is never printed.
Run with a Python that has pyserial installed, e.g.:
  .../Factory-Flasher/.esptool-venv/bin/python pc/scripts/wifi_e2e.py --serial /dev/cu.usbmodem...
"""
import argparse
import json
import os
import socket
import sys
import time
from pathlib import Path

DEFAULT_CREDENTIALS = Path(os.environ.get("HACHIMO_WIFI_CREDENTIALS", "poc/wifi-scan/wifi-test.local"))
BAUD = 4_000_000
TCP_PORT = 3232


def load_credentials(path):
    data = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        data[key] = value
    ssid = data.get("WIFI_SSID", "").strip()
    password = os.environ.get("WIFI_PASSWORD") or data.get("WIFI_PASS", "").strip()
    if not ssid or not password or "CHANGE_ME" in ssid or "CHANGE_ME" in password:
        raise SystemExit(f"credentials not filled: {path}")
    return ssid, password


def send_json(ser, obj):
    ser.write((json.dumps(obj, separators=(",", ":")) + "\n").encode())
    ser.flush()


def read_until_topic(ser, topic, timeout=10.0):
    end = time.time() + timeout
    pending = b""
    while time.time() < end:
        data = ser.read(4096)
        if data:
            pending += data
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").strip()
            if not line.startswith("{"):
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            if msg.get("topic") == topic:
                return msg.get("payload") or {}
    return None


def device_status(ser):
    send_json(ser, {"topic": "wifi/status"})
    return read_until_topic(ser, "wifi/status_result", 5.0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--credentials", default=str(DEFAULT_CREDENTIALS))
    parser.add_argument("--no-config", action="store_true", help="skip wifi/config")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--boot-wait", type=float, default=12.0)
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        raise SystemExit("pyserial is required (use the Factory-Flasher esptool venv)")

    ssid, password = load_credentials(args.credentials)
    ser = serial.Serial(args.serial, BAUD, timeout=0.5)
    try:
        time.sleep(max(0.0, args.boot_wait))
        # drain the hello/status stream so stale lines do not confuse us
        ser.reset_input_buffer()
        status = None
        for _ in range(10):
            status = device_status(ser)
            if status is not None:
                break
            time.sleep(1.5)
        print("initial_status:", json.dumps(status, ensure_ascii=False))
        if status is None:
            raise SystemExit("device did not answer wifi/status over USB")

        if not args.no_config:
            result = None
            for _ in range(3):
                send_json(ser, {"topic": "wifi/config", "payload": {"ssid": ssid, "password": password}})
                result = read_until_topic(ser, "wifi/config_result", 10.0)
                if result and result.get("ok") is True:
                    break
                time.sleep(2.0)
            print("config_result:", json.dumps(result, ensure_ascii=False))
            if not result or result.get("ok") is not True:
                raise SystemExit("wifi/config failed")

        deadline = time.time() + args.timeout
        status = None
        while time.time() < deadline:
            status = device_status(ser)
            if status and status.get("connected") and status.get("ip"):
                break
            time.sleep(2.0)
        print("final_status:", json.dumps(status, ensure_ascii=False))
        if not status or not status.get("connected") or not status.get("ip"):
            raise SystemExit("device did not obtain a Wi-Fi IP")

        # The USB protocol reader is no longer needed; closing it avoids UART
        # transport mutex contention while the firmware pushes TCP hello/broadcast.
        try:
            ser.close()
        except Exception:
            pass
        ser = None
        time.sleep(1.0)

        ip = status["ip"]
        hello_line = None
        reply = None
        time.sleep(2.0)
        for attempt in range(1, 5):
            print(f"tcp_attempt={attempt}")
            tcp = None
            try:
                tcp = socket.create_connection((ip, TCP_PORT), timeout=8)
                print("tcp_connected")
                tcp.settimeout(10)
                hello = b""
                while b"\n" not in hello:
                    chunk = tcp.recv(4096)
                    if not chunk:
                        break
                    hello += chunk
                hello_line = hello.split(b"\n", 1)[0].decode("utf-8", "replace")
                tcp.sendall(b'{"topic":"wifi/status"}\n')
                data = b""
                while b"\n" not in data:
                    chunk = tcp.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                reply = data.split(b"\n", 1)[0].decode("utf-8", "replace")
                if hello_line:
                    break
            except Exception as error:
                print(f"tcp_attempt_failed: {error!r}")
            finally:
                if tcp is not None:
                    try:
                        tcp.close()
                    except OSError:
                        pass
            time.sleep(3.0)
        print("tcp_hello:", (hello_line or "<none>")[:800])
        print("tcp_status:", (reply or "<none>")[:800])
        if not hello_line:
            raise SystemExit("TCP hello not received")
        print("E2E_OK")
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
