"""Actual startup/data/routing with host zlib ROM adapter; no P4 acceptance claim."""
from pathlib import Path
import hashlib,importlib.util,re,struct,subprocess,sys,zlib
import pytest
ROOT=Path(__file__).resolve().parents[1]
LVGL=ROOT/'managed_components/lvgl__lvgl'

def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def values(source,name):
 body=re.search(r'\b'+name+r'\[\]\s*=\s*\{(.*?)\n\};',source,re.S)[1]
 return [int(n,0) for n in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b',re.sub(r'/\*.*?\*/','',body,flags=re.S))]

def descriptors(source):
 return [tuple(map(int,row)) for row in re.findall(r'\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)',source)]

def test_exact_pr12_pixels_metrics_mapping_and_identity_asset():
 p=load('native_pack',ROOT/'tools/compress_font_native32.py');source,raw=p.source_data()
 text=source.decode();actual=(ROOT/'main/pet_p4_font_cn_32.c').read_text()
 assert descriptors(text)==descriptors(actual) and len(descriptors(actual))==200
 assert values(text,'unicode_list_0')==values(actual,'unicode_list_0') and len(values(actual,'unicode_list_0'))==199
 packed=(ROOT/'assets/fonts/pet_cn_32.deflate').read_bytes()
 assert len(packed)==14768 and zlib.decompress(packed)==raw
 inc=(ROOT/'main/pet_p4_font_native32_packed.inc').read_text()
 assert bytes(int(v,16) for v in re.findall(r'0x([a-f0-9]{2})',inc))==packed
 for field in ['.line_height = 36','.base_line = 6','.bpp = 2','.bitmap_format = 0']:
  assert field in text and field in actual
 assert 'MiSans Fonts Intellectual Property License Agreement' in actual
 assert '85e194d9d970513c6a475a33ce8cabe0c5c72603' in actual
 subprocess.run([sys.executable,str(ROOT/'tools/compress_font_native32.py'),'--check'],check=True)

MINIZ=r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {unsigned state,adler;} tinfl_decompressor;
typedef int tinfl_status;
#define TINFL_STATUS_DONE 0
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 4
#define tinfl_init(r) ((r)->state=0)
#define tinfl_get_adler32(r) ((r)->adler)
tinfl_status tinfl_decompress(tinfl_decompressor*,const uint8_t*,size_t*,uint8_t*,uint8_t*,size_t*,unsigned);
'''
HARNESS=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "pet_p4_font_bitmap.c"
#include "pet_p4_font_cn_32.c"
bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t*f,lv_font_glyph_dsc_t*g,uint32_t cp,uint32_t next){(void)f;(void)g;(void)cp;(void)next;return false;}
const void*lv_font_get_bitmap_fmt_txt(lv_font_glyph_dsc_t*g,lv_draw_buf_t*b){(void)g;(void)b;return NULL;}
static unsigned calls;static int stage,kind;
tinfl_status tinfl_decompress(tinfl_decompressor*r,const uint8_t*src,size_t*in,uint8_t*start,uint8_t*out,size_t*length,unsigned flags){
 calls++;assert(r==&inflater&&start==out);
 assert(flags==(TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
 assert(!pet_p4_font_bitmap_init());
 const size_t expected[]={0,169664,554086,30579};assert(calls<4&&*length==expected[calls]);
 assert(out==(calls<3?pet_font_bitmap:pet_font_native32_bitmap));
 z_stream z={0};z.next_in=(void*)src;z.avail_in=(uInt)*in;z.next_out=out;z.avail_out=(uInt)*length;
 assert(inflateInit(&z)==Z_OK);int result=inflate(&z,Z_FINISH);*in=z.total_in;*length=z.total_out;r->adler=(unsigned)z.adler;assert(inflateEnd(&z)==Z_OK);
 if((int)calls==stage){if(kind==0)return -1;if(kind==1)--*in;if(kind==2)--*length;if(kind==3)r->adler++;}
 return result==Z_STREAM_END?TINFL_STATUS_DONE:-1;
}
static void exact(const char*path,const uint8_t*actual,size_t size){FILE*f=fopen(path,"rb");assert(f);uint8_t b[4096];size_t n,at=0;while((n=fread(b,1,sizeof b,f))){assert(at+n<=size&&!memcmp(actual+at,b,n));at+=n;}assert(at==size&&!ferror(f)&&!fclose(f));}
int main(int argc,char**argv){
 assert(argc==6);stage=atoi(argv[1]);kind=atoi(argv[2]);
 assert(!pet_p4_font_native32_has_glyph('A'));
 bool ok=pet_p4_font_bitmap_init();assert(ok==(stage==0));
 unsigned count=calls;assert(pet_p4_font_bitmap_init()==ok&&calls==count);
 assert(pet_p4_font_native32_prepare(&inflater)==ok&&calls==count);
 if(!ok){
  assert(calls==(unsigned)stage);
  for(size_t i=0;i<sizeof pet_font_bitmap;i++)assert(!pet_font_bitmap[i]);
  const uint8_t*p=(const uint8_t*)pet_font_metrics;for(size_t i=0;i<sizeof pet_font_metrics;i++)assert(!p[i]);
  for(size_t i=0;i<sizeof pet_font_kern_left;i++)assert(!pet_font_kern_left[i]&&!pet_font_kern_right[i]);
  for(size_t i=0;i<sizeof pet_font_native32_bitmap;i++)assert(!pet_font_native32_bitmap[i]);
  assert(!pet_p4_font_native32_has_glyph('A'));return 0;
 }
 assert(calls==3);exact(argv[3],pet_font_bitmap,sizeof pet_font_bitmap);exact(argv[4],pet_font_native32_bitmap,sizeof pet_font_native32_bitmap);
 FILE*f=fopen(argv[5],"rb");assert(f);uint8_t b[8];for(size_t i=0;i<PET_FONT_METRIC_RECORDS;i++){
  assert(fread(b,1,8,f)==8);uint32_t word=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);int8_t x,y;memcpy(&x,b+6,1);memcpy(&y,b+7,1);
  const lv_font_fmt_txt_glyph_dsc_t*g=&pet_font_metrics[i];assert(g->bitmap_index==(word&0xfffff)&&g->adv_w==word>>20&&g->box_w==b[4]&&g->box_h==b[5]&&g->ofs_x==x&&g->ofs_y==y);
 }assert(fgetc(f)==EOF&&!fclose(f));
 assert(pet_p4_font_cn_32.line_height==36&&pet_p4_font_cn_32.base_line==6&&font_dsc.bpp==2);
 for(uint32_t cp=0;cp<65536;cp++){
  bool wanted=false;for(unsigned i=0;i<199;i++)if(cp==32U+(uint32_t)unicode_list_0[i])wanted=true;
  assert(pet_p4_font_native32_has_glyph(cp)==wanted);
 }
 pet_p4_font_native32_clear();assert(!pet_p4_font_native32_prepare(&inflater)&&calls==count);return 0;
}
'''

def asm_assets(tmp_path):
 section='.section __TEXT,__const' if sys.platform=='darwin' else '.section .rodata'
 text=''
 for short in ['pet_cn_16','pet_cn_metrics','pet_cn_metrics_columns']:
  text+=section+'\n.globl _binary_'+short+'_deflate_start\n.globl _binary_'+short+'_deflate_end\n_binary_'+short+'_deflate_start:\n.incbin "'+str(ROOT/'assets/fonts'/(short+'.deflate'))+'"\n_binary_'+short+'_deflate_end:\n'
 p=tmp_path/'assets.S';p.write_text(text);return p

def test_actual_integrated_startup_order_shared_inflater_and_failure_latches(tmp_path):
 (tmp_path/'esp_attr.h').write_text('#pragma once\n#define EXT_RAM_BSS_ATTR\n')
 (tmp_path/'miniz.h').write_text(MINIZ)
 p=load('native32_pack_test',ROOT/'tools/compress_font_native32.py');_,raw=p.source_data()
 bitmap=zlib.decompress((ROOT/'assets/fonts/pet_cn_16.deflate').read_bytes())
 metric=zlib.decompress((ROOT/'assets/fonts/pet_cn_metrics.deflate').read_bytes())
 for name,data in [('bitmap.bin',bitmap),('native32.bin',raw),('metrics.bin',metric)]: (tmp_path/name).write_bytes(data)
 src=tmp_path/'init.c';src.write_text(HARNESS);exe=tmp_path/'init'
 subprocess.run(['cc','-std=gnu11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DLV_CONF_SKIP',
  '-DPET_FONT_NATIVE_32=1','-DPET_FONT_BITMAP_PACKED=1','-DPET_FONT_METRICS_PACKED=1',
  '-I',str(tmp_path),'-I',str(ROOT/'main'),'-I',str(LVGL),str(src),str(asm_assets(tmp_path)),'-lz','-o',str(exe)],check=True)
 for stage,kind in [(0,0),*((s,k) for s in [1,2,3] for k in range(4))]:
  subprocess.run([str(exe),str(stage),str(kind),*(str(tmp_path/n) for n in ['bitmap.bin','native32.bin','metrics.bin'])],check=True,timeout=10)


def test_default_off_module_empty_and_invalid_parent_refuses_compile(tmp_path):
 src=ROOT/'main/pet_p4_font_cn_32.c';obj=tmp_path/'off.o'
 common=['cc','-std=c11','-Wall','-Wextra','-Werror','-DLV_CONF_SKIP','-I',str(ROOT/'main'),'-I',str(LVGL)]
 subprocess.run([*common,'-c',str(src),'-o',str(obj)],check=True)
 symbols=subprocess.check_output(['nm',str(obj)],text=True)
 assert 'pet_p4_font_cn_32' not in symbols and 'pet_font_native32' not in symbols
 for flags in [[],['-DPET_FONT_BITMAP_PACKED=1']]:
  result=subprocess.run([*common,'-DPET_FONT_NATIVE_32=1',*flags,'-fsyntax-only',str(src)],capture_output=True,text=True)
  assert result.returncode and 'requires PET_FONT_BITMAP_PACKED and PET_FONT_METRICS_PACKED' in result.stderr


def test_actual_renderer_routes_only_scale2_and_reads_baseline_before_release(tmp_path):
 from p4_render_trace_test import function
 text=(ROOT/'main/pet_p4_renderer.c').read_text()
 helpers=function(text,'scaled_glyph_dsc')+function(text,'scaled_glyph_baseline')+function(text,'glyph_visual_bounds')+function(text,'glyph_width')+function(text,'draw_lvgl_glyph')
 code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct{int line_height,base_line;}lv_font_t;
typedef struct{const lv_font_t*resolved_font;int adv_w,box_w,box_h,ofs_x,ofs_y,req_raw_bitmap;}lv_font_glyph_dsc_t;
static const lv_font_t pet_p4_font_cn_16={18,3},pet_p4_font_cn_32={36,6};
static unsigned native_calls,legacy_calls,releases;
static bool pet_p4_font_native32_has_glyph(uint32_t cp){return cp=='A';}
static bool lv_font_get_glyph_dsc(const lv_font_t*f,lv_font_glyph_dsc_t*g,uint32_t cp,uint32_t next){(void)next;assert(cp);if(f==&pet_p4_font_cn_32)native_calls++;else legacy_calls++;*g=(lv_font_glyph_dsc_t){.resolved_font=f,.adv_w=f==&pet_p4_font_cn_32?19:9,.box_w=2,.box_h=4,.ofs_y=-2};return true;}
static uint8_t glyph_alpha_at(const uint8_t*b,const lv_font_glyph_dsc_t*g,int x,int y){(void)b;(void)g;(void)x;(void)y;return 255;}
static const void*bitmap(lv_font_glyph_dsc_t*g,void*b){(void)g;(void)b;static const uint8_t data[4]={255,255,255,255};return data;}
static void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t*g){releases++;memset(g,0,sizeof(*g));}
'''
 # Minimal adapter carries the real callback access used by bounds.
 code=code.replace('typedef struct{int line_height,base_line;}lv_font_t;', 'typedef struct glyph lv_font_glyph_dsc_t;\ntypedef struct{int line_height,base_line;const void*(*get_glyph_bitmap)(lv_font_glyph_dsc_t*,void*);}lv_font_t;\nstatic const void*bitmap(lv_font_glyph_dsc_t*,void*);')
 code=code.replace('typedef struct{const lv_font_t*resolved_font;int adv_w,box_w,box_h,ofs_x,ofs_y,req_raw_bitmap;}lv_font_glyph_dsc_t;', 'struct glyph{const lv_font_t*resolved_font;int adv_w,box_w,box_h,ofs_x,ofs_y,req_raw_bitmap;};')
 code=code.replace('={18,3},pet_p4_font_cn_32={36,6};','={18,3,bitmap},pet_p4_font_cn_32={36,6,bitmap};')
 code+=r'''
