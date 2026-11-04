"""Actual aligned-word clean-frame helper versus byte-exact libc copying."""
from pathlib import Path
import subprocess
import pytest
ROOT=Path(__file__).resolve().parents[1]

@pytest.mark.parametrize('enabled',[0,1])
def test_clean_frame_copy_alignment_tails_and_full_frame_guards(tmp_path,enabled):
    src=tmp_path/'copy.c';exe=tmp_path/'copy'
    src.write_text(r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_app_cache.h"
static unsigned char source[614464],actual[614464],expected[614464];
int main(void){
 for(unsigned i=0;i<sizeof(source);i++)source[i]=(unsigned char)(i*131U+47U);
 const unsigned sizes[]={0,1,2,3,4,7,8,15,16,31,32,33,63,64,127,128,255,256,1023,1024,1025};
 for(unsigned a=0;a<8;a++)for(unsigned b=0;b<8;b++)for(unsigned n=0;n<sizeof(sizes)/sizeof(*sizes);n++){
  memset(actual,0x73,4096);memset(expected,0x73,4096);
  memcpy(expected+16+a,source+16+b,sizes[n]);pet_app_frame_copy(actual+16+a,source+16+b,sizes[n]);
  assert(!memcmp(actual,expected,4096));
 }
 memset(actual,0x29,sizeof(actual));memset(expected,0x29,sizeof(expected));
 memcpy(expected+32,source+32,614400);pet_app_frame_copy(actual+32,source+32,614400);
 assert(!memcmp(actual,expected,sizeof(actual)));return 0;
}
''')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
      '-fsanitize=address,undefined',f'-DPET_P4_APP_COPY_WORDS={enabled}','-I',str(ROOT/'main'),
      '-I',str(ROOT/'tests/vendor/cjson'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=20)
