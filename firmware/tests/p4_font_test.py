"""Static font payload invariants: full fallback + original native32 alpha ink."""
import hashlib,json,re,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def array(source,name):
    m=re.search(r'\b'+name+r'\[\d*\]\s*=\s*\{([\s\S]*?)\};',source)
    assert m,name
    return [int(v,0) for v in re.findall(r'-?0x[\da-f]+|-?\d+',re.sub(r'/\*[\s\S]*?\*/','',m[1]),re.I)]
def font(name):
    s=(ROOT/'main'/name).read_text();glyphs=[tuple(map(int,m)) for m in re.findall(r'\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}',s)];ids={}
    for m in re.finditer(r'\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*\.unicode_list = (\w+), \.glyph_id_ofs_list = NULL, \.list_length = \d+, \.type = (\w+)',s):
        base,length,start=map(int,m.group(1,2,3));offsets=range(length) if m[4]=='NULL' else array(s,m[4])
        for i,offset in enumerate(offsets):assert base+offset not in ids;ids[base+offset]=start+i
    bitmap=list(zlib.decompress((ROOT/'assets/fonts/pet_cn_32.deflate').read_bytes())) if name=='pet_p4_font_cn_32.c' else array(s,'glyph_bitmap')
    return s,glyphs,ids,bitmap

def test_full_21207_fallback_keeps_original_coverage_and_one_bit_ink():
    s,glyphs,ids,bits=font('pet_p4_font_cn.c')
    assert len(ids)==21207 and len(glyphs)==21208 and len(bits)==554086
    # MiSans leaves some Unicode slots undefined; preserve the actual original
    # payload exactly rather than inventing glyphs or shrinking to GB2312.
    assert hashlib.sha256(bytes(bits)).hexdigest()=='1e357bf8ec947c56e3aca7221f315cb9b883aa26367abf8cb0c5fb7eeb601f5a'
    required=set()
    for hi in range(0xb0,0xf8):
        for lo in range(0xa1,0xff):
            try:required.add(ord(bytes([hi,lo]).decode('gb2312')))
            except UnicodeDecodeError:pass
    assert len(required)==6763 and required<=ids.keys()
    assert re.search(r'\.bpp = 1\b',s) and '.line_height = 18' in s and '.base_line = 3' in s
    for i,(offset,_,w,h,_,_) in enumerate(glyphs[1:],1):
        end=glyphs[i+1][0] if i+1<len(glyphs) else len(bits)
        assert offset+(w*h+7)//8==end

def test_native32_subset_retains_gray_ink_and_fallback_layout_advances():
    _,base,ids,_=font('pet_p4_font_cn.c');s,glyphs,codes,bitmap=font('pet_p4_font_cn_32.c')
    assert len(codes)==199 and len(glyphs)==200 and len(bitmap)<40*1024
    assert hashlib.sha256(bytes(bitmap)).hexdigest()=='11c6f016620ea73b1be8c4e78a3f2fb6342cf9e4072781f233f14b16ea4c4dd7'  # original PR12 ink, unmodified
    assert '.bpp = 2' in s and '.line_height = 36' in s and '.base_line = 6' in s
    # The original199 are a subset, not a claim that every factory title is native32.
    # Unsupported32 codepoints keep the full16 fallback and its layout advances.
    assert set(range(32,127))<=codes.keys() and codes.keys()<=ids.keys()
    renderer=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert 'pet_p4_font_native32_has_glyph(cp)' in renderer
    assert 'return lv_font_get_glyph_dsc(&pet_p4_font_cn_16,glyph,cp,next)' in renderer
    for cp,i in codes.items():
        offset,advance,w,h,_,_=glyphs[i];end=glyphs[i+1][0] if i+1<len(glyphs) else len(bitmap)
        assert offset+(w*h*2+7)//8==end
        assert advance==base[ids[cp]][1]*2
    for cp in map(ord,'每日新鲜事休息中你宠物'):
        offset,_,w,h,_,_=glyphs[codes[cp]]
        pixels=[(bitmap[offset+i//4]>>(6-i%4*2))&3 for i in range(w*h)]
        assert {0,3}<=set(pixels) and set(pixels)&{1,2} and h>20
