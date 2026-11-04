"""Track C capability expansion (p4-sequence-v1, p4-samples-v1, p4-clip-v1, p4-big-image-v1,
p4-events-v1): production parser/DSP/event cores via cc + ctypes, shared contract vectors,
flag wiring and hook presence. Offline evidence only: no speaker, device or decoder."""
import ctypes as C
import json
import re
import struct
import subprocess
import sys
import time
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / 'firmware'
sys.path.insert(0, str(ROOT / 'pc/skills/petui/scripts'))
from validate_generated_widget import DEFAULT_CAPABILITIES  # noqa: E402
from capability_ext import validate_capability_ext, strip, big_tiles  # noqa: E402
from app_surface import app_surface_errors  # noqa: E402

SUITES = {name: json.loads((ROOT / f'pc/skills/petui/contracts/{name}.json').read_text()) for name in ('sequence-v1', 'samples-v1', 'clip-v1', 'big-image-v1', 'events-v1')}
CC = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-error=deprecated-declarations', '-I', str(FW / 'main'), '-I', str(FW / 'tests/vendor/cjson')]
SOURCES = [str(FW / 'main' / f) for f in ('pet_p4_component_ext.c', 'pet_p4_component_events.c', 'pet_p4_sfx_core.c')]


@pytest.fixture(scope='module')
def harness(tmp_path_factory):
    binary = tmp_path_factory.mktemp('cext') / 'harness'
    subprocess.run([*CC, '-fsanitize=address,undefined', str(FW / 'tests/p4_capability_ext_test.c'), *SOURCES, str(FW / 'tests/vendor/cjson/cJSON.c'), '-lm', '-o', str(binary)], check=True)
    def run(runtime):
        result = subprocess.run([str(binary)], input=json.dumps(runtime), text=True, capture_output=True)
        assert result.returncode == 0 and not result.stderr, result.stderr
        return json.loads(result.stdout)
    return run


class SeqTrack(C.Structure):
    _fields_ = [('wave', C.c_uint8), ('volume', C.c_uint8), ('count', C.c_uint8), ('midi', C.c_uint8 * 64), ('beats', C.c_uint8 * 64)]


class SeqProgram(C.Structure):
    _fields_ = [('bpm', C.c_uint16), ('track_count', C.c_uint8), ('loop', C.c_bool), ('tracks', SeqTrack * 2)]


class SeqVoice(C.Structure):
    _fields_ = [('index', C.c_uint8), ('done', C.c_bool), ('position', C.c_uint32), ('length', C.c_uint32), ('phase', C.c_uint32), ('step', C.c_uint32), ('noise', C.c_uint32)]


class SeqState(C.Structure):
    _fields_ = [('playing', C.c_bool), ('voice', SeqVoice * 2)]


class SampleVoice(C.Structure):
    _fields_ = [('pcm', C.POINTER(C.c_int16)), ('frames', C.c_uint32), ('position', C.c_uint32), ('active', C.c_bool)]


class Sampler(C.Structure):
    _fields_ = [('voice', SampleVoice * 2), ('next', C.c_uint)]


class Component(C.Structure):
    _fields_ = [('widget_id', C.c_char * 48), ('names', (C.c_char * 16) * 8), ('name_count', C.c_uint8), ('queue', (C.c_char * 16) * 8), ('head', C.c_uint8), ('count', C.c_uint8), ('dropped', C.c_uint32), ('used', C.c_bool)]


class Bus(C.Structure):
    _fields_ = [('items', Component * 32), ('emitted', C.c_uint32), ('delivered', C.c_uint32), ('dropped', C.c_uint32)]


class Alarm(C.Structure):
    _fields_ = [('id', C.c_char * 16), ('action', C.c_char * 48), ('text', C.c_char * 41), ('every_ms', C.c_uint32), ('at_var', C.c_int8), ('last_var', C.c_int32), ('deadline_ms', C.c_uint64), ('background', C.c_bool), ('armed', C.c_bool), ('pending', C.c_bool), ('utc_deadline_ms', C.c_uint64), ('waiting_ms', C.c_uint32), ('utc_deadline', C.c_bool), ('waiting_for_clock', C.c_bool), ('cancelled', C.c_bool)]


class AlarmSet(C.Structure):
    _fields_ = [('items', Alarm * 4), ('count', C.c_uint8), ('clock_changes', C.c_uint32)]


class Big(C.Structure):
    _fields_ = [('id', C.c_char * 16), ('w', C.c_uint16), ('h', C.c_uint16), ('tile_w', C.c_uint16), ('tile_h', C.c_uint16), ('cols', C.c_uint8), ('rows', C.c_uint8), ('bytes', C.c_uint32)]


class Clip(C.Structure):
    _fields_ = [('id', C.c_char * 16), ('w', C.c_uint16), ('h', C.c_uint16), ('frames', C.c_uint16), ('fps', C.c_uint8), ('bytes', C.c_uint32)]


class ClipState(C.Structure):
    _fields_ = [('visible', C.c_bool), ('started_ms', C.c_uint64), ('hidden_ms', C.c_uint64), ('presented', C.c_int16)]


