"""All licensed metric fields, real startup decoder/latch, no font regeneration."""
from pathlib import Path
import hashlib
import importlib.util
import subprocess
import sys
import zlib
ROOT=Path(__file__).resolve().parents[1]


def load(name,file):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/file)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module


def test_all_original_metric_records_and_actual_lvgl_descriptors():
    pack=load('metric_records','compress_font_metrics.py');raw=pack.metrics_bytes()
    assert len(raw)==169664 and hashlib.sha256(raw).hexdigest()=='63e69048a92d090d59cd7466ff6554e98055dba83bd9da69205099a600a9bb49'
    assert zlib.decompress((ROOT/'assets/fonts/pet_cn_metrics.deflate').read_bytes())==raw
    subprocess.run([sys.executable,str(ROOT/'tools/compress_font_metrics.py'),'--check'],check=True)
    for bitmap,metrics in ((0,0),(1,0),(1,1)):
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DLV_CONF_SKIP',
          f'-DPET_FONT_BITMAP_PACKED={bitmap}',f'-DPET_FONT_METRICS_PACKED={metrics}',
          '-I',str(ROOT/'managed_components/lvgl__lvgl'),'-I',str(ROOT/'main'),
          '-fsyntax-only',str(ROOT/'main/pet_p4_font_cn.c')],check=True)
    text=(ROOT/'main/pet_p4_font_cn.c').read_text()
    assert 'MiSans Fonts Intellectual Property License Agreement' in text
    assert '.glyph_dsc = PET_FONT_GLYPH_METRICS' in text and '#define glyph_dsc ' not in text


