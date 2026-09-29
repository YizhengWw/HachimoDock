"""Compile original transparent PNG layers into bounded RGB565+A8 firmware data."""
from pathlib import Path
import struct
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
for name, size in (("body", (320, 244)), ("mallet", (160, 164))):
    im = Image.open(ROOT / f"pc/public/instruments/wooden-fish/{name}.png").convert("RGBA")
    assert im.getextrema()[3][0] == 0, "layer must have real transparency"
    im = im.crop(im.getbbox())
    im.thumbnail(size, Image.Resampling.LANCZOS)
    data = bytearray(struct.pack("<HH", *im.size))
    for r, g, b, a in im.getdata():
        data.extend(struct.pack("<HB", (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3), a))
    (ROOT / f"firmware/assets/instruments/wooden_fish_{name}.bin").write_bytes(data)
    print(name, im.size, len(data))
