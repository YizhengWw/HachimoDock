"""Compile production DSP and render preview PCM (no recording or remote audio)."""
import ctypes as C
import subprocess
import tempfile
import wave
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
class Core(C.Structure):
    _fields_=[("samples",C.c_uint32),("noise",C.c_uint32),("age",C.c_uint32*4),("next",C.c_uint),("gain",C.c_float)]
with tempfile.TemporaryDirectory(prefix="pet-instrument-") as folder:
    libpath=Path(folder)/"core.so"
    subprocess.run(["cc","-shared","-fPIC","-O2",str(ROOT/"firmware/main/pet_p4_instrument_core.c"),"-lm","-o",str(libpath)],check=True)
    lib=C.CDLL(str(libpath))
    lib.pet_instrument_render.argtypes=[C.POINTER(Core),C.POINTER(C.c_int16),C.c_size_t,C.c_bool,C.c_uint,C.c_uint]
    lib.pet_instrument_render_tone.argtypes=lib.pet_instrument_render.argtypes+[C.c_uint]
    for name,n,e,a in (("knock",16000,70,0),("ambience",256000,0,18)):
        s=Core();lib.pet_instrument_reset(C.byref(s));s.gain=1
        if name=="knock":lib.pet_instrument_strike(C.byref(s))
        pcm=(C.c_int16*n)();lib.pet_instrument_render(C.byref(s),pcm,n,True,e,a)
        with wave.open(str(ROOT/f"pc/public/instruments/wooden-fish/{name}.wav"),"wb") as wav:
            wav.setparams((1,2,16000,0,"NONE","not compressed"));wav.writeframes(bytes(pcm))
    for tone,name in enumerate(("A-crisp","B-hollow","C-soft")):
        s=Core();lib.pet_instrument_reset(C.byref(s));s.gain=1
        segments=[]
        for _ in range(3):
            lib.pet_instrument_strike(C.byref(s));pcm=(C.c_int16*20800)()
            lib.pet_instrument_render_tone(C.byref(s),pcm,20800,True,70,0,tone)
            segments.append(bytes(pcm))
        with wave.open(str(ROOT/f"pc/public/instruments/wooden-fish/{name}.wav"),"wb") as wav:
            wav.setparams((1,2,16000,0,"NONE","not compressed"));wav.writeframes(b"".join(segments))
