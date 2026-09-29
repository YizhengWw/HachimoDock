"""Reproduce the fixed A strike and verify the flash table without rewriting source."""
import math
import re
from pathlib import Path

path=Path(__file__).resolve().parents[1]/'main/pet_p4_instrument_tone_table.h'
source=path.read_text().split('={',1)[1].split('};',1)[0]
actual=[int(x) for x in re.findall(r'-?\d+',source)]
expected=[]
noise=0x6d757975
for age in range(8192):
    noise ^= (noise << 13) & 0xffffffff
    noise ^= noise >> 17
    noise ^= (noise << 5) & 0xffffffff
    t=(age-880)/16000
    hit=0 if age<880 else min(t*2200,1)*(
        .44*math.sin(2*math.pi*1050*t)*math.exp(-28*t)
        +.28*math.sin(2*math.pi*1808*t)*math.exp(-48*t)
        +.15*math.sin(2*math.pi*2780*t)*math.exp(-72*t)
        +.12*((noise&65535)-32768)/32768*math.exp(-340*t))
    expected.append(math.floor(hit*32768+.5))
assert actual==expected, 'A strike PCM table differs from the documented modal synthesis'
assert len(actual)==8192 and max(abs(x) for x in actual[-128:])<=1
print('PASS: reproducible 8192-sample A strike, bounded inaudible tail')
