#!/usr/bin/env python3
import os
import sys
import time


def read_fb_stride(fb_path: str, width: int, bytes_per_pixel: int) -> int:
    fb_num = ""
    if fb_path.startswith("/dev/fb"):
        fb_num = fb_path[len("/dev/fb") :]
    if fb_num.isdigit():
        stride_path = f"/sys/class/graphics/fb{fb_num}/stride"
        try:
            with open(stride_path, encoding="ascii") as fh:
                stride = int(fh.read().strip())
                if stride >= width * bytes_per_pixel:
                    return stride
        except OSError:
            pass
    return width * bytes_per_pixel


def normalize_rotation(value: str) -> int:
    try:
        rotation = int(value)
    except (TypeError, ValueError):
        return 0
    rotation %= 360
    return rotation if rotation in (0, 180) else 0


def reverse_rgb565_row(row: bytes) -> bytes:
    output = bytearray(len(row))
    out_index = 0
    for index in range(len(row) - 2, -1, -2):
        output[out_index] = row[index]
        output[out_index + 1] = row[index + 1]
        out_index += 2
    return bytes(output)


def main() -> int:
    if len(sys.argv) not in (4, 5, 6):
        print("usage: fb-rawvideo-blit.py /dev/fb0 WIDTH HEIGHT [FPS] [ROTATE]", file=sys.stderr)
        return 2

    fb_path = sys.argv[1]
    width = int(sys.argv[2])
    height = int(sys.argv[3])
    fps = float(sys.argv[4]) if len(sys.argv) >= 5 else 0.0
    rotation = normalize_rotation(sys.argv[5] if len(sys.argv) == 6 else os.environ.get("PET_CLAW_FB_ROTATE", "0"))
    row_bytes = width * 2
    stride = read_fb_stride(fb_path, width, 2)
    frame_interval = 1.0 / fps if fps > 0 else 0.0
    frame_size = row_bytes * height
    next_frame_at = time.monotonic()

    with open(fb_path, "r+b", buffering=0) as fb:
        while True:
            chunk = sys.stdin.buffer.read(frame_size)
            if not chunk:
                break
            if len(chunk) < frame_size:
                break
            if rotation == 0 and stride == row_bytes:
                fb.seek(0)
                fb.write(chunk)
            else:
                for y in range(height):
                    start = y * row_bytes
                    end = start + row_bytes
                    if rotation == 180:
                        fb.seek((height - 1 - y) * stride)
                        fb.write(reverse_rgb565_row(chunk[start:end]))
                    else:
                        fb.seek(y * stride)
                        fb.write(chunk[start:end])
            os.fsync(fb.fileno())
            if frame_interval > 0:
                next_frame_at += frame_interval
                delay = next_frame_at - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                else:
                    next_frame_at = time.monotonic()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
