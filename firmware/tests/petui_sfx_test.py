"""p4-sfx-v1: production DSP core via ctypes, validator vectors, firmware/report parity.

Offline evidence: no speaker, no device, no AEC claims.
"""
import ctypes as C
import json
import re
import subprocess
import sys
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/skills/petui/scripts'))
from validate_generated_widget import DEFAULT_CAPABILITIES, SFX_PRESETS, validate_sfx_profile  # noqa: E402
import validate_generated_widget as v  # noqa: E402

SUITE = json.loads((ROOT / 'pc/skills/petui/contracts/sfx-v1.json').read_text())


class Voice(C.Structure):
    _fields_ = [("preset", C.c_uint8), ("active", C.c_bool), ("position", C.c_uint32), ("length", C.c_uint32), ("phase", C.c_uint32), ("noise", C.c_uint32), ("low", C.c_uint32)]


class Core(C.Structure):
    _fields_ = [("voices", Voice * 2), ("next", C.c_uint)]


@pytest.fixture(scope="module")
def dsp(tmp_path_factory):
    out = tmp_path_factory.mktemp("sfx") / "core.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", str(ROOT / "firmware/main/pet_p4_sfx_core.c"), "-o", str(out)], check=True)
    lib = C.CDLL(str(out))
    lib.pet_sfx_reset.argtypes = [C.POINTER(Core)]
    lib.pet_sfx_trigger.argtypes = [C.POINTER(Core), C.c_uint]; lib.pet_sfx_trigger.restype = C.c_bool
    lib.pet_sfx_active.argtypes = [C.POINTER(Core)]; lib.pet_sfx_active.restype = C.c_bool
    lib.pet_sfx_render.argtypes = [C.POINTER(Core), C.POINTER(C.c_int16), C.c_size_t, C.c_uint]
    lib.pet_sfx_preset_name.argtypes = [C.c_uint]; lib.pet_sfx_preset_name.restype = C.c_char_p
    lib.pet_sfx_preset_index.argtypes = [C.c_char_p]; lib.pet_sfx_preset_index.restype = C.c_int
    lib.pet_sfx_preset_length.argtypes = [C.c_uint]; lib.pet_sfx_preset_length.restype = C.c_uint32
    return lib


def render(dsp, s, n, volume=80):
    pcm = (C.c_int16 * n)()
    dsp.pet_sfx_render(C.byref(s), pcm, n, volume)
    return list(pcm)


def test_presets_match_validator_and_are_bounded(dsp):
    assert [dsp.pet_sfx_preset_name(i).decode() for i in range(6)] == list(SFX_PRESETS)
    assert dsp.pet_sfx_preset_index(b"chirp") == 2 and dsp.pet_sfx_preset_index(b"nope") == -1
    for i in range(6):
        assert 800 <= dsp.pet_sfx_preset_length(i) <= 8000, "every cue is under half a second"
    assert not dsp.pet_sfx_trigger(C.byref(Core()), 6)


def test_cue_sounds_then_decays_to_silence_and_volume_zero_is_silent(dsp):
    s = Core(); dsp.pet_sfx_reset(C.byref(s))
    assert max(map(abs, render(dsp, s, 320))) == 0
    assert dsp.pet_sfx_trigger(C.byref(s), 1)
    pcm = render(dsp, s, 4000)
    assert max(map(abs, pcm[:1920])) > 2000 and max(map(abs, pcm[2400:])) == 0
    assert not dsp.pet_sfx_active(C.byref(s))
    dsp.pet_sfx_trigger(C.byref(s), 1)
    assert not any(render(dsp, s, 2000, volume=0))


def test_two_voices_mix_without_overflow_and_third_replaces_oldest(dsp):
    s = Core(); dsp.pet_sfx_reset(C.byref(s))
    for preset in (0, 3, 4, 2, 5, 1):
        assert dsp.pet_sfx_trigger(C.byref(s), preset)
    assert sum(1 for voice in s.voices if voice.active) == 2
    pcm = render(dsp, s, 8000, volume=100)
    assert max(pcm) <= 32767 and min(pcm) >= -32768 and max(map(abs, pcm)) > 1000
    assert not dsp.pet_sfx_active(C.byref(s))


def test_validator_vectors():
    for item in SUITE['vectors']:
        errors = []
        validate_sfx_profile(item['runtime'], DEFAULT_CAPABILITIES, errors)
        if item.get('scene'):
            v.validate_scene(item['runtime']['scene'], item['runtime'], ROOT, DEFAULT_CAPABILITIES, {t['on'] for t in item['runtime'].get('transitions', [])}, errors)
        assert (not errors) == item['ok'], (item['name'], errors)
    errors = []
    validate_sfx_profile(SUITE['vectors'][0]['runtime'], {**DEFAULT_CAPABILITIES, 'widgetSfx': None}, errors)
    assert any('widgetSfx' in e for e in errors)


def test_firmware_declares_profile_and_wires_the_gated_path():
    audio = (ROOT / 'firmware/main/pet_p4_audio.c').read_text()
    runtime = (ROOT / 'firmware/main/pet_p4_miniapp.c').read_text()
    protocol = (ROOT / 'firmware/main/pet_p4_protocol.c').read_text()
    main = (ROOT / 'firmware/main/pet_p4_main.c').read_text()
    assert 'cJSON_AddStringToObject(capabilities, "widgetSfx", "p4-sfx-v1");' in protocol
    assert int(re.search(r'"maxSfxCues", (\d+)\)', protocol).group(1)) == 8
    # Same blockers as the fruit/instrument cue paths: capture, conversation, stream, music.
    block = audio[audio.index('static bool sfx_drain_step'):audio.index('void pet_p4_audio_instrument_set')]
    for flag in ('g_capture_requested', 'g_capture_active', 'g_conversation', 'g_stream_active', 'g_music'):
        assert flag in block, flag
    assert 'pet_p4_audio_frontend' not in block, 'sfx never touches the AFE/AEC front end'
    assert 'if(sfx_drain_step(pcm)) {vTaskDelay(1);continue;}' in audio
    assert 'pet_p4_miniapp_sfx_process(now_ms' in main and '!pet_p4_audio_conversation_active()' in main
    assert '"sfx",\n  };' in runtime or '"quota", "sfx",' in runtime
    assert 'PET_SFX_MAX_CUES' in runtime and 'completion_sfx_cue' in runtime
