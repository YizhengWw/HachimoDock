"""Exercise production DSP and motion, not a replacement Python synthesizer."""
import ctypes as C
import subprocess
import math
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[1]
class Core(C.Structure):
    _fields_ = [("samples",C.c_uint32),("noise",C.c_uint32),("age",C.c_uint32*4),("next",C.c_uint),("gain",C.c_float)]
class Motion(C.Structure):
    _fields_ = [("mallet_y",C.c_float),("fish_scale",C.c_float),("wave",C.c_float)]

@pytest.fixture(scope="module")
def dsp(tmp_path_factory):
    out=tmp_path_factory.mktemp("instrument")/"core.so"
    subprocess.run(["cc","-shared","-fPIC","-O2",str(ROOT/"main/pet_p4_instrument_core.c"),"-lm","-o",str(out)],check=True)
    lib=C.CDLL(str(out))
    lib.pet_instrument_reset.argtypes=[C.POINTER(Core)]
    lib.pet_instrument_strike.argtypes=[C.POINTER(Core)]
    lib.pet_instrument_render.argtypes=[C.POINTER(Core),C.POINTER(C.c_int16),C.c_size_t,C.c_bool,C.c_uint,C.c_uint]
    lib.pet_instrument_render_tone.argtypes=lib.pet_instrument_render.argtypes+[C.c_uint]
    lib.pet_instrument_motion.argtypes=[C.c_uint64];lib.pet_instrument_motion.restype=Motion
    lib.pet_instrument_motion_from.argtypes=[C.c_uint64,C.c_float,C.c_float]
    lib.pet_instrument_motion_from.restype=Motion
    return lib

def render(dsp,s,n=16000,on=True,effect=70,ambience=18):
    pcm=(C.c_int16*n)();dsp.pet_instrument_render(C.byref(s),pcm,n,on,effect,ambience);return list(pcm)

def test_idle_hit_tail_and_exit(dsp):
    s=Core();dsp.pet_instrument_reset(C.byref(s));s.gain=1
    assert max(map(abs,render(dsp,s,320,ambience=0)))==0
    dsp.pet_instrument_strike(C.byref(s));pcm=render(dsp,s,ambience=0)
    assert max(map(abs,pcm[:1600]))>4000
    assert max(map(abs,pcm[-1600:]))<3
    render(dsp,s,3200,on=False)
    assert s.gain==0 and max(map(abs,render(dsp,s,on=False)))==0

def test_fast_hits_bounded_no_overflow_and_reset(dsp):
    s=Core();dsp.pet_instrument_reset(C.byref(s));s.gain=1
    for _ in range(100):dsp.pet_instrument_strike(C.byref(s))
    pcm=render(dsp,s,effect=100,ambience=100)
    assert max(pcm)<32767 and min(pcm)>-32768
    dsp.pet_instrument_reset(C.byref(s))
    assert max(map(abs,render(dsp,s,on=False)))==0

def test_volume_zero_and_motion_settles(dsp):
    s=Core();dsp.pet_instrument_reset(C.byref(s));dsp.pet_instrument_strike(C.byref(s))
    assert not any(render(dsp,s,effect=0,ambience=0))
    assert dsp.pet_instrument_motion(0).mallet_y==0
    assert dsp.pet_instrument_motion(55).mallet_y==68
    assert dsp.pet_instrument_motion(200).mallet_y<0
    m=dsp.pet_instrument_motion(900)
    assert (m.mallet_y,m.fish_scale,m.wave)==(0,1,1)

def test_default_is_selected_a_and_contacts_at_55ms(dsp):
    s=Core();dsp.pet_instrument_reset(C.byref(s));s.gain=1
    dsp.pet_instrument_strike(C.byref(s))
    default=render(dsp,s,ambience=0)
    a=Core();dsp.pet_instrument_reset(C.byref(a));a.gain=1
    dsp.pet_instrument_strike(C.byref(a))
    pcm=(C.c_int16*16000)()
    dsp.pet_instrument_render_tone(C.byref(a),pcm,16000,True,70,0,0)
    assert default==list(pcm)
    assert not any(default[:880]) and any(default[880:1000])

def test_default_background_is_audible_but_below_knock(dsp):
    s=Core();dsp.pet_instrument_reset(C.byref(s));s.gain=1
    bed=render(dsp,s,256000,effect=0,ambience=18)
    rms=math.sqrt(sum(x*x for x in bed)/len(bed))/32768
    assert .015 < rms < .04
    assert max(map(abs,bed)) < 3500
    s=Core();dsp.pet_instrument_reset(C.byref(s));s.gain=1
    dsp.pet_instrument_strike(C.byref(s))
    assert max(map(abs,render(dsp,s,ambience=0))) > 3*max(map(abs,bed))

def test_control_loop_uses_small_instrument_query_not_render_snapshot():
    main=(ROOT/'main/pet_p4_main.c').read_text()
    assert 'pet_p4_miniapp_view_t instrument_view' not in main
    assert 'pet_p4_miniapp_instrument_config(&instrument_effect,&instrument_ambience)' in main
    renderer=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert 'media_page || instrument_page ||' in renderer

def test_rapid_hits_keep_motion_continuous(dsp):
    y,scale=0,1
    for gap in [20,40,90,120,160,70,200,35,65]:
        before=dsp.pet_instrument_motion_from(gap,y,scale)
        after=dsp.pet_instrument_motion_from(0,before.mallet_y,before.fish_scale)
        assert abs(after.mallet_y-before.mallet_y)<.001
        assert abs(after.fish_scale-before.fish_scale)<.001
        y,scale=after.mallet_y,after.fish_scale
        assert dsp.pet_instrument_motion_from(55,y,scale).mallet_y==68