static unsigned pixels;static int min_x,min_y,max_x,max_y;
static void blend_px(int x,int y,uint8_t r,uint8_t g,uint8_t b,uint8_t a){(void)r;(void)g;(void)b;assert(a==255);if(!pixels){min_x=max_x=x;min_y=max_y=y;}else{if(x<min_x)min_x=x;if(x>max_x)max_x=x;if(y<min_y)min_y=y;if(y>max_y)max_y=y;}pixels++;}
static void fill_ellipse(int x,int y,int rx,int ry,uint16_t c){(void)x;(void)y;(void)rx;(void)ry;(void)c;assert(0);}
static void draw_line(int x,int y,int x2,int y2,uint16_t c){(void)x;(void)y;(void)x2;(void)y2;(void)c;assert(0);}
static void draw_glyph5x7(int x,int y,char ch,uint16_t c,int scale){(void)x;(void)y;(void)ch;(void)c;(void)scale;assert(0);}
'''
 code+=helpers+r'''
int main(void){
 lv_font_glyph_dsc_t g;for(int scale=1;scale<=3;scale++){int actual=scale;assert(scaled_glyph_dsc('A',0,&g,&actual));assert(g.resolved_font==(scale==2?&pet_p4_font_cn_32:&pet_p4_font_cn_16));assert(actual==(scale==2?1:scale));}
 int scale=2;assert(scaled_glyph_dsc(0x9f98,0,&g,&scale)&&scale==2&&g.resolved_font==&pet_p4_font_cn_16);
 unsigned before=native_calls;assert(glyph_width('A',0)==9&&native_calls==before);assert(glyph_width(0xfe0f,0)==0);
 int top,bottom;assert(glyph_visual_bounds('A',0,2,&top,&bottom)&&top==28&&bottom==31);
 assert(glyph_visual_bounds(0x9f98,0,2,&top,&bottom)&&top==26&&bottom==33);assert(releases==2);
 pixels=0;draw_lvgl_glyph('A',0,10,20,0xffff,2);assert(pixels==8&&min_x==10&&max_x==11&&min_y==48&&max_y==51);
 pixels=0;draw_lvgl_glyph(0x9f98,0,10,20,0xffff,2);assert(pixels==32&&min_x==10&&max_x==13&&min_y==46&&max_y==53);
 assert(releases==4);return 0;
}
'''
 src=tmp_path/'route.c';src.write_text(code);exe=tmp_path/'route'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DPET_FONT_NATIVE_32=1',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)
