#!/usr/bin/env python3
"""Execute a command on the board via serial console and print output.

Usage:
    python3 scripts/serial-exec.py "sh /mnt/extsd/apply-on-board.sh"
    python3 scripts/serial-exec.py "ps w | grep board"
    python3 scripts/serial-exec.py --send-file local.b64 /mnt/UDISK/board-runtime/update.b64

Environment variables:
    SERIAL_PORT   - serial device (default: /dev/cu.usbserial-210)
    SERIAL_BAUD   - baud rate (default: 115200)
    SERIAL_TIMEOUT - seconds to wait for output (default: 30)
"""
import sys
import os
import time
try:
    import serial
except ModuleNotFoundError:
    serial = None
    import fcntl
    import select
    import struct
    import termios
    import tty

PORT = os.environ.get("SERIAL_PORT", "/dev/cu.usbserial-210")
BAUD = int(os.environ.get("SERIAL_BAUD", "115200"))
TIMEOUT = int(os.environ.get("SERIAL_TIMEOUT", "30"))
SEND_CHUNK = int(os.environ.get("SERIAL_SEND_CHUNK", "1024"))


class StdlibSerial:
    def __init__(self, port, baud, timeout=1):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.timeout = timeout
        self._old_attrs = termios.tcgetattr(self.fd)
        attrs = termios.tcgetattr(self.fd)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        baud_const = getattr(termios, f"B{baud}", termios.B115200)
        attrs[4] = baud_const
        attrs[5] = baud_const
        attrs[2] |= termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    @property
    def in_waiting(self):
        try:
            return struct.unpack("I", fcntl.ioctl(self.fd, termios.FIONREAD, struct.pack("I", 0)))[0]
        except OSError:
            ready, _, _ = select.select([self.fd], [], [], 0)
            return 1 if ready else 0

    def reset_input_buffer(self):
        while True:
            ready, _, _ = select.select([self.fd], [], [], 0)
            if not ready:
                return
            try:
                if not os.read(self.fd, 4096):
                    return
            except BlockingIOError:
                return

    def write(self, data):
        total = 0
        while total < len(data):
            try:
                total += os.write(self.fd, data[total:])
            except BlockingIOError:
                time.sleep(0.01)

    def flush(self):
        termios.tcdrain(self.fd)

    def read(self, size):
        deadline = time.time() + self.timeout
        while True:
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if ready:
                try:
                    return os.read(self.fd, size)
                except BlockingIOError:
                    pass
            if time.time() >= deadline:
                return b""

    def close(self):
        termios.tcsetattr(self.fd, termios.TCSANOW, self._old_attrs)
        os.close(self.fd)


def open_serial(port, baud, timeout=1):
    if serial is not None:
        return serial.Serial(port, baud, timeout=timeout)
    return StdlibSerial(port, baud, timeout=timeout)


def shell_quote(value):
    return "'" + value.replace("'", "'\"'\"'") + "'"


def read_until_marker(ser, marker, timeout, echo=True):
    deadline = time.time() + timeout
    buf = ""
    while time.time() < deadline:
        waiting = ser.in_waiting
        if waiting > 0:
            data = ser.read(waiting)
        else:
            time.sleep(0.1)
            data = ser.read(ser.in_waiting or 1)
        if data:
            text = data.decode("utf-8", errors="replace")
            buf += text
            found = marker in buf
            if len(buf) > 4096:
                buf = buf[-4096:]
            if echo:
                sys.stdout.write(text)
                sys.stdout.flush()
            if found:
                return True
    return False


def drain_available(ser, max_idle=0.2):
    idle_deadline = time.time() + max_idle
    while time.time() < idle_deadline:
        waiting = ser.in_waiting
        if waiting > 0:
            ser.read(waiting)
            idle_deadline = time.time() + max_idle
        else:
            time.sleep(0.02)


def send_file(local_path, remote_path):
    size = os.path.getsize(local_path)
    marker_id = int(time.time())
    marker = f"__SEND_DONE_{marker_id}__"
    ser = open_serial(PORT, BAUD, timeout=1)
    ser.reset_input_buffer()
    ser.write(b"\r\n")
    ser.flush()
    time.sleep(0.5)
    ser.reset_input_buffer()

    start_cmd = f"cat > {shell_quote(remote_path)} <<'__SERIAL_FILE__'\r\n"
    ser.write(start_cmd.encode())
    ser.flush()
    sent = 0
    last_report = 0
    with open(local_path, "rb") as fh:
        while True:
            chunk = fh.read(SEND_CHUNK)
            if not chunk:
                break
            ser.write(chunk)
            drain_available(ser, max_idle=0.03)
            sent += len(chunk)
            if sent - last_report >= 262144 or sent == size:
                print(f"[serial-send] {sent}/{size} bytes", file=sys.stderr, flush=True)
                last_report = sent
    ser.write(f"\r\n__SERIAL_FILE__\r\n__codex_marker={marker_id}; echo __SEND_DONE_${{__codex_marker}}__\r\n".encode())
    ser.flush()
    ok = read_until_marker(ser, marker, TIMEOUT, echo=False)
    if not ok:
        recovery_marker_id = int(time.time())
        recovery_marker = f"__SEND_RECOVERED_{recovery_marker_id}__"
        print(f"\n[serial-send] timeout waiting for {marker}; sending recovery delimiter", file=sys.stderr)
        ser.write(f"\r\n__SERIAL_FILE__\r\n__codex_marker={recovery_marker_id}; echo __SEND_RECOVERED_${{__codex_marker}}__\r\n".encode())
        ser.flush()
        ok = read_until_marker(ser, recovery_marker, 30, echo=False)
    ser.close()
    if not ok:
        print(f"\n[serial-send] timeout waiting for file transfer completion", file=sys.stderr)
        return 1
    return 0

def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <command> | --send-file <local> <remote>", file=sys.stderr)
        sys.exit(1)

    if sys.argv[1] == "--send-file":
        if len(sys.argv) != 4:
            print(f"usage: {sys.argv[0]} --send-file <local> <remote>", file=sys.stderr)
            sys.exit(1)
        sys.exit(send_file(sys.argv[2], sys.argv[3]))

    cmd = sys.argv[1]

    ser = open_serial(PORT, BAUD, timeout=1)
    # Flush stale data
    ser.reset_input_buffer()

    # Wake up the console — send a bare newline first, wait for prompt
    ser.write(b"\r\n")
    ser.flush()
    time.sleep(0.5)
    ser.reset_input_buffer()

    # Use a unique marker to detect command completion
    marker_id = int(time.time())
    marker = f"__DONE_{marker_id}__"
    full_cmd = f"__codex_marker={marker_id}; {cmd}; echo __DONE_${{__codex_marker}}__\r\n"

    ser.write(full_cmd.encode())
    ser.flush()

    if not read_until_marker(ser, marker, TIMEOUT):
        print(f"\n[serial-exec] timeout after {TIMEOUT}s", file=sys.stderr)

    ser.close()

if __name__ == "__main__":
    main()