@pytest.fixture(scope='module')
def core(tmp_path_factory):
    out = tmp_path_factory.mktemp('cext-core') / 'core.so'
    subprocess.run([*CC, '-shared', '-fPIC', '-O2', *SOURCES, str(FW / 'tests/vendor/cjson/cJSON.c'), '-o', str(out)], check=True)
    lib = C.CDLL(str(out))
    lib.pet_seq_program_valid.argtypes = [C.POINTER(SeqProgram)]; lib.pet_seq_program_valid.restype = C.c_bool
    lib.pet_seq_start.argtypes = [C.POINTER(SeqState), C.POINTER(SeqProgram)]
    lib.pet_seq_stop.argtypes = [C.POINTER(SeqState)]
    lib.pet_seq_active.argtypes = [C.POINTER(SeqState)]; lib.pet_seq_active.restype = C.c_bool
    lib.pet_seq_mix.argtypes = [C.POINTER(SeqState), C.POINTER(SeqProgram), C.POINTER(C.c_int16), C.c_size_t, C.c_uint]
    lib.pet_seq_note_hz_x16.argtypes = [C.c_uint]; lib.pet_seq_note_hz_x16.restype = C.c_uint32
    lib.pet_seq_samples_per_beat.argtypes = [C.c_uint]; lib.pet_seq_samples_per_beat.restype = C.c_uint32
    lib.pet_sampler_reset.argtypes = [C.POINTER(Sampler)]
    lib.pet_sampler_trigger.argtypes = [C.POINTER(Sampler), C.POINTER(C.c_int16), C.c_uint32]; lib.pet_sampler_trigger.restype = C.c_bool
    lib.pet_sampler_active.argtypes = [C.POINTER(Sampler)]; lib.pet_sampler_active.restype = C.c_bool
    lib.pet_sampler_mix.argtypes = [C.POINTER(Sampler), C.POINTER(C.c_int16), C.c_size_t, C.c_uint]
    lib.pet_cev_name_valid.argtypes = [C.c_char_p]; lib.pet_cev_name_valid.restype = C.c_bool
    lib.pet_cev_bus_reset.argtypes = [C.POINTER(Bus)]
    lib.pet_cev_bus_subscribe.argtypes = [C.POINTER(Bus), C.c_char_p, C.c_void_p, C.c_uint]; lib.pet_cev_bus_subscribe.restype = C.c_bool
    lib.pet_cev_bus_unsubscribe.argtypes = [C.POINTER(Bus), C.c_char_p]
    lib.pet_cev_bus_emit.argtypes = [C.POINTER(Bus), C.c_char_p]; lib.pet_cev_bus_emit.restype = C.c_uint
    lib.pet_cev_bus_pop.argtypes = [C.POINTER(Bus), C.c_char_p, C.c_char_p]; lib.pet_cev_bus_pop.restype = C.c_bool
    lib.pet_cev_bus_pending.argtypes = [C.POINTER(Bus), C.c_char_p]; lib.pet_cev_bus_pending.restype = C.c_uint
    lib.pet_cev_bus_dropped.argtypes = [C.POINTER(Bus), C.c_char_p]; lib.pet_cev_bus_dropped.restype = C.c_uint32
    lib.pet_cev_alarms_start.argtypes = [C.POINTER(AlarmSet), C.c_uint64]
    lib.pet_cev_alarms_observe_var.argtypes = [C.POINTER(AlarmSet), C.c_uint, C.c_int32, C.c_uint64]
    lib.pet_cev_alarms_tick.argtypes = [C.POINTER(AlarmSet), C.c_uint64]; lib.pet_cev_alarms_tick.restype = C.c_uint
    lib.pet_cev_alarms_take_pending.argtypes = [C.POINTER(AlarmSet)]; lib.pet_cev_alarms_take_pending.restype = C.c_uint
    lib.pet_cev_alarms_serialize.argtypes = [C.POINTER(AlarmSet), C.c_uint64, C.c_char_p, C.c_size_t]; lib.pet_cev_alarms_serialize.restype = C.c_size_t
    lib.pet_cev_alarms_restore.argtypes = [C.POINTER(AlarmSet), C.c_char_p, C.c_uint64]; lib.pet_cev_alarms_restore.restype = C.c_bool
    lib.pet_cext_media_path_kind.argtypes = [C.c_char_p, C.c_char_p]; lib.pet_cext_media_path_kind.restype = C.c_int
    lib.pet_cext_sample_header.argtypes = [C.c_char_p, C.c_size_t, C.c_void_p]; lib.pet_cext_sample_header.restype = C.c_bool
    lib.pet_cext_clip_header.argtypes = [C.c_char_p, C.c_size_t, C.POINTER(Clip)]; lib.pet_cext_clip_header.restype = C.c_bool
    lib.pet_cext_big_header.argtypes = [C.c_char_p, C.c_size_t, C.POINTER(Big)]; lib.pet_cext_big_header.restype = C.c_bool
    lib.pet_cext_big_visible_tiles.argtypes = [C.POINTER(Big), C.c_int32, C.c_int32, C.c_uint, C.c_uint, C.POINTER(C.c_uint8)]; lib.pet_cext_big_visible_tiles.restype = C.c_uint
    lib.pet_cext_clip_target.argtypes = [C.POINTER(ClipState), C.POINTER(Clip), C.c_bool, C.c_uint64]; lib.pet_cext_clip_target.restype = C.c_int
    return lib


