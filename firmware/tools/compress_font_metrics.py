"""Losslessly encode existing licensed metrics; never regenerate/subset a font."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import zlib

ROOT=Path(__file__).resolve().parents[1]
RAW_SHA256='63e69048a92d090d59cd7466ff6554e98055dba83bd9da69205099a600a9bb49'
RECORDS=21208


def metrics_bytes():
    source=(ROOT/'main/pet_p4_font_cn.c').read_text()
    tables=re.findall(r'const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\] = \{(.*?)\n\};',source,re.S)
    if len(tables)!=1:raise ValueError('Expected original metric table')
    body=re.sub(r'/\*.*?\*/','',tables[0],flags=re.S)
    pattern=r'\{\s*\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\s*\}'
    rows=re.findall(pattern,body)
    if len(rows)!=RECORDS or re.sub(r'[\s,]','',re.sub(pattern,'',body)):
        raise ValueError('Review changed font metric syntax/count')
    raw=bytearray()
    for row in rows:
        index,advance,width,height,ox,oy=map(int,row)
        if not(0<=index<2**20 and 0<=advance<2**12 and 0<=width<256 and 0<=height<256 and -128<=ox<128 and -128<=oy<128):
            raise ValueError('Metric exceeds canonical bounds')
        raw.extend(struct.pack('<IBBbb',index|(advance<<20),width,height,ox,oy))
    raw=bytes(raw)
    if hashlib.sha256(raw).hexdigest()!=RAW_SHA256:raise ValueError('Review changed original metrics before packing')
    return raw


def size_header(raw,packed):
    return ('/* Lossless original MiSans metrics; retain source and license. */\n'
      '#pragma once\n'
      f'#define PET_FONT_METRIC_RECORDS {RECORDS}U\n'
      f'#define PET_FONT_METRIC_BYTES {len(raw)}U\n'
      f'#define PET_FONT_METRIC_PACKED_BYTES {len(packed)}U\n'
      f'#define PET_FONT_METRIC_ADLER UINT32_C(0x{zlib.adler32(raw):08x})\n')


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true');args=parser.parse_args()
    raw=metrics_bytes();asset=ROOT/'assets/fonts/pet_cn_metrics.deflate';header=ROOT/'main/pet_p4_font_metrics_size.h'
    if args.check:
        packed=asset.read_bytes()
        if zlib.decompress(packed)!=raw or header.read_text()!=size_header(raw,packed):raise ValueError('Stale lossless metric asset')
    else:
        packed=zlib.compress(raw,9)
        assert zlib.decompress(packed)==raw
        asset.write_bytes(packed);header.write_text(size_header(raw,packed))
    print(f'metrics: {RECORDS} records/{len(raw)} -> {len(packed)} bytes; lossless sha256={RAW_SHA256}')


if __name__=='__main__':main()
