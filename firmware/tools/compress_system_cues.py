"""Lossless representation of seven unchanged 16kHz/mono/s16 factory cues.
Six modulo-65536 differences, signed zigzag, low/high byteplanes, then zlib.
No resampling, gain, waveform editing, network or external codec tools.
The packed bytes are included once by C, not also embedded by CMake.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import wave
import zlib

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('permission_request', 'permission_approved', 'permission_denied',
         'permission_expired', 'session_terminated', 'done', 'error')
ORDER = 6


def fnv32(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def transform(raw):
    values = [int.from_bytes(raw[i:i+2], 'little') for i in range(0, len(raw), 2)]
    for _ in range(ORDER):
        previous, differences = 0, []
        for value in values:
            differences.append((value - previous) & 0xffff)
            previous = value
        values = differences
    signed = [value if value < 32768 else value - 65536 for value in values]
    zigzag = [(value << 1) ^ (value >> 15) for value in signed]
    return bytes(value & 255 for value in zigzag) + bytes(value >> 8 for value in zigzag)


def restore(planes):
    assert len(planes) % 2 == 0
    count, history, raw = len(planes) // 2, [0] * ORDER, bytearray()
    for i in range(count):
        zigzag = planes[i] | (planes[count+i] << 8)
        value = ((zigzag >> 1) ^ -(zigzag & 1)) & 0xffff
        for order in range(ORDER):
            history[order] = (history[order] + value) & 0xffff
            value = history[order]
        raw.extend(value.to_bytes(2, 'little'))
    return bytes(raw)


def outputs():
    packed, rows, pcm_offset = bytearray(), [], 0
    for name in NAMES:
        path = ROOT.parent / 'pc/public/terrier-clips' / (name + '.wav')
        wav = path.read_bytes()
        with wave.open(io.BytesIO(wav), 'rb') as source:
            if (source.getnchannels(), source.getsampwidth(), source.getframerate(), source.getcomptype()) != (1, 2, 16000, 'NONE'):
                raise ValueError('Factory cue format changed: ' + name)
            pcm = source.readframes(source.getnframes())
        if not pcm or len(pcm) % 2:
            raise ValueError('Invalid PCM length: ' + name)
        planes = transform(pcm)
        encoded = zlib.compress(planes, 9)
        if restore(zlib.decompress(encoded)) != pcm:
            raise ValueError('Non-exact cue pack: ' + name)
        rows.append(dict(name=name, wavBytes=len(wav), pcmBytes=len(pcm),
                         packedBytes=len(encoded), packedOffset=len(packed), pcmOffset=pcm_offset,
                         planesAdler32=zlib.adler32(planes), pcmFnv32=fnv32(pcm),
                         wavSha256=hashlib.sha256(wav).hexdigest(),
                         pcmSha256=hashlib.sha256(pcm).hexdigest()))
        packed.extend(encoded)
        pcm_offset += len(pcm)
    header = ('/* Generated lossless factory cue dimensions; see tools/compress_system_cues.py. */\n'
              '#pragma once\n'
              f'#define PET_CUE_PACKED_COUNT {len(rows)}U\n'
              f'#define PET_CUE_PACKED_BYTES {len(packed)}U\n'
              f'#define PET_CUE_PCM_BYTES {pcm_offset}U\n'
              f'#define PET_CUE_TEMP_BYTES {max(row["pcmBytes"] for row in rows)}U\n'
              f'#define PET_CUE_DELTA_ORDER {ORDER}U\n')
    inc = '/* Generated: immutable zlib bytes, unchanged factory PCM. Included ONCE. */\n'
    inc += 'static const uint8_t packed_bytes[PET_CUE_PACKED_BYTES] = {\n'
    inc += ''.join('  ' + ','.join(f'0x{value:02x}' for value in packed[i:i+16]) + ',\n'
                   for i in range(0, len(packed), 16)) + '};\n'
    inc += 'static const cue_descriptor_t descriptors[PET_CUE_PACKED_COUNT] = {\n'
    for row in rows:
        inc += ('  {%dU,%dU,%dU,%dU,UINT32_C(0x%08x),UINT32_C(0x%08x)}, /* %s */\n'
                % (row['packedOffset'], row['packedBytes'], row['pcmOffset'], row['pcmBytes'],
                   row['planesAdler32'], row['pcmFnv32'], row['name']))
    inc += '};\n'
    metadata = dict(format='delta16-mod65536-order6-zigzag-low-high-zlib-v1',
                    rawWavBytes=sum(row['wavBytes'] for row in rows),
                    pcmBytes=pcm_offset, packedBytes=len(packed),
                    descriptorBytes32bit=24 * len(rows), cues=rows)
    return {'main/pet_p4_cue_packed_size.h': header.encode(),
            'main/pet_p4_cue_packed_data.inc': inc.encode(),
            'assets/audio/system_cues_packed.json': (json.dumps(metadata, indent=2) + '\n').encode()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    result = outputs()
    for name, data in result.items():
        path = ROOT / name
        if args.check:
            if not path.is_file() or path.read_bytes() != data:
                raise ValueError('Stale packed cue input: ' + name)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    metadata = json.loads(result['assets/audio/system_cues_packed.json'])
    print('factory cues: %d WAV -> %d packed + %d descriptor bytes; %d exact PCM bytes'
          % (metadata['rawWavBytes'], metadata['packedBytes'], metadata['descriptorBytes32bit'], metadata['pcmBytes']))


if __name__ == '__main__':
    main()
