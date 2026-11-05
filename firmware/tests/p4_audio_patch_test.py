"""Actual audio-overlay storage/phase code, synthetic IO faults, no device/API.

Tests compile production C with the existing host cJSON/resource broker. SDK
route/borrow/base64 adapters are explicit fixtures, not hardware validation.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
FUNCTIONS = ("copy_text", "json_string", "pet_p4_appearance_claim",
             "pet_p4_appearance_owned", "pet_p4_appearance_end",
             "pet_p4_appearance_mutation_blocked", "appearance_phase_try_begin",
             "pet_p4_appearance_phase_begin", "pet_p4_appearance_phase_end",
             "asset_phase_send_current")


def function(source, name):
    # A definition, never its earlier prototype. Preserve every production byte.
    match = re.search(r"(?m)^[A-Za-z_][A-Za-z0-9_ \t*]*\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", source)
    assert match, name
    pos = source.index("{", match.start())
    depth, quote, comment, escaped = 1, None, None, False
    i = pos + 1
    while depth:
        c, n = source[i], source[i:i+2]
        if comment == "line":
            if c == "\n": comment = None
        elif comment == "block":
            if n == "*/": comment = None; i += 1
        elif quote:
            if escaped: escaped = False
            elif c == "\\": escaped = True
            elif c == quote: quote = None
        elif n == "//": comment = "line"; i += 1
        elif n == "/*": comment = "block"; i += 1
        elif c in "\"'": quote = c
        elif c == "{": depth += 1
        elif c == "}": depth -= 1
        i += 1
    return source[match.start():i]


def compile_run(tmp_path, source, extra=(), faults=True, pthread=False):
    (tmp_path / "spiffs").mkdir()
    (tmp_path / "esp_attr.h").write_text("#define EXT_RAM_BSS_ATTR\n")
    src, binary = tmp_path / "case.c", tmp_path / "case"
    src.write_text(source)
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           "-D_POSIX_C_SOURCE=200809L"]
    if pthread: cmd += ["-pthread"]
    if faults:
        cmd += ['-DPET_AUDIO_PATCH_FS_PREFIX="spiffs"',
                "-DPET_AUDIO_PATCH_SYNC=patch_test_fsync",
                "-DPET_AUDIO_PATCH_INDEX_OPEN=patch_index_open",
                "-DPET_AUDIO_PATCH_INDEX_READ=patch_index_read",
                "-DPET_AUDIO_PATCH_INDEX_ERROR=patch_index_error",
                "-DPET_AUDIO_PATCH_INDEX_CLOSE=patch_index_close",
                "-DPET_AUDIO_PATCH_REMOVE=patch_remove",
                "-include", str(TESTS / "p4_audio_patch_faults.h")]
    cmd += ["-I", str(tmp_path), "-I", str(ROOT / "main"),
            "-I", str(TESTS / "vendor/cjson"), str(src)]
    cmd += [str(ROOT / p) for p in extra]
    cmd += [str(TESTS / "vendor/cjson/cJSON.c"), "-lm", "-o", str(binary)]
    subprocess.run(cmd, check=True, cwd=tmp_path, timeout=60)
    subprocess.run([str(binary)], check=True, cwd=tmp_path, timeout=30)


def test_storage_banks_io_unknown_empty_snapshot_and_originals(tmp_path):
    compile_run(tmp_path, (TESTS / "p4_audio_patch_storage_test.c").read_text(),
                ["main/pet_p4_audio_patch.c"])


def test_actual_protocol_phase_binding_empty_snapshot_and_unknown(tmp_path):
    source = (ROOT / "main/pet_p4_protocol.c").read_text()
    production = "\n".join(function(source, n) for n in FUNCTIONS)
    production += '\n#include "pet_p4_protocol_audio_patch.inc"\n'
    fixture = (TESTS / "p4_audio_patch_protocol_test.c.in").read_text()
    assert fixture.count("/* PRODUCTION_PROTOCOL */") == 1
    compile_run(tmp_path, fixture.replace("/* PRODUCTION_PROTOCOL */", production),
                ["main/pet_p4_audio_patch.c", "main/pet_p4_resources.c"])


def test_actual_custom_video_header_proof_and_existing_cache_races(tmp_path):
    raw = (ROOT / "main/pet_p4_raw_assets.c").read_text()
    for include in ('#include "esp_partition.h"', '#include "freertos/FreeRTOS.h"', '#include "freertos/task.h"'):
        raw = raw.replace(include, "")
    protocol = (ROOT / "main/pet_p4_protocol.c").read_text()
    start = protocol.index("typedef struct {char path[PET_P4_ASSET_PATH_MAX];uint32_t size;} asset_query_video_t;")
    end = protocol.index("static void send_asset_slot_query_ack(", start)
    helpers = '#include "pet_p4_assets.h"\n#include "cJSON.h"\n#define EXT_RAM_BSS_ATTR\n'
    helpers += function(protocol, "copy_text") + "\n" + protocol[start:end]
    fixture = (TESTS / "p4_raw_asset_cache_test.c").read_text()
    fixture = fixture.replace("/* RAW_PRODUCTION */", raw)
    assert fixture.count("int main(void) {") == 1
    fixture = fixture.replace("int main(void) {", helpers + "\nint main(void) {\n" +
                              (TESTS / "p4_audio_patch_video_cases.inc").read_text())
    compile_run(tmp_path, fixture, faults=False, pthread=True)
    query = function(protocol, "send_asset_slot_query_ack")
    assert query.index("asset_query_snapshot_videos") < query.index("if(!g_asset_query_io_begin(g_asset_query_io_context))")
    assert query.index("if(!g_asset_query_io_begin(g_asset_query_io_context))") < query.index("asset_query_video_proof") < query.index("g_asset_query_io_end(g_asset_query_io_context);")
    assert 'cJSON_AddItemToObject(payload,"videos",videos)' in query
    assert "state->asset_revision==revision" in query and "pet_resources_device_current(lease)" in query


def test_actual_global_audio_phases_survive_appearance_changes(tmp_path):
    source=(ROOT/"main/pet_p4_protocol.c").read_text()
    production="\n".join(function(source,n) for n in FUNCTIONS)
    production+='\n#include "pet_p4_protocol_audio_patch.inc"\n'
    fixture=(TESTS/"p4_audio_patch_protocol_test.c.in").read_text()
    fixture=fixture[:fixture.index('int main(void)')]+r'''
int main(void){
 strcpy(state.board_device_id,"fixture-board");
 cJSON *begin=begin_request();cJSON_AddStringToObject(begin,"scope","global");dispatch("asset/audio-begin",begin);
 assert(cJSON_IsTrue(cJSON_GetObjectItem(last_ack,"ok")));
 cJSON *chunk=request();cJSON_AddStringToObject(chunk,"scope","global");cJSON_AddStringToObject(chunk,"path","p4/audio/done.wav");
 cJSON_AddNumberToObject(chunk,"index",0);cJSON_AddNumberToObject(chunk,"offset",0);
 cJSON_AddStringToObject(chunk,"data","UklGRiYAAABXQVZFZm10IBAAAAABAAEAgD4AAAB9AAACABAAZGF0YQIAAAABAA==");dispatch("asset/audio-chunk",chunk);
 assert(cJSON_IsTrue(cJSON_GetObjectItem(last_ack,"ok")));
 cJSON *commit=request();cJSON_AddStringToObject(commit,"scope","global");dispatch("asset/audio-commit",commit);
 assert(cJSON_IsTrue(cJSON_GetObjectItem(last_ack,"ok"))&&!audio_patch_protocol_active());
 char first[64],second[64];assert(pet_p4_asset_audio_override_path("done",first,sizeof(first)));
 unsigned reads=ready_reads;atomic_store(&g_asset_active_slot,1);
 assert(pet_p4_asset_audio_override_path("done",second,sizeof(second))&&!strcmp(first,second));
 assert(ready_reads==reads&&strstr(second,"ov2_"));
 // Scope changes are rejected, never reinterpreted as a different storage namespace.
 atomic_store(&g_asset_active_slot,0);begin=begin_request();cJSON_AddStringToObject(begin,"scope","global");dispatch("asset/audio-begin",begin);
 dispatch("asset/audio-commit",request());assert(cJSON_IsFalse(cJSON_GetObjectItem(last_ack,"ok"))&&audio_patch_protocol_active());
 cJSON *abort=request();cJSON_AddStringToObject(abort,"scope","global");dispatch("asset/audio-abort",abort);
 assert(cJSON_IsTrue(cJSON_GetObjectItem(last_ack,"ok")));assert(pet_p4_asset_audio_override_path("done",second,sizeof(second))&&!strcmp(first,second));
 audio_patch_timeout_worker(&state);cJSON_Delete(last_ack);return 0;
}
'''
    compile_run(tmp_path,fixture.replace("/* PRODUCTION_PROTOCOL */",production),["main/pet_p4_audio_patch.c","main/pet_p4_resources.c"])
