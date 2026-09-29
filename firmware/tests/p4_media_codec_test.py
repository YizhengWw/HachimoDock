"""Run the real Rust encoder against the real C decoder and independent IMA reference."""
import ctypes
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[2]

@pytest.fixture(scope="module")
def codecs(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("media-codec")
    library = tmp / "codec.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", str(ROOT / "firmware/main/pet_p4_media_codec.c"), "-o", str(library)], check=True)
    harness = tmp / "encoder.rs"
    harness.write_text('use std::io::{Read,Write};\nmod codec {include!("' + str(ROOT / "pc/src-tauri/src/media_codec.rs") + '");}\nfn main(){let mut pcm=vec![];std::io::stdin().read_to_end(&mut pcm).unwrap();std::io::stdout().write_all(&codec::encode(&pcm).unwrap()).unwrap();}\n')
    # Module-level //! docs cannot be included after an include macro; use #[path].
    harness.write_text(harness.read_text().replace('mod codec {include!("', '#[path="').replace('");}', '"] mod codec;'))
    rustc = shutil.which("rustc") or str(Path.home() / ".rustup/toolchains/stable-aarch64-apple-darwin/bin/rustc")
    exe = tmp / "encoder"
    subprocess.run([rustc, "--edition=2021", str(harness), "-o", str(exe)], check=True)
    decode = ctypes.CDLL(str(library)).pet_p4_media_decode
    decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    decode.restype = ctypes.c_bool
    return exe, decode

def decode_frame(decode, frame, capacity=7680):
    out = ctypes.create_string_buffer(capacity)
    written = ctypes.c_size_t(999)
    ok = decode(frame, len(frame), out, capacity, ctypes.byref(written))
    return ok, out.raw[:written.value], written.value

@pytest.mark.parametrize("count", [1, 2, 3, 319, 320, 1919, 1920, 3839, 3840])
def test_independent_codec_roundtrip(codecs, count):
    audioop = pytest.importorskip("audioop")
    exe, decode = codecs
    original = [int(9000 * math.sin(i * 2 * math.pi * 440 / 48000)) for i in range(count)]
    pcm = struct.pack("<" + "h" * count, *original)
    frame = subprocess.check_output([str(exe)], input=pcm)
    ok, decoded, written = decode_frame(decode, frame)
    assert ok and written == count * 2
    predictor, index = struct.unpack("<hB", frame[:3])
    # audioop expects high nibble first; our wire format is low nibble first.
    swapped = bytes((byte >> 4) | ((byte & 15) << 4) for byte in frame[6:])
    reference, _ = audioop.adpcm2lin(swapped, 2, (predictor, index))
    assert decoded == frame[:2] + reference[:(count - 1) * 2]
    if count > 300:
        samples = struct.unpack("<" + "h" * count, decoded)
        mse = sum((a-b)**2 for a,b in zip(original, samples))/count
        assert 10 * math.log10(sum(a*a for a in original)/count/max(1, mse)) > 25

def test_malformed_blocks_are_rejected_without_output(codecs):
    exe, decode = codecs
    valid = subprocess.check_output([str(exe)], input=b"\0" * 3840)
    malformed = [b"", valid[:5], valid[:-1], valid+b"x"]
    for offset, value in [(2, 89), (3, 1), (4, 0), (5, 255), (len(valid)-1, 240)]:
        frame = bytearray(valid); frame[offset] = value; malformed.append(bytes(frame))
    for frame in malformed:
        ok, _, written = decode_frame(decode, frame)
        assert not ok and written == 0
    assert not decode_frame(decode, valid, 10)[0]