def program(bpm=120, loop=True, tracks=(((60, 2), (0, 1), (64, 1)),), wave=0, volume=80):
    p = SeqProgram(); p.bpm = bpm; p.loop = loop; p.track_count = len(tracks)
    for i, notes in enumerate(tracks):
        p.tracks[i].wave = wave if isinstance(wave, int) else wave[i]; p.tracks[i].volume = volume; p.tracks[i].count = len(notes)
        for j, (m, b) in enumerate(notes): p.tracks[i].midi[j] = m; p.tracks[i].beats[j] = b
    return p


def mix(core, state, prog, n=320, volume=100):
    pcm = (C.c_int16 * n)(); core.pet_seq_mix(C.byref(state), C.byref(prog), pcm, n, volume); return list(pcm)


# ---- shared vectors -----------------------------------------------------------------------
@pytest.mark.parametrize('name', sorted(SUITES))
def test_contract_vectors_match_firmware_parser_and_python_validator(harness, name):
    for item in SUITES[name]['vectors']:
        errors = []
        validate_capability_ext(item['runtime'], DEFAULT_CAPABILITIES, errors)
        stripped = strip(item['runtime'])
        if isinstance(stripped.get('surface'), dict) and stripped['surface'].get('engine') == 'p4-app-surface-v1':
            errors += app_surface_errors(stripped['surface'], stripped)
        assert (not errors) == item['ok'], (name, item['name'], errors)
        if item.get('firmware') != 'surface':
            result = harness(item['runtime'])
            assert result['ok'] == item['ok'], (name, item['name'], result)
    key = SUITES[name]['vectors'][0]['runtime']
    missing = {k: None for k in ('widgetSequence', 'widgetSamples', 'widgetClip', 'widgetBigImage', 'widgetEvents')}
    errors = []
    validate_capability_ext(key, {**DEFAULT_CAPABILITIES, **missing}, errors)
    assert any('请先升级固件' in e for e in errors), name


def test_firmware_parser_records_declarations(harness):
    ok = SUITES['events-v1']['vectors'][1]['runtime']
    result = harness(ok)
    assert result['ok'] and result['emits'] == 2 and result['alarms'] == 2
    ok = SUITES['events-v1']['vectors'][0]['runtime']
    result = harness(ok)
    assert result['subs'] == 2 and result['emit_pending'] == 1, result
    big = harness(SUITES['big-image-v1']['vectors'][0]['runtime'])
    assert big['bigs'] == 1 and harness(SUITES['clip-v1']['vectors'][0]['runtime'])['clips'] == 1


# ---- sequencer DSP ------------------------------------------------------------------------
def test_sequencer_bounds_table_and_tempo(core):
    assert core.pet_seq_note_hz_x16(69) == 7040 and core.pet_seq_note_hz_x16(60) == 4186
    assert core.pet_seq_note_hz_x16(35) == 0 and core.pet_seq_note_hz_x16(97) == 0
    assert core.pet_seq_samples_per_beat(120) == 8000 and core.pet_seq_samples_per_beat(240) == 4000
    assert core.pet_seq_program_valid(C.byref(program()))
    for bad in (program(bpm=39), program(bpm=241), program(tracks=(((97, 1),),)), program(tracks=(((60, 17),),)), program(wave=3)):
        assert not core.pet_seq_program_valid(C.byref(bad))
    empty = program(); empty.track_count = 0
    assert not core.pet_seq_program_valid(C.byref(empty))


def test_sequencer_plays_rests_loops_and_stops(core):
    prog = program(bpm=240, loop=False, tracks=(((60, 1), (0, 1)),))
    state = SeqState(); core.pet_seq_start(C.byref(state), C.byref(prog))
    assert core.pet_seq_active(C.byref(state))
    note = mix(core, state, prog, 4000)
    assert max(map(abs, note)) > 1000
    rest = mix(core, state, prog, 4000)
    assert max(map(abs, rest)) == 0, 'midi 0 is a rest'
    assert not core.pet_seq_active(C.byref(state)), 'a non-looping program ends'
    looped = program(bpm=240, loop=True, tracks=(((60, 1), (0, 1)),))
    core.pet_seq_start(C.byref(state), C.byref(looped))
    for _ in range(10): mix(core, state, looped, 4000)
    assert core.pet_seq_active(C.byref(state)) and max(map(abs, mix(core, state, looped, 4000))) > 1000
    core.pet_seq_stop(C.byref(state))
    assert not core.pet_seq_active(C.byref(state)) and max(map(abs, mix(core, state, looped, 320))) == 0
    core.pet_seq_start(C.byref(state), C.byref(looped))
    assert max(map(abs, mix(core, state, looped, 1000, volume=0))) == 0, 'volume 0 is silent'


