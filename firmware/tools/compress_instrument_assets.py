"""Losslessly pack the existing RGB565+alpha assets; never resample pixels."""
import argparse
from pathlib import Path
import struct
import zlib

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1] / "assets/instruments"
    for name, expected in [("body", (320, 244)), ("mallet", (160, 163))]:
        source = root / f"wooden_fish_{name}.bin"
        raw = source.read_bytes()
        width, height = struct.unpack("<HH", raw[:4])
        if (width, height) != expected or len(raw) != 4 + width * height * 3:
            raise ValueError(f"Invalid bounded instrument layer: {name}")
        packed = zlib.compress(raw, 9)
        assert zlib.decompress(packed) == raw
        output = source.with_suffix(".deflate")
        if args.check:
            if not output.exists() or output.read_bytes() != packed:
                raise ValueError(f"Stale compressed instrument layer: {name}")
        else:
            output.write_bytes(packed)
        print(f"{name}: {len(raw)} -> {len(packed)} bytes; lossless")

if __name__ == "__main__":
    main()
