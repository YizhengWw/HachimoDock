"""Compile real renderer replay functions against an intentionally mutating decoder."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.index("static bool " + name + "(") if name != "reset_h264_decoder" else source.index("static void " + name + "(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_mutating_decoder_cannot_damage_cached_stream():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    declarations = source[source.index("static esp_h264_dec_handle_t g_h264_decoder;"):source.index("static uint8_t *g_h264_ppa_input;")]
    functions = "\n".join(function(source, name) for name in [
        "reset_h264_decoder", "start_h264_decoder", "decode_next_h264_frame",
    ])
    harness = (ROOT / "tests/p4_h264_replay_test.c").read_text()
    with tempfile.TemporaryDirectory() as tmp:
        test = Path(tmp) / "replay.c"
        binary = Path(tmp) / "replay"
        test.write_text(harness.replace("/* RENDERER_IMPLEMENTATION */", declarations + '\n' + functions))
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_h264_render_uses_private_replay_buffer():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    decode = function(source, "decode_next_h264_frame")
    assert ".buffer = g_h264_work_stream + g_h264_stream_offset" in decode
    assert "(uint8_t *) (stream +" not in decode
    assert "start_h264_decoder(fs_path, stream->bytes, stream->size)" in source
    assert "decode_next_h264_frame(asset->width, asset->height)" in source
    start = function(source, "start_h264_decoder")
    assert "stream_size > PET_P4_ASSET_CACHE_MAX_FILE_BYTES" in start
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in start
