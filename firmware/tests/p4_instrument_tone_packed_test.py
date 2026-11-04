"""Lossless candidate tone data and unchanged production PCM; no device/audio IO."""
import importlib.util
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def packer():
    path=ROOT/'tools/compress_instrument_tone.py'
    spec=importlib.util.spec_from_file_location('instrument_tone_pack',path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


def test_pack_is_reproducible_exact_and_saves_at_least_three_kib():
    module=packer();raw=module.tone_bytes()
    assert len(raw)==16384 and module.fnv(raw)==0x6d95b941
    assert module.header()==module.header()==(ROOT/'main/pet_p4_instrument_tone_packed.h').read_text()
    import zlib
    packed=zlib.compress(raw,9)
    assert len(packed)==7360 and zlib.decompress(packed)==raw
    assert len(raw)-len(packed)>=3072


def test_real_boot_initializer_and_production_pcm_are_byte_exact_and_fail_closed(tmp_path):
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
    source=tmp_path/'tone.c'
    source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "pet_p4_font_bitmap.c"
#include "pet_p4_instrument_core.c"
static int mode;
#if PET_FONT_BITMAP_PACKED
static int calls;
tinfl_status tinfl_decompress(tinfl_decompressor*r,const unsigned char*src,size_t*in,
 unsigned char*start,unsigned char*out,size_t*length,unsigned flags){
 calls++;assert(start==out&&flags==(TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
 bool tone=src==pet_instrument_tone_packed;
 unsigned char corrupt[PET_INSTRUMENT_TONE_PACKED_BYTES];
 if(tone&&mode==6){memcpy(corrupt,src,*in);corrupt[40]^=0x40;src=corrupt;}
 if(tone&&mode==7)--*in;
 z_stream z={0};z.next_in=(void*)src;z.avail_in=(uInt)*in;z.next_out=out;z.avail_out=(uInt)*length;
 assert(inflateInit(&z)==Z_OK);int result=inflate(&z,Z_FINISH);
 *in=z.total_in;*length=z.total_out;r->m_check_adler32=(unsigned)z.adler;assert(inflateEnd(&z)==Z_OK);
 if(!tone&&mode==8)return -1;
 if(tone){
  if(mode==1)return -1;
  if(mode==2)--*in;
  if(mode==3)--*length;
  if(mode==4)r->m_check_adler32++;
  if(mode==5)out[1234]^=1; /* Valid decoder/adler claims but wrong output: FNV must reject. */
 }
 return result==Z_STREAM_END?TINFL_STATUS_DONE:-1;
}
#endif
int main(int argc,char**argv){
 assert(argc==2);mode=atoi(argv[1]);
#if PET_INSTRUMENT_TONE_PACKED
 memset(pet_instrument_tone_pcm,0xa5,sizeof(pet_instrument_tone_pcm));
#endif
 bool ok=pet_p4_font_bitmap_init();assert(ok==(mode==0));
 assert(pet_p4_font_bitmap_init()==ok);
#if PET_INSTRUMENT_TONE_PACKED
 assert(calls==(mode==8?1:2));
#endif
 if(!ok){
#if PET_INSTRUMENT_TONE_PACKED
  for(size_t i=0;i<PET_INSTRUMENT_TONE_SAMPLES;i++)assert(pet_instrument_tone_pcm[i]==0);
  for(size_t i=0;i<sizeof(pet_font_bitmap);i++)assert(pet_font_bitmap[i]==0);
#endif
  return 0;
 }
 assert(fwrite(pet_instrument_tone_pcm,1,sizeof(pet_instrument_tone_pcm),stdout)==sizeof(pet_instrument_tone_pcm));
 /* Same original C render loop, every output byte: all tones, reset/ramp,
  * four overlapping strikes, repeated hits, volume extremes and exit tail. */
 int16_t pcm[257];
 for(unsigned tone=0;tone<3;tone++)for(unsigned variant=0;variant<3;variant++){
  pet_instrument_core_t core;pet_instrument_reset(&core);
  for(unsigned block=0;block<72;block++){
   if(block<4||block==12||block==13||block==36)pet_instrument_strike(&core);
   pet_instrument_render_tone(&core,pcm,257,block<56,variant==0?0:variant==1?70:100,variant==0?0:variant==1?18:100,tone);
   assert(fwrite(pcm,sizeof(*pcm),257,stdout)==257);
  }
 }
 return 0;
}
''')
    section='.section __TEXT,__const' if sys.platform=='darwin' else '.section .rodata'
    assembly=tmp_path/'font.S'
    assembly.write_text(section+'\n.globl _binary_pet_cn_16_deflate_start\n.globl _binary_pet_cn_16_deflate_end\n_binary_pet_cn_16_deflate_start:\n.incbin "'+str(ROOT/'assets/fonts/pet_cn_16.deflate')+'"\n_binary_pet_cn_16_deflate_end:\n')
    outputs=[]
    for packed in (0,1):
        binary=tmp_path/f'tone-{packed}'
        command=['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                 f'-DPET_INSTRUMENT_TONE_PACKED={packed}',f'-DPET_FONT_BITMAP_PACKED={packed}',
                 '-I',str(tmp_path),'-I',str(ROOT/'main'),str(source)]
        if packed:command.append(str(assembly))
        subprocess.run(command+['-lm','-lz','-o',str(binary)],check=True)
        outputs.append(subprocess.run([str(binary),'0'],capture_output=True,check=True,timeout=20).stdout)
        if packed:
            for mode in range(1,9):
                subprocess.run([str(binary),str(mode)],capture_output=True,check=True,timeout=10)
    assert outputs[0][:16384]==packer().tone_bytes()
    assert len(outputs[0])==16384+3*3*72*257*2
    assert outputs[1]==outputs[0]


def test_flag_requires_boot_parent_and_only_candidate_parent_enables_it(tmp_path):
    import configparser
    p=tmp_path/'guard.c';p.write_text('#include "pet_p4_instrument_tone_table.h"\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_INSTRUMENT_TONE_PACKED=1',
                           '-I',str(ROOT/'main'),str(p)],capture_output=True,text=True)
    assert result.returncode and 'requires the packed-font startup' in result.stderr
    cfg=configparser.ConfigParser(interpolation=None);cfg.read(ROOT/'platformio.ini')
    enabled=[name for name in cfg.sections() if '-DPET_INSTRUMENT_TONE_PACKED=1' in cfg[name].get('build_flags','')]
    assert enabled==['env:esp32_p4_capability_parent']
    # This data flag adds no new undef to the reviewed public admission list.
    flags=cfg['env:esp32_p4_program_public_candidate']['build_flags']
    assert 'PET_INSTRUMENT_TONE_PACKED' not in flags
    font=(ROOT/'main/pet_p4_font_bitmap.c').read_text()
    assert font.index('ready=ready&&prepare_instrument_tone();')<font.index('  return ready;')
    boot=(ROOT/'main/pet_p4_main.c').read_text().split('void app_main(void) {',1)[1]
    assert boot.index('pet_p4_font_bitmap_init()')<boot.index('pet_p4_audio_init(')