def test_sequencer_mixes_additively_without_overflow_and_within_one_millisecond_per_block(core):
    prog = program(bpm=120, loop=True, tracks=(((48, 4),), ((84, 4),)), wave=(0, 2), volume=100)
    state = SeqState(); core.pet_seq_start(C.byref(state), C.byref(prog))
    pcm = (C.c_int16 * 320)(*([30000] * 320))
    core.pet_seq_mix(C.byref(state), C.byref(prog), pcm, 320, 100)
    assert max(pcm) <= 32767 and min(pcm) >= -32768 and max(pcm) > 30000, 'additive on top of existing sfx samples, clamped'
    blocks = 2000
    started = time.perf_counter()
    for _ in range(blocks): core.pet_seq_mix(C.byref(state), C.byref(prog), pcm, 320, 100)
    per_block_ms = (time.perf_counter() - started) * 1000 / blocks
    assert per_block_ms < 1.0, f'{per_block_ms:.3f} ms per 20 ms block on the host'


# ---- sampler ------------------------------------------------------------------------------
def test_sampler_one_shots_two_voices_replace_oldest(core):
    s = Sampler(); core.pet_sampler_reset(C.byref(s))
    a = (C.c_int16 * 100)(*([8000] * 100)); b = (C.c_int16 * 50)(*([-8000] * 50)); c = (C.c_int16 * 10)(*([100] * 10))
    assert not core.pet_sampler_trigger(C.byref(s), a, 0)
    assert core.pet_sampler_trigger(C.byref(s), a, 100) and core.pet_sampler_trigger(C.byref(s), b, 50)
    pcm = (C.c_int16 * 60)(); core.pet_sampler_mix(C.byref(s), pcm, 60, 100)
    assert pcm[0] == 0 and pcm[59] == 8000, 'two voices sum; the shorter one ends'
    assert core.pet_sampler_trigger(C.byref(s), c, 10) and core.pet_sampler_trigger(C.byref(s), c, 10)
    assert sum(1 for v in s.voice if v.active) == 2, 'third trigger replaced the oldest voice'
    pcm = (C.c_int16 * 200)(); core.pet_sampler_mix(C.byref(s), pcm, 200, 50)
    assert not core.pet_sampler_active(C.byref(s)) and pcm[0] == 100 and pcm[150] == 0
    loud = (C.c_int16 * 10)(*([32000] * 10)); core.pet_sampler_trigger(C.byref(s), loud, 10); core.pet_sampler_trigger(C.byref(s), loud, 10)
    pcm = (C.c_int16 * 10)(); core.pet_sampler_mix(C.byref(s), pcm, 10, 100)
    assert max(pcm) == 32767, 'clamped'


# ---- event bus ----------------------------------------------------------------------------
def names(*values):
    arr = ((C.c_char * 16) * 8)()
    for i, v in enumerate(values): arr[i].value = v.encode()
    return arr


def test_event_bus_bounds_fanout_queue_drop_and_hidden_delivery(core):
    bus = Bus(); core.pet_cev_bus_reset(C.byref(bus))
    assert core.pet_cev_name_valid(b'done') and not core.pet_cev_name_valid(b'a:b') and not core.pet_cev_name_valid(b'x' * 16)
    for i in range(32):
        assert core.pet_cev_bus_subscribe(C.byref(bus), f'c{i}'.encode(), names('done'), 1)
    assert not core.pet_cev_bus_subscribe(C.byref(bus), b'c32', names('done'), 1), '33rd component is refused'
    assert not core.pet_cev_bus_subscribe(C.byref(bus), b'c0', names(*[f'n{i}' for i in range(8)]), 9), 'nine names are refused'
    assert core.pet_cev_bus_emit(C.byref(bus), b'done') == 32 and core.pet_cev_bus_emit(C.byref(bus), b'other') == 0
    assert core.pet_cev_bus_emit(C.byref(bus), b'a:b') == 0
    out = C.create_string_buffer(16)
    assert core.pet_cev_bus_pop(C.byref(bus), b'c5', out) and out.value == b'done' and not core.pet_cev_bus_pop(C.byref(bus), b'c5', out)
    for _ in range(9): core.pet_cev_bus_emit(C.byref(bus), b'done')
    assert core.pet_cev_bus_pending(C.byref(bus), b'c7') == 8 and core.pet_cev_bus_dropped(C.byref(bus), b'c7') == 2, 'queue of 8, drops counted (hidden component keeps its queue)'
    core.pet_cev_bus_unsubscribe(C.byref(bus), b'c7')
    assert core.pet_cev_bus_pending(C.byref(bus), b'c7') == 0
    assert core.pet_cev_bus_subscribe(C.byref(bus), b'c7', names('ping'), 1), 'slot freed'
    # re-registration keeps the queue of a still-installed component
    assert core.pet_cev_bus_subscribe(C.byref(bus), b'c6', names('done', 'ping'), 2) and core.pet_cev_bus_pending(C.byref(bus), b'c6') == 8
    assert bus.dropped >= 2 and bus.emitted == 11, 'invalid names never count as emitted'


# ---- alarms -------------------------------------------------------------------------------
def alarm_set():
    s = AlarmSet(); s.count = 2
    s.items[0].id = b'water'; s.items[0].action = b'remind'; s.items[0].text = b'drink'; s.items[0].every_ms = 60000; s.items[0].at_var = -1; s.items[0].background = True
    s.items[1].id = b'timer'; s.items[1].action = b'ring'; s.items[1].text = b'timer'; s.items[1].every_ms = 0; s.items[1].at_var = 0
    return s


