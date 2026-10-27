"""Compile the existing original atlas into bounded device assets; no new art."""
import json
import sys
from pathlib import Path
from PIL import Image
from app_font_assets import add_fonts

root, source = map(Path, sys.argv[1:3])
path = root / "runtime/widget.json"
runtime = json.loads(path.read_text())
surface = runtime["surface"]
original = Image.open(source).convert("RGBA")
assert original.size == (1254, 1254)
# Only the growth row is used by this component: spend pixels on visible art,
# rather than transmitting six unused furniture tiles.
original.crop((0, 836, 1254, 1254)).resize((480, 160), Image.Resampling.LANCZOS).save(root / "assets/decor.png", optimize=True)
surface["images"] = [im for im in surface["images"] if not im["id"].startswith("type")]
surface.pop("fonts", None)
for im in surface["images"]:
    if im["id"] == "decor":
        im.update(frame_width=480, frame_height=160)
for n in surface["elements"]:
    if n.get("image") == "decor":
        n["tile"].update(rows=1, row=0)
    if n.get("font_size", 0) == 76:
        n["font_size"] = 48
        n["weight"] = 600
add_fonts(root, runtime, max_chars=21)
for name in ("type1.png",):
    file = root / "assets" / name
    if file.exists():
        file.unlink()  # Disposable independent working copy, not the library.
assert sum(im["frame_width"] * im["frame_height"] for im in surface["images"] if im.get("encoding") != "jpeg") <= 98304
path.write_text(json.dumps(runtime, ensure_ascii=False, separators=(",", ":")) + "\n")
print("Garden: 160px growth tiles (was 64px); 48px dynamic type; bounded 98,304-pixel palette budget")
