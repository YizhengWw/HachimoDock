"""Lossless identity packing of PR12's licensed 32px/199-glyph font only.
No rasterization, bpp reduction, glyph removal, network or firmware build.
The .deflate is a reproducibility artifact; firmware includes its bytes ONCE
through the gate-wrapped C initializer, not through target_add_binary_data.
"""
import argparse,hashlib,re,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOURCE_SHA='4dbdb86e9f5e871be917eabcd7753674b64a03b54320d733fe3c4a3d60d70cf8'
BITMAP_SHA='11c6f016620ea73b1be8c4e78a3f2fb6342cf9e4072781f233f14b16ea4c4dd7'
PROVENANCE='PR12 head 85e194d9d970513c6a475a33ce8cabe0c5c72603'
def source_data():
 source=(ROOT/'tools/font_sources/pr12_pet_p4_font_cn_32.c').read_bytes()
 if hashlib.sha256(source).hexdigest()!=SOURCE_SHA:raise ValueError('Review changed PR12 source before repacking')
 text=source.decode()
 body=re.search(r'\bglyph_bitmap\[\]\s*=\s*\{(.*?)\n\};',text,re.S)[1]
 raw=bytes(int(n,0) for n in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b',re.sub(r'/\*.*?\*/','',body,flags=re.S)))
 if len(raw)!=30579 or hashlib.sha256(raw).hexdigest()!=BITMAP_SHA:raise ValueError('Changed 32px bitmap')
 return source,raw

def outputs():
 source,raw=source_data();packed=zlib.compress(raw,9)
 inc='/* Identity zlib, no changed pixels or glyph order. '+PROVENANCE+' */\n'+''.join('  '+','.join(f'0x{v:02x}' for v in packed[i:i+16])+',\n' for i in range(0,len(packed),16))
 header=('/* Original 199-glyph MiSans Medium 32px/2bpp data, lossless. */\n'
  '#pragma once\n#include <stdint.h>\n'
  '#define PET_FONT_NATIVE32_BITMAP_BYTES 30579U\n'
  f'#define PET_FONT_NATIVE32_PACKED_BYTES {len(packed)}U\n'
  '#define PET_FONT_NATIVE32_GLYPHS 199U\n'
  f'#define PET_FONT_NATIVE32_BITMAP_ADLER UINT32_C(0x{zlib.adler32(raw):08x})\n'
  f'#define PET_FONT_NATIVE32_SOURCE_SHA256 "{SOURCE_SHA}"\n'
  f'#define PET_FONT_NATIVE32_BITMAP_SHA256 "{BITMAP_SHA}"\n')
 return {'assets/fonts/pet_cn_32.deflate':packed,'main/pet_p4_font_native32_packed.inc':inc.encode(),'main/pet_p4_font_native32_size.h':header.encode()}

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--check',action='store_true');p.add_argument('--output-root',type=Path);args=p.parse_args()
 expected=outputs();root=args.output_root or ROOT
 if not args.check and args.output_root is None:p.error('Explicit --output-root required to generate; use --check for read-only validation')
 for name,data in expected.items():
  target=root/name
  if args.check:
   if target.read_bytes()!=data:raise ValueError('Changed/stale asset: '+name)
  else:
   target.parent.mkdir(parents=True,exist_ok=True)
   with target.open('xb') as f:f.write(data)
 packed=expected['assets/fonts/pet_cn_32.deflate'];decoder=zlib.decompressobj();raw=decoder.decompress(packed)+decoder.flush()
 if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail or hashlib.sha256(raw).hexdigest()!=BITMAP_SHA:raise ValueError('Not an exact identity stream')
 print(f'native32: 199 glyphs, 2bpp, 30579 -> {len(packed)} bytes; identity SHA {BITMAP_SHA}')
if __name__=='__main__':main()