def test_actual_combined_init_reconstructs_all_fields_and_latches_failures(tmp_path):
    (tmp_path/'esp_attr.h').write_text('#pragma once\n#define EXT_RAM_BSS_ATTR\n')
    (tmp_path/'miniz.h').write_text(r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct{unsigned m_state,m_check_adler32;}tinfl_decompressor;
typedef int tinfl_status;
#define TINFL_STATUS_DONE 0
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 4
#define tinfl_init(r) ((r)->m_state=0)
#define tinfl_get_adler32(r) ((r)->m_check_adler32)
tinfl_status tinfl_decompress(tinfl_decompressor*,const unsigned char*,size_t*,unsigned char*,unsigned char*,size_t*,unsigned);
''')
    code=r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "pet_p4_font_bitmap.c"
static int mode,calls;
tinfl_status tinfl_decompress(tinfl_decompressor*r,const unsigned char*src,size_t*in,
 unsigned char*start,unsigned char*out,size_t*length,unsigned flags){
 calls++;assert(start==out&&flags==(TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
 assert(!pet_p4_font_bitmap_init()); /* No partial-ready escape on reentry. */
 z_stream z={0};z.next_in=(void*)src;z.avail_in=(uInt)*in;z.next_out=out;z.avail_out=(uInt)*length;
 assert(inflateInit(&z)==Z_OK);int result=inflate(&z,Z_FINISH);
 *in=z.total_in;*length=z.total_out;r->m_check_adler32=(unsigned)z.adler;assert(inflateEnd(&z)==Z_OK);
 if(mode==5&&calls==1)return -1;
 if(calls==2){
  if(mode==1)return -1;
  if(mode==2)--*in;
  if(mode==3)--*length;
  if(mode==4)r->m_check_adler32++;
  if(mode==6){out[0]=out[1]=out[2]=out[3]=255;}
  if(mode==7){uint32_t index=554080;for(unsigned i=0;i<4;i++)out[i]=(uint8_t)(index>>(i*8));out[4]=out[5]=255;}
 }
 return result==Z_STREAM_END?TINFL_STATUS_DONE:-1;
}
static void bitmap_equal(const char*path){
 FILE*f=fopen(path,"rb");assert(f);uint8_t expected[4096];size_t offset=0,n;
 while((n=fread(expected,1,sizeof(expected),f))){assert(offset+n<=sizeof(pet_font_bitmap));assert(!memcmp(pet_font_bitmap+offset,expected,n));offset+=n;}
 assert(!ferror(f)&&!fclose(f)&&offset==sizeof(pet_font_bitmap));
}
static void metrics_equal(const char*path){
 FILE*f=fopen(path,"rb");assert(f);uint8_t p[8];
 for(unsigned i=0;i<PET_FONT_METRIC_RECORDS;i++){
  assert(fread(p,1,8,f)==8);uint32_t word=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);int8_t ox,oy;memcpy(&ox,p+6,1);memcpy(&oy,p+7,1);
  const lv_font_fmt_txt_glyph_dsc_t*g=&pet_font_metrics[i];
  assert(g->bitmap_index==(word&0xfffff)&&g->adv_w==word>>20&&g->box_w==p[4]&&g->box_h==p[5]&&g->ofs_x==ox&&g->ofs_y==oy);
 }
 assert(fgetc(f)==EOF&&!ferror(f)&&!fclose(f));
}
int main(int argc,char**argv){
 assert(argc==4);mode=atoi(argv[1]);bool ok=pet_p4_font_bitmap_init();assert(ok==(mode==0));
 int expected=mode==5||mode==8?1:2;assert(calls==expected&&pet_p4_font_bitmap_init()==ok&&calls==expected);
 if(ok){bitmap_equal(argv[2]);metrics_equal(argv[3]);}
 else{
  for(size_t i=0;i<sizeof(pet_font_bitmap);i++)assert(!pet_font_bitmap[i]);
  const uint8_t*p=(const uint8_t*)pet_font_metrics;for(size_t i=0;i<sizeof(pet_font_metrics);i++)assert(!p[i]);
 }
 return 0;
}
'''
    bitmap=load('metrics_bitmap','compress_font_asset.py').bitmap_bytes()
    metrics=load('metrics_canonical','compress_font_metrics.py').metrics_bytes()
    (tmp_path/'bitmap.bin').write_bytes(bitmap);(tmp_path/'metrics.bin').write_bytes(metrics)
    section='.section __TEXT,__const' if sys.platform=='darwin' else '.section .rodata'
    def assembly(short=False):
        code=section+'\n'
        for name,file in (('pet_cn_16','pet_cn_16.deflate'),('pet_cn_metrics','pet_cn_metrics.deflate')):
            asset=ROOT/'assets/fonts'/file
            code+=f'.globl _binary_{name}_deflate_start\n.globl _binary_{name}_deflate_end\n_binary_{name}_deflate_start:\n'
            code+=f'.incbin "{asset}"'+(f',0,{asset.stat().st_size-1}' if short and name=='pet_cn_metrics' else '')+'\n'
            code+=f'_binary_{name}_deflate_end:\n'
        return code
    src=tmp_path/'metrics.c';src.write_text(code)
    for short in (False,True):
        asm=tmp_path/('short.S' if short else 'full.S');asm.write_text(assembly(short));exe=tmp_path/('short' if short else 'full')
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DLV_CONF_SKIP',
          '-DPET_FONT_BITMAP_PACKED=1','-DPET_FONT_METRICS_PACKED=1','-I',str(tmp_path),'-I',str(ROOT/'main'),
          '-I',str(ROOT/'managed_components/lvgl__lvgl'),str(src),str(asm),'-lz','-o',str(exe)],check=True)
        for mode in ((8,) if short else range(8)):
            subprocess.run([str(exe),str(mode),str(tmp_path/'bitmap.bin'),str(tmp_path/'metrics.bin')],check=True,timeout=10)


def test_metadata_parent_and_boot_only_boundary(tmp_path):
    import configparser
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_glyph_rows_parent':assert 'PET_FONT_METRICS_PACKED=1' in flags
        else:assert 'PET_FONT_METRICS_PACKED=1' not in flags
    # CMake and PlatformIO own separate embedding graphs. Both declarations
    # are required; a host incbin test cannot prove the SCons .S was generated.
    embedded=[section for section in config.sections() if 'board_build.embed_files' in config[section]]
    assert len(embedded)==1
    assert 'assets/fonts/pet_cn_metrics.deflate' in config[embedded[0]]['board_build.embed_files'].splitlines()
    assert 'target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pet_cn_metrics.deflate" BINARY)' in (ROOT/'main/CMakeLists.txt').read_text()
    implementation=(ROOT/'main/pet_p4_font_bitmap.c').read_text()
    assert 'ready=bitmap_ok&&prepare_metrics()&&prepare_kerning();' in implementation
    for forbidden in ('malloc(', 'calloc(', 'fopen(', 'xTaskCreate', 'vTaskDelay', 'audio_', 'owner_open'):
        assert forbidden not in implementation
    src=tmp_path/'parent.c';src.write_text('#include "pet_p4_font_bitmap.h"\nint main(void){return 0;}\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_FONT_METRICS_PACKED=1','-I',str(ROOT/'main'),
      '-I',str(ROOT/'managed_components/lvgl__lvgl'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'Packed metrics require the reviewed packed-bitmap startup parent' in result.stderr