def test_alarms_periodic_oneshot_and_persistence_across_restart(core):
    s = alarm_set()
    core.pet_cev_alarms_start(C.byref(s), 1000)
    assert s.items[0].armed and s.items[0].deadline_ms == 61000 and not s.items[1].armed
    assert core.pet_cev_alarms_tick(C.byref(s), 60999) == 0 and core.pet_cev_alarms_tick(C.byref(s), 61000) == 1
    assert s.items[0].armed and s.items[0].deadline_ms == 121000, 'periodic alarms re-arm'
    core.pet_cev_alarms_observe_var(C.byref(s), 1, 59999, 2000)
    assert not s.items[1].armed, 'below the one-minute floor is ignored'
    core.pet_cev_alarms_observe_var(C.byref(s), 1, 300000, 2000)
    assert s.items[1].armed and s.items[1].deadline_ms == 302000
    core.pet_cev_alarms_observe_var(C.byref(s), 1, 300000, 5000)
    assert s.items[1].deadline_ms == 302000, 'an unchanged var does not re-arm'
    assert core.pet_cev_alarms_take_pending(C.byref(s)) == 1
    # simulated restart at uptime 100000: remaining time is what persists, not the wall clock
    text = C.create_string_buffer(160)
    assert core.pet_cev_alarms_serialize(C.byref(s), 100000, text, 160) > 0
    saved = text.value.decode()
    assert saved == 'water:21000;timer:202000:v300000', saved
    fresh = alarm_set()
    assert core.pet_cev_alarms_restore(C.byref(fresh), saved.encode(), 500)
    assert fresh.items[0].deadline_ms == 21500 and fresh.items[1].deadline_ms == 202500 and fresh.items[1].armed
    core.pet_cev_alarms_start(C.byref(fresh), 500)
    assert fresh.items[0].deadline_ms == 21500, 'start does not re-arm a restored periodic alarm'
    assert core.pet_cev_alarms_tick(C.byref(fresh), 202500) == 3 and not fresh.items[1].armed
    assert core.pet_cev_alarms_take_pending(C.byref(fresh)) == 3
    assert core.pet_cev_alarms_restore(C.byref(fresh), b'', 0) and not core.pet_cev_alarms_restore(C.byref(fresh), b'bad', 0)
    assert not core.pet_cev_alarms_restore(C.byref(fresh), b'water:x', 0) and core.pet_cev_alarms_restore(C.byref(fresh), b'unknown:1000', 0)
    pending = alarm_set(); pending.items[0].pending = True
    core.pet_cev_alarms_serialize(C.byref(pending), 0, text, 160)
    assert text.value == b'water:0:p;timer:0:v0'
    core.pet_cev_alarms_observe_var(C.byref(s), 1, 0, 6000)
    assert not s.items[1].armed, 'zero disarms'


# ---- media containers, tiles, clip clock ---------------------------------------------------
def p4i1(w=240, h=240):
    header = b'P4I1' + bytes([w, h, 1, 0]) + struct.pack('<I', 0)
    palette = bytes([10, 20, 30, 255] * 256)
    body = b''
    remaining = w * h
    while remaining:
        run = min(128, remaining); body += bytes([128 | (run - 1), 0]); remaining -= run
    data = header + palette + body
    return data[:8] + struct.pack('<I', len(data)) + data[12:]


