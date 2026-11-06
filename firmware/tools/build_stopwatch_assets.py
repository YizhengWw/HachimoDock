"""Generate embedded factory MJPEG from upstream terrier videos, not a USB format."""
import argparse
import pathlib
import shutil
import struct
import subprocess
import tempfile

FAMILIES = {"idle": "idle.daydreaming", "working": "working.thinking",
            "waiting_user": "waiting_user", "done": "done", "error": "error",
            "stopped": "idle.daydreaming"}


def build(output, ffmpeg="ffmpeg"):
    sources = pathlib.Path(__file__).resolve().parents[2] / "pc/public/terrier-clips"
    inputs = [sources / (name + ".mp4") for name in FAMILIES.values()]
    if output.exists() and output.stat().st_mtime >= max(
        p.stat().st_mtime for p in inputs + [pathlib.Path(__file__)]
    ):
        return
    if not shutil.which(ffmpeg):
        raise RuntimeError("FFmpeg is required for the StopWatch factory pet")
    frames, entries = [], []
    with tempfile.TemporaryDirectory(prefix="hachimo-stopwatch-") as temp:
        for index, (family, name) in enumerate(FAMILIES.items()):
            target = pathlib.Path(temp) / str(index)
            target.mkdir()
            subprocess.run(
                [ffmpeg, "-nostdin", "-v", "error", "-i", str(sources / (name + ".mp4")),
                 "-vf", "fps=8,scale=240:240:force_original_aspect_ratio=decrease,"
                 "pad=240:240:(ow-iw)/2:(oh-ih)/2:black",
                 "-frames:v", "16", "-q:v", "5", str(target / "%03d.jpg")],
                check=True)
            encoded = [p.read_bytes() for p in sorted(target.glob("*.jpg"))]
            if not encoded:
                raise RuntimeError("No frames decoded from " + name)
            entries.append((family, len(frames), len(encoded)))
            frames.extend(encoded)
    header = struct.pack("<4sHHHHI", b"HSP1", 240, 240, 8, len(entries), len(frames))
    directory = b"".join(struct.pack("<24sHH", name.encode(), first, count)
                         for name, first, count in entries)
    offset = len(header) + len(directory) + 8 * len(frames)
    index = bytearray()
    for frame in frames:
        index.extend(struct.pack("<II", offset, len(frame)))
        offset += len(frame)
    if offset > 1536 * 1024:
        raise RuntimeError("Factory animation exceeds its 1.5 MiB budget")
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = output.with_suffix(".tmp")
    pending.write_bytes(header + directory + index + b"".join(frames))
    pending.replace(output)
    print("StopWatch factory pet:", offset, "bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    build(args.output, args.ffmpeg)
