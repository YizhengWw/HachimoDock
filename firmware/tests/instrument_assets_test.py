"""Exact decoded pixels and bounded renderer cache, not visual-quality guesses."""
from pathlib import Path
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]

def test_instrument_assets_lossless_and_space_saved():
    subprocess.run([sys.executable, str(ROOT / "tools/compress_instrument_assets.py"), "--check"], check=True)
    original = packed = 0
    for name, dimensions in [("body", (320, 244)), ("mallet", (160, 163))]:
        stem = ROOT / f"assets/instruments/wooden_fish_{name}"
        raw, compressed = stem.with_suffix(".bin").read_bytes(), stem.with_suffix(".deflate").read_bytes()
        assert zlib.decompress(compressed) == raw
        assert struct.unpack("<HH", raw[:4]) == dimensions
        assert len(raw) == 4 + dimensions[0] * dimensions[1] * 3
        original += len(raw)
        packed += len(compressed)
    assert original - packed > 250000
    renderer = (ROOT / "main/pet_p4_renderer.c").read_text()
    assert "tinfl_decompressor *inflater=calloc" in renderer
    assert "consumed!=size||decoded!=expected" in renderer
    assert 'if(strcmp(page,"app"))instrument_release();' in renderer
    assert 'if(!app||!app->active||!app->instrument)instrument_release();' in renderer
