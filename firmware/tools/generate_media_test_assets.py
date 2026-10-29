"""Generate independent synthetic images for the non-distributed media fixture."""
import struct
import zlib
from pathlib import Path


def png(width, height, sample):
    def chunk(kind, content):
        return (struct.pack(">I", len(content)) + kind + content
                + struct.pack(">I", zlib.crc32(kind + content)))
    rows = b"".join(b"\0" + b"".join(bytes(sample(x, y)) for x in range(width))
                    for y in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    target = Path(__file__).resolve().parents[2] / "pc/tests/fixtures/media-sample/assets"
    target.mkdir(parents=True, exist_ok=True)
    (target / "checker.png").write_bytes(png(64, 64, lambda x, y:
        (255, 255, 255, 255) if (x // 8 + y // 8) % 2 else (20, 80, 160, 160)))
    (target / "stripe.png").write_bytes(png(32, 16, lambda x, y:
        (40, 200, 90, 255) if x % 8 < 4 else (40, 200, 90, 0)))