def p4t1(w, h):
    cols, rows = -(-w // 240), -(-h // 240)
    tiles = [p4i1(240, 240)] * (cols * rows)
    offsets = []; at = 20 + 8 * len(tiles)
    for t in tiles: offsets.append((at, len(t))); at += len(t)
    head = b'P4T1' + bytes([cols, rows]) + struct.pack('<HHHHBBI', 240, 240, w, h, len(tiles), 0, 0)
    return head + b''.join(struct.pack('<II', o, n) for o, n in offsets) + b''.join(tiles)


def test_media_paths_headers_tiles_and_clip_clock(core):
    buf = C.create_string_buffer(16)
    assert core.pet_cext_media_path_kind(b'runtime/samples/ding.pcm', buf) == 1 and buf.value == b'ding'
    assert core.pet_cext_media_path_kind(b'runtime/clips/intro.h264', buf) == 2 and core.pet_cext_media_path_kind(b'runtime/big/map.p4t', buf) == 3
    for bad in (b'runtime/samples/ding.wav', b'runtime/samples/abcdefghijklmnop.pcm', b'assets/ding.wav', b'runtime/big/../x.p4t'):
        assert core.pet_cext_media_path_kind(bad, buf) == 0, bad
    sample = b'P4A1' + struct.pack('<I', 8000) + bytes(16000)
    assert core.pet_cext_sample_header(sample, len(sample), None) and not core.pet_cext_sample_header(sample, len(sample) - 2, None)
    too_big = b'P4A1' + struct.pack('<I', 32769) + bytes(65538)
    assert not core.pet_cext_sample_header(too_big, len(too_big), None)
    clip = Clip(); stream = b'P4V1' + struct.pack('<HHBBHI', 320, 240, 15, 0, 75, 100) + bytes(100)
    assert core.pet_cext_clip_header(stream, len(stream), C.byref(clip)) and clip.frames == 75
    assert not core.pet_cext_clip_header(b'P4V1' + struct.pack('<HHBBHI', 640, 480, 15, 0, 10, 4) + bytes(4), 20, C.byref(clip))
    assert not core.pet_cext_clip_header(b'P4V1' + struct.pack('<HHBBHI', 320, 240, 15, 0, 76, 4) + bytes(4), 20, C.byref(clip))
    big = Big(); data = p4t1(960, 720)
    assert core.pet_cext_big_header(data, len(data), C.byref(big)) and (big.cols, big.rows) == (4, 3) and big_tiles(960, 720) == 12
    assert not core.pet_cext_big_header(p4t1(1280, 960), len(p4t1(1280, 960)), C.byref(big)), '24 tiles exceed 16'
    tiles = (C.c_uint8 * 16)()
    assert core.pet_cext_big_visible_tiles(C.byref(big), 0, 0, 640, 480, tiles) == 6 and list(tiles[:6]) == [0, 1, 2, 4, 5, 6]
    assert core.pet_cext_big_visible_tiles(C.byref(big), 10000, 10000, 640, 480, tiles) == 6 and list(tiles[:6]) == [5, 6, 7, 9, 10, 11], 'offsets clamp to the image (320,240)'
    assert core.pet_cext_big_visible_tiles(C.byref(big), 300, 250, 200, 200, tiles) == 2 and list(tiles[:2]) == [5, 6]
    assert core.pet_cext_big_visible_tiles(C.byref(big), 300, 250, 200, 240, tiles) == 4 and list(tiles[:4]) == [5, 6, 9, 10]
    assert core.pet_cext_big_visible_tiles(C.byref(big), -50, -50, 100, 100, tiles) == 1 and tiles[0] == 0
    state = ClipState(); clip.frames = 75; clip.fps = 15
    assert core.pet_cext_clip_target(C.byref(state), C.byref(clip), False, 1000) == -1
    assert core.pet_cext_clip_target(C.byref(state), C.byref(clip), True, 1000) == 0
    assert core.pet_cext_clip_target(C.byref(state), C.byref(clip), True, 2000) == 15
    assert core.pet_cext_clip_target(C.byref(state), C.byref(clip), False, 2500) == -1, 'hidden pauses'
    assert core.pet_cext_clip_target(C.byref(state), C.byref(clip), True, 9000) == 22, 'resumes at the frame shown when hidden (2500 ms -> 22); hidden time never advances the clip'
    assert core.pet_cext_clip_target(C.byref(state), C.byref(clip), True, 9000 + 6000) == (22 + 90) % 75, 'loops'


# ---- wiring: flags, hello, hooks, env -----------------------------------------------------
def test_flags_default_off_and_capability_env_enables_them():
    features = (FW / 'main/pet_p4_component_features.h').read_text()
    for flag in ('PET_P4_SEQUENCE', 'PET_P4_SAMPLES', 'PET_P4_CLIP', 'PET_P4_BIG_IMAGE', 'PET_P4_EVENTS'):
        assert re.search(rf'#ifndef {flag}[^\n]*\n#define {flag} 0\n#endif', features), flag
    assert '#if PET_P4_CLIP && !(defined(PET_P4_H264_WORKER_PACK) && PET_P4_H264_WORKER_PACK)' in features
    ini = (FW / 'platformio.ini').read_text()
    env = ini[ini.index('[env:esp32_p4_capability_parent]'):]
    env = env[:env.index('\n[env:', 1)]
    assert 'custom_p4_project_version = 0.7.103-p4-r150-capability' in env
    for flag in ('SEQUENCE', 'SAMPLES', 'CLIP', 'BIG_IMAGE', 'EVENTS'):
        assert f'  -DPET_P4_{flag}=1' in env, flag
    assert (FW / 'sdkconfig.esp32_p4_capability_parent').read_bytes() == (FW / 'sdkconfig.esp32_p4_main_frame_parent').read_bytes()
    cmake = (FW / 'main/CMakeLists.txt').read_text()
    for src in ('pet_p4_component_events.c', 'pet_p4_component_ext.c', 'pet_p4_component_media.c'):
        assert f'"{src}"' in cmake, src


def test_hello_hooks_and_audio_exclusivity_wiring():
    protocol = (FW / 'main/pet_p4_protocol.c').read_text()
    for flag, key, value in (('PET_P4_SEQUENCE', 'widgetSequence', 'p4-sequence-v1'), ('PET_P4_SAMPLES', 'widgetSamples', 'p4-samples-v1'), ('PET_P4_CLIP', 'widgetClip', 'p4-clip-v1'), ('PET_P4_BIG_IMAGE', 'widgetBigImage', 'p4-big-image-v1'), ('PET_P4_EVENTS', 'widgetEvents', 'p4-events-v1')):
        assert f'#if {flag}\n  cJSON_AddStringToObject(capabilities, "{key}", "{value}");\n#endif' in protocol, key
    assert '"component/alarm_policy"' in protocol and '"capabilityExt"' in protocol
    audio = (FW / 'main/pet_p4_audio.c').read_text()
    block = audio[audio.index('static bool cext_mix_step'):audio.index('static bool sfx_drain_step')]
    assert 'pet_seq_mix' in block and 'pet_sampler_mix' in block and 'pet_p4_audio_frontend' not in block
    drain = audio[audio.index('static bool sfx_drain_step'):audio.index('void pet_p4_audio_instrument_set')]
    for flag in ('g_capture_requested', 'g_capture_active', 'g_conversation', 'g_stream_active', 'g_music'):
        assert flag in drain, flag
    assert 'cext_mix_step((int16_t*)pcm,true)' in drain and 'cext_mix_step((int16_t*)pcm,false)' in drain, 'blocked drains reset the sequencer/sampler, never queue'
    assert 'CEXT_ENABLED() || atomic_load(&g_instrument_enabled)' in audio, 'file cues yield to component audio like sfx'
    runtime = (FW / 'main/pet_p4_miniapp.c').read_text()
    for hook in ('miniapp_ext_parse(parsed, root, runtime_v4, error, error_size)', 'miniapp_ext_fire(&g_runtime, &transition->cext)', 'miniapp_ext_fire(&g_runtime, &tick->completion_cext)',
                 'miniapp_ext_process(now_ms, visible)', 'miniapp_ext_prepare(next,g_catalog[index].slot', 'miniapp_ext_removed(widget_id, removed_slot)',
                 'miniapp_ext_install_chunk(payload, path, data, index, error, error_size)', 'pet_cmedia_stage_matches(&next->cext,error,error_size)',
                 'pet_cmedia_persist((size_t) slot, package_generation, &next->cext)', 'miniapp_ext_installed(next)', 'miniapp_ext_save_text(alarms_text,sizeof(',
                 'miniapp_ext_restore_text(r,saved_alarms)', '#include "pet_p4_miniapp_ext.inc"', 'MINIAPP_EXT_OR_EVENT_ACTION(action)'):
        assert hook in runtime, hook
    for hook in ('miniapp_ext_prepare(', 'miniapp_ext_commit(', 'miniapp_ext_abort('):
        assert runtime.count(hook) >= 4, hook
    assert 'miniapp_ext_validate_bindings(parsed,root,buttons_json,error,error_size)' in runtime
    assert runtime.count('#if PET_P4_CAPABILITY_EXT') + runtime.count('#if PET_P4_EVENTS') + runtime.count('#if PET_P4_BIG_IMAGE') >= 20
    main = (FW / 'main/pet_p4_main.c').read_text()
    assert 'pet_p4_miniapp_alarm_notification(alarm_title, alarm_text)' in main and '"通知"' in main
    renderer = (FW / 'main/pet_p4_renderer.c').read_text()
    assert '#include "pet_p4_renderer_ext.inc"' in renderer and 'renderer_ext_draw_clip(n,now_ms)' in renderer and 'renderer_ext_draw_big(n)' in renderer
    ext = (FW / 'main/pet_p4_renderer_ext.inc').read_text()
    assert 'h264_prefetch_offer(&g_clip_entry' in ext and 'decode itself stayed on the worker' in ext
    surface = (FW / 'main/pet_p4_surface.c').read_text()
    assert 'PET_SURFACE_NODE_CLIP' in surface and '"big_images"' in surface and 'offset_x_var' in surface


def test_sampler_releases_borrow_on_exact_final_frame(core):
    s = Sampler()
    sample = (C.c_int16 * 1)(1234)
    assert not core.pet_sampler_trigger(C.byref(s), sample, 32769)
    assert core.pet_sampler_trigger(C.byref(s), sample, 1)
    out = (C.c_int16 * 1)()
    core.pet_sampler_mix(C.byref(s), out, 1, 100)
    assert out[0] == 1234
    assert not core.pet_sampler_active(C.byref(s))
    assert not s.voice[0].pcm


def test_invalid_sequence_restart_stops_previous_program(core):
    state = SeqState()
    p = program()
    core.pet_seq_start(C.byref(state), C.byref(p))
    p.bpm = 0
    core.pet_seq_start(C.byref(state), C.byref(p))
    assert not core.pet_seq_active(C.byref(state))


def test_alarm_restore_is_atomic_and_rejects_missing_number(core):
    alarms = AlarmSet(); alarms.count = 1
    alarms.items[0].id = b'wake'
    before = bytes(alarms)
    for text in (b'wake:60000;bad', b'wake::p', b'wake:+123', b'wake:-1'):
        assert not core.pet_cev_alarms_restore(C.byref(alarms), text, 100)
        assert bytes(alarms) == before


def test_alarm_large_clock_jump_is_constant_time(core):
    alarms = AlarmSet(); alarms.count = 1
    a = alarms.items[0]; a.armed = True; a.every_ms = 60000; a.deadline_ms = 60000
    assert core.pet_cev_alarms_tick(C.byref(alarms), 10**15) == 1
    assert 10**15 < a.deadline_ms <= 10**15 + 60000


def test_media_source_reads_are_bounded_and_confined(tmp_path):
    from capability_ext import validate_media_files, wav_info
    assets = tmp_path / 'assets'; assets.mkdir()
    path = assets / 'huge.wav'
    with path.open('wb') as f:
        f.truncate(4 * 1024 * 1024 + 1)
    runtime = {'samples': {'items': [{'asset': 'assets/huge.wav'}]}}
    errors = []; validate_media_files(tmp_path, runtime, errors)
    assert any('4 MiB' in e for e in errors)
    runtime['samples']['items'][0]['asset'] = '../escape.wav'
    errors = []; validate_media_files(tmp_path, runtime, errors)
    assert any('非法素材路径' in e for e in errors)
    assert wav_info(b'RIFF' + b'\0' * 4 + b'WAVEdata' + struct.pack('<I', 999)) is None


def test_alarm_restore_preserves_observation_and_cancel(core):
    s = alarm_set()
    core.pet_cev_alarms_observe_var(C.byref(s), 1, 300000, 1000)
    text = C.create_string_buffer(256)
    core.pet_cev_alarms_serialize(C.byref(s), 100000, text, 256)
    restored = alarm_set()
    assert core.pet_cev_alarms_restore(C.byref(restored), text.value, 50)
    deadline = restored.items[1].deadline_ms
    core.pet_cev_alarms_observe_var(C.byref(restored), 1, 300000, 100)
    assert restored.items[1].deadline_ms == deadline
    core.pet_cev_alarms_cancel.argtypes = [C.POINTER(AlarmSet), C.c_uint]
    core.pet_cev_alarms_cancel(C.byref(restored), 0)
    core.pet_cev_alarms_cancel(C.byref(restored), 1)
    core.pet_cev_alarms_start(C.byref(restored), 100)
    core.pet_cev_alarms_observe_var(C.byref(restored), 1, 300000, 100)
    assert not restored.items[0].armed and not restored.items[1].armed
    core.pet_cev_alarms_serialize(C.byref(restored), 100, text, 256)
    assert b'water:0:c' in text.value and b'timer:0:v300000' in text.value
    rebooted = alarm_set()
    assert core.pet_cev_alarms_restore(C.byref(rebooted), text.value, 0)
    core.pet_cev_alarms_start(C.byref(rebooted), 0)
    core.pet_cev_alarms_observe_var(C.byref(rebooted), 1, 300000, 0)
    assert not rebooted.items[0].armed and not rebooted.items[1].armed
    assert core.pet_cev_alarms_tick(C.byref(rebooted), 10**12) == 0
    before = bytes(rebooted)
    for bad in (b'timer:1;timer:2', b';timer:1', b'timer:1;;water:2', b'timer:1;', b'water:0:c:p'):
        assert not core.pet_cev_alarms_restore(C.byref(rebooted), bad, 0)
        assert bytes(rebooted) == before


def test_overdue_alarm_save_keeps_pending_and_small_buffer_fails_whole_save(core):
    s = alarm_set()
    core.pet_cev_alarms_observe_var(C.byref(s), 1, 60000, 0)
    text = C.create_string_buffer(256)
    core.pet_cev_alarms_serialize(C.byref(s), 70000, text, 256)
    assert b'timer:0:p:v60000' in text.value
    restored = alarm_set()
    assert core.pet_cev_alarms_restore(C.byref(restored), text.value, 0)
    assert restored.items[1].pending and not restored.items[1].armed
    small = C.create_string_buffer(5)
    assert core.pet_cev_alarms_serialize(C.byref(s), 0, small, 5) == 0
    assert small.value == b''


def test_legacy_remaining_duration_does_not_rearm_on_first_observation(core):
    s = alarm_set()
    assert core.pet_cev_alarms_restore(C.byref(s), b'timer:123000', 50)
    core.pet_cev_alarms_observe_var(C.byref(s), 1, 300000, 100)
    assert s.items[1].deadline_ms == 123050
    core.pet_cev_alarms_observe_var(C.byref(s), 1, 0, 101)
    assert not s.items[1].armed


@pytest.mark.parametrize('role', ('sequence','sample','clip','big','event-send','event-recv','alarm'))
def test_private_hil_sources_use_real_media_and_parse_in_production_core(harness, role):
    import hashlib
    from capability_ext import validate_media_files
    root = FW / 'tests/fixtures/track_c_private'
    manifest = json.loads((root/'manifest.json').read_text())
    assert manifest['private'] and not manifest['deviceEvidence'] and not manifest['installed']
    item = next(p for p in manifest['packages'] if p['id'] == 'private-cext-' + role)
    path = root/item['path']
    runtime = json.loads((path/'runtime/widget.json').read_text())
    assert not any(k in runtime for k in ('instrument','media','local','program','host'))
    result = harness(runtime)
    assert result['ok'], result
    errors = []
    validate_media_files(path,runtime,errors)
    assert not errors,errors
    for source in item['assets']:
        assert hashlib.sha256((path/source['asset']).read_bytes()).hexdigest() == source['sha256']
    # Internal events are deliberately not bound to fake physical inputs.
    actions = {b['action'] for b in json.loads((path/'buttons.json').read_text())}
    assert 'event:done' not in actions and 'ring' not in actions
