#!/usr/bin/env python3
"""New, gated byte-column format; retain all original licensed metric records.
Standard library only. Never modifies the legacy asset or the source font.
"""
import argparse,hashlib,json,re,struct,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
RECORDS=21208
STRIDE=8
ORIGINAL_SHA='63e69048a92d090d59cd7466ff6554e98055dba83bd9da69205099a600a9bb49'
def original(source):
 s=Path(source).read_text()
 tables=re.findall(r'const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\] = \{(.*?)\n\};',s,re.S)
 if len(tables)!=1:raise ValueError('Original metric table missing')
 body=re.sub(r'/\*.*?\*/','',tables[0],flags=re.S)
 pattern=r'\{\s*\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\s*\}'
 rows=re.findall(pattern,body)
 if len(rows)!=RECORDS or re.sub(r'[\s,]','',re.sub(pattern,'',body)):raise ValueError('Original metric syntax/count changed')
 out=bytearray()
 for row in rows:
  index,advance,w,h,ox,oy=map(int,row)
  if not(0<=index<2**20 and 0<=advance<2**12 and 0<=w<256 and 0<=h<256 and -128<=ox<128 and -128<=oy<128):raise ValueError('Metric field out of range')
  out.extend(struct.pack('<IBBbb',index|(advance<<20),w,h,ox,oy))
 raw=bytes(out)
 if hashlib.sha256(raw).hexdigest()!=ORIGINAL_SHA:raise ValueError('Original metrics SHA changed')
 return raw
def columns(raw):
 if len(raw)!=RECORDS*STRIDE:raise ValueError('Canonical metric length mismatch')
 return b''.join(raw[column::STRIDE] for column in range(STRIDE))
def inverse(data):
 if len(data)!=RECORDS*STRIDE:raise ValueError('Column metric length mismatch')
 out=bytearray(len(data))
 for col in range(STRIDE):out[col::STRIDE]=data[col*RECORDS:(col+1)*RECORDS]
 return bytes(out)
def header(raw,data,packed):
 return ('/* Original licensed MiSans metrics, byte-column v1; no subset/regeneration. */\n'
  '/* Payload is exactly eight ordered columns; no implicit format autodetection. */\n'
  '#pragma once\n#include <stdint.h>\n'
  '#define PET_FONT_METRIC_COLUMNS_FORMAT 1U\n'
  f'#define PET_FONT_METRIC_COLUMNS_RECORDS {RECORDS}U\n'
  f'#define PET_FONT_METRIC_COLUMNS_STRIDE {STRIDE}U\n'
  f'#define PET_FONT_METRIC_COLUMNS_BYTES {len(data)}U\n'
  f'#define PET_FONT_METRIC_COLUMNS_PACKED_BYTES {len(packed)}U\n'
  f'#define PET_FONT_METRIC_COLUMNS_PACKED_CRC UINT32_C(0x{zlib.crc32(packed):08x})\n'
  f'#define PET_FONT_METRIC_COLUMNS_CRC UINT32_C(0x{zlib.crc32(data):08x})\n'
  f'#define PET_FONT_METRIC_COLUMNS_ADLER UINT32_C(0x{zlib.adler32(data):08x})\n'
  f'#define PET_FONT_METRIC_COLUMNS_ORIGINAL_CRC UINT32_C(0x{zlib.crc32(raw):08x})\n'
  f'#define PET_FONT_METRIC_COLUMNS_ORIGINAL_ADLER UINT32_C(0x{zlib.adler32(raw):08x})\n'
  f'#define PET_FONT_METRIC_COLUMNS_ORIGINAL_SHA256 "{ORIGINAL_SHA}"\n')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT/'main/pet_p4_font_cn.c');p.add_argument('--asset',type=Path,default=ROOT/'assets/fonts/pet_cn_metrics_columns.deflate');p.add_argument('--header',type=Path,default=ROOT/'main/pet_p4_font_metrics_columns_size.h');p.add_argument('--check',action='store_true');a=p.parse_args()
 raw=original(a.source);data=columns(raw);packed=zlib.compress(data,9)
 if inverse(data)!=raw:raise ValueError('Metric inverse mismatch')
 if a.check:
  existing=a.asset.read_bytes();d=zlib.decompressobj();unpacked=d.decompress(existing)+d.flush()
  if not d.eof or d.unused_data or d.unconsumed_tail or unpacked!=data or a.header.read_text()!=header(raw,data,existing):raise ValueError('Changed/stale/trailing metric column asset')
  packed=existing
 else:
  for f,b in [(a.asset,packed),(a.header,header(raw,data,packed).encode())]:
   with f.open('xb') as stream:stream.write(b)
 print(json.dumps({'records':RECORDS,'originalBytes':len(raw),'originalZlib9Bytes':len(zlib.compress(raw,9)),'columnZlib9Bytes':len(packed),'savedBytes':len(zlib.compress(raw,9))-len(packed),'originalSha256':ORIGINAL_SHA,'columnsSha256':hashlib.sha256(data).hexdigest(),'packedSha256':hashlib.sha256(packed).hexdigest(),'roundTripExact':True}))
if __name__=='__main__':main()
