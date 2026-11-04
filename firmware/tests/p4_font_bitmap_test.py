"""Lossless font bytes and actual startup latch. Host zlib adapts the ROM API;
this is not ROM timing, P4 memory or visual/performance hardware acceptance.
"""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]

def test_all_original_glyph_bytes_and_reproducible_pack():
    script = ROOT / 'tools/compress_font_asset.py'
    spec = importlib.util.spec_from_file_location('font_pack', script)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    raw = module.bitmap_bytes()
    assert len(raw) == 554086
    assert hashlib.sha256(raw).hexdigest() == '1e357bf8ec947c56e3aca7221f315cb9b883aa26367abf8cb0c5fb7eeb601f5a'
    subprocess.run([sys.executable, str(script), '--check'], check=True)
    text = (ROOT / 'main/pet_p4_font_cn.c').read_text()
    assert 'MiSans Fonts Intellectual Property License Agreement' in text
    assert '.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt' in text
    assert '.glyph_bitmap = PET_FONT_GLYPH_BITMAP' in text
    assert '#define glyph_bitmap ' not in text  # Must not rewrite the LVGL field name.
    # Compile the actual complete descriptors against the actual LVGL types;
    # byte equality alone cannot detect a preprocessor collision in a field.
    for packed in (0,1):
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DLV_CONF_SKIP',
            f'-DPET_FONT_BITMAP_PACKED={packed}','-I',str(ROOT/'managed_components/lvgl__lvgl'),
            '-I',str(ROOT/'main'),'-fsyntax-only',str(ROOT/'main/pet_p4_font_cn.c')],check=True)

def test_real_init_exact_lengths_checksum_failure_latch_and_byte_identity(tmp_path):
    # Bounded API adapter only: real compressed bytes go through host zlib.
    (tmp_path/'esp_attr.h').write_text('#pragma once\n#define EXT_RAM_BSS_ATTR\n')
    (tmp_path/'miniz.h').write_text(r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {unsigned m_state,m_check_adler32;} tinfl_decompressor;
typedef int tinfl_status;
#define TINFL_STATUS_DONE 0
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 4
#define tinfl_init(r) ((r)->m_state=0)
#define tinfl_get_adler32(r) ((r)->m_check_adler32)
tinfl_status tinfl_decompress(tinfl_decompressor*,const unsigned char*,size_t*,unsigned char*,unsigned char*,size_t*,unsigned);
''')
    source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "pet_p4_font_bitmap.c"
static int mode,calls;
tinfl_status tinfl_decompress(tinfl_decompressor*r,const unsigned char*src,size_t*in,
 unsigned char*start,unsigned char*out,size_t*length,unsigned flags){
  calls++;assert(start==out&&flags==(TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  z_stream z={0};z.next_in=(void*)src;z.avail_in=(uInt)*in;z.next_out=out;z.avail_out=(uInt)*length;
  assert(inflateInit(&z)==Z_OK);int result=inflate(&z,Z_FINISH);
  *in=z.total_in;*length=z.total_out;r->m_check_adler32=(unsigned)z.adler;assert(inflateEnd(&z)==Z_OK);
  if(mode==1)return -1;
  if(mode==2)--*in;
  if(mode==3)--*length;
  if(mode==4)r->m_check_adler32++;
  return result==Z_STREAM_END?TINFL_STATUS_DONE:-1;
}
int main(int argc,char**argv){
  assert(argc==3);mode=atoi(argv[1]);bool ok=pet_p4_font_bitmap_init();
  assert(ok==(mode==0));assert(pet_p4_font_bitmap_init()==ok&&calls==1);
  if(ok){FILE*f=fopen(argv[2],"rb");assert(f);unsigned char expected[4096];size_t offset=0,n;
    while((n=fread(expected,1,sizeof(expected),f))){assert(offset+n<=sizeof(pet_font_bitmap));assert(!memcmp(pet_font_bitmap+offset,expected,n));offset+=n;}
    assert(!ferror(f)&&!fclose(f)&&offset==sizeof(pet_font_bitmap));
  }else for(size_t i=0;i<sizeof(pet_font_bitmap);i++)assert(pet_font_bitmap[i]==0);
  return 0;
}
'''
    script = ROOT / 'tools/compress_font_asset.py'
    spec = importlib.util.spec_from_file_location('font_pack_faults', script)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    raw_path=tmp_path/'expected.bin';raw_path.write_bytes(module.bitmap_bytes())
    asset=ROOT/'assets/fonts/pet_cn_16.deflate'
    section='.section __TEXT,__const' if sys.platform=='darwin' else '.section .rodata'
    assembly=tmp_path/'font.S'
    assembly.write_text(section+'\n.globl _binary_pet_cn_16_deflate_start\n.globl _binary_pet_cn_16_deflate_end\n'
        '_binary_pet_cn_16_deflate_start:\n.incbin "'+str(asset)+'"\n_binary_pet_cn_16_deflate_end:\n')
    path=tmp_path/'font.c';path.write_text(source);binary=tmp_path/'font'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-DPET_FONT_BITMAP_PACKED=1','-I',str(tmp_path),'-I',str(ROOT/'main'),
        str(path),str(assembly),'-lz','-o',str(binary)],check=True)
    for mode in range(5):
        subprocess.run([str(binary),str(mode),str(raw_path)],check=True,timeout=10)
    plain=tmp_path/'plain.c'
    plain.write_text('#include <assert.h>\n#include "pet_p4_font_bitmap.c"\nint main(void){assert(pet_p4_font_bitmap_init());assert(pet_p4_font_bitmap_init());}\n')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),
        str(plain),'-o',str(tmp_path/'plain')],check=True)
    subprocess.run([str(tmp_path/'plain')],check=True)

def test_startup_precedes_input_audio_render_and_has_no_runtime_effects():
    text=(ROOT/'main/pet_p4_main.c').read_text().split('void app_main(void) {',1)[1]
    at=text.index('pet_p4_font_bitmap_init()?ESP_OK:ESP_ERR_INVALID_STATE')
    for call in ('pet_p4_input_init()', 'pet_p4_audio_init(', 'pet_p4_renderer_render('):
        assert at<text.index(call)
    implementation=(ROOT/'main/pet_p4_font_bitmap.c').read_text()
    for forbidden in ('malloc(', 'calloc(', 'fopen(', 'xTaskCreate', 'vTaskDelay', 'audio_', 'owner_open'):
        assert forbidden not in implementation
    assert 'static EXT_RAM_BSS_ATTR tinfl_decompressor inflater;' in implementation
    assert 'if(attempted)return ready;' in implementation
