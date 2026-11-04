"""Actual fixed-cue decoder with a host zlib adapter, not a P4 timing/budget claim."""
from pathlib import Path
import hashlib
import importlib.util
import re
import subprocess
import sys
import wave

import pytest

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('permission_request', 'permission_approved', 'permission_denied',
         'permission_expired', 'session_terminated', 'done', 'error')
GOLDEN = ('966df0b88c22b9ec8437bce73ba5123b36be5839e410b38b63696a068f6bfdd2',
          '543f4e133c49f8c87428ac0e30716bd8bfa9d1d0fd4fc1fa1c4db94f29dc7612',
          '7cceff5b426547e3fa1eeb3bd585367df6bc612f2ff4adb466dcc029f20101bd',
          '71a733ca2764e017e05f7ceee0ece8b59663d73e882b247c2161b2d4f37d3162',
          'cf2a912f56211107a7a91edd9cb041f345d3c5b03164a1284d86e9f58944b3f6',
          '4455ff7e52e403b34bf6a112d4d8ddb8664034ad84054f5e151bb655e1c273f8',
          'f197ca531f46afba40af0df2e464d22c50b88ccfacf653300d068d2e5581f751')
MINIZ = r'''
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
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <zlib.h>
#include "pet_p4_cue_packed.c"
#include "pet_p4_system_cues.h"
static unsigned calls,fail_at,kind;
static const char *names[]={"permission_request","permission_approved","permission_denied","permission_expired","session_terminated","done","error"};
tinfl_status tinfl_decompress(tinfl_decompressor *r,const uint8_t *src,size_t *in,
    uint8_t *start,uint8_t *out,size_t *length,unsigned flags) {
  ++calls;assert(calls<=PET_CUE_PACKED_COUNT && r==&cue_inflater && start==cue_temp && out==cue_temp);
  assert(*in==descriptors[calls-1].packed_size && *length==descriptors[calls-1].pcm_size);
  assert(flags==(TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  /* Recursive init/read must neither expose partial PCM nor erase the caller. */
  const uint8_t *p=(const uint8_t*)1;size_t n=99;
  assert(!pet_p4_system_cues_init());
  assert(!pet_p4_system_cue_pcm(names[0],&p,&n) && !p && n==0);
  z_stream z={0};z.next_in=(void*)src;z.avail_in=(uInt)*in;
  z.next_out=out;z.avail_out=(uInt)*length;
  assert(inflateInit(&z)==Z_OK);int result=inflate(&z,Z_FINISH);
  *in=z.total_in;*length=z.total_out;r->adler=(unsigned)z.adler;
  assert(inflateEnd(&z)==Z_OK && result==Z_STREAM_END);
  if(calls==fail_at){
    if(kind==0)return -1;
    if(kind==1)--*in;
    if(kind==2)--*length;
    if(kind==3)r->adler^=1;
    if(kind==4)out[0]^=1; /* dishonest/stale Adler is still caught by PCM checksum */
    if(kind==5)++*in;
    if(kind==6)++*length;
  }
  return TINFL_STATUS_DONE;
}
static void all_zero(const void *bytes,size_t size){const uint8_t *p=bytes;for(size_t i=0;i<size;i++)assert(!p[i]);}
static void compare_file(const char *path,const uint8_t *bytes,size_t size){
  FILE *f=fopen(path,"rb");assert(f);uint8_t scratch[1024];size_t total=0,n;
  while((n=fread(scratch,1,sizeof scratch,f))){assert(total+n<=size && !memcmp(bytes+total,scratch,n));total+=n;}
  assert(total==size && !ferror(f) && !fclose(f));
}
static void *read_ready(void *unused){
  (void)unused;
  for(unsigned round=0;round<20;round++)for(unsigned i=0;i<PET_CUE_PACKED_COUNT;i++){
    const uint8_t *p=NULL;size_t n=0;
    assert(pet_p4_system_cue_pcm(names[i],&p,&n));
    assert(p==cue_pcm+descriptors[i].pcm_offset && n==descriptors[i].pcm_size);
  }
  return NULL;
}
int main(int argc,char **argv){
  assert(argc==(int)PET_CUE_PACKED_COUNT+5);fail_at=(unsigned)atoi(argv[1]);kind=(unsigned)atoi(argv[2]);
  const uint8_t *p=(const uint8_t*)1;size_t n=99;
  assert(!pet_p4_system_cue_pcm(names[0],&p,&n) && !p && !n);
  assert(!pet_p4_cue_packed_pcm(99,&p,&n) && !p && !n);
  assert(!pet_p4_cue_packed_pcm(0,NULL,&n));
  assert(!pet_p4_cue_packed_pcm(0,&p,NULL));
  bool ok=pet_p4_system_cues_init();assert(ok==!fail_at);
  unsigned once=calls;assert(pet_p4_system_cues_init()==ok && calls==once);
  all_zero(cue_temp,sizeof cue_temp);all_zero(&cue_inflater,sizeof cue_inflater);
  if(!ok){
    assert(calls==fail_at);all_zero(cue_pcm,sizeof cue_pcm);
    for(unsigned i=0;i<PET_CUE_PACKED_COUNT;i++)assert(!pet_p4_system_cue_pcm(names[i],&p,&n) && !p && !n);
    return 0;
  }
  assert(calls==PET_CUE_PACKED_COUNT);
  for(unsigned i=0;i<PET_CUE_PACKED_COUNT;i++){
    assert(pet_p4_system_cue_pcm(names[i],&p,&n));compare_file(argv[3+i],p,n);
  }
  const uint8_t *error_pcm=NULL,*waiting_pcm=NULL;size_t error_size=0,waiting_size=0;
  assert(pet_p4_system_cue_pcm("error",&error_pcm,&error_size));
  assert(pet_p4_system_cue_pcm("waiting_user",&waiting_pcm,&waiting_size));
  assert(error_pcm==waiting_pcm && error_size==waiting_size);
  pthread_t readers[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&readers[i],NULL,read_ready,NULL));
  for(unsigned i=0;i<4;i++)assert(!pthread_join(readers[i],NULL));
  assert(calls==PET_CUE_PACKED_COUNT);
  /* The actual inverse on wraparound/negative/extreme samples, independent
   * Python oracle files; startup's fixed factory data are not the only vectors. */
  FILE *f=fopen(argv[3+PET_CUE_PACKED_COUNT],"rb");assert(f);uint8_t planes[2048],pcm[2048];
  size_t size=fread(planes,1,sizeof planes,f);assert(size>0 && !(size&1) && !ferror(f) && !fclose(f));
  (void)restore_pcm(pcm,planes,size);compare_file(argv[4+PET_CUE_PACKED_COUNT],pcm,size);
  assert(pet_p4_system_cue_legacy("done",UINT64_C(0x392e208e77b6a2a4)));
  assert(pet_p4_system_cue_legacy("error",UINT64_C(0x85ab7ddd599f8277)));
  assert(!pet_p4_system_cue_legacy("done",UINT64_C(12345)));
  return 0;
}
'''


@pytest.fixture(scope='module')
def decoder(tmp_path_factory):
    tmp = tmp_path_factory.mktemp('cue-packed')
    (tmp/'sdkconfig.h').write_text('#define CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY 1\n')
    (tmp/'esp_attr.h').write_text('#pragma once\n#define EXT_RAM_BSS_ATTR\n')
    (tmp/'miniz.h').write_text(MINIZ)
    source = tmp/'main.c'
    source.write_text(HARNESS)
    exe = tmp/'decoder'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
                    '-I', str(tmp), '-I', str(ROOT/'main'), str(source),
                    str(ROOT/'main/pet_p4_system_cues.c'), '-lz', '-o', str(exe)], check=True)
    paths = []
    for name, golden in zip(NAMES, GOLDEN):
        with wave.open(str(ROOT.parent/'pc/public/terrier-clips'/f'{name}.wav'), 'rb') as wav:
            raw = wav.readframes(wav.getnframes())
        assert hashlib.sha256(raw).hexdigest() == golden
        path = tmp/(name+'.pcm');path.write_bytes(raw);paths.append(str(path))
    spec = importlib.util.spec_from_file_location('pack_cues', ROOT/'tools/compress_system_cues.py')
    module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    values = [0, 1, 65535, 32768, 32767, 16384, 49152]*100
    raw = b''.join(value.to_bytes(2,'little') for value in values)
    (tmp/'edges.pcm').write_bytes(raw);(tmp/'edges.planes').write_bytes(module.transform(raw))
    return exe, [*paths, str(tmp/'edges.planes'), str(tmp/'edges.pcm')]


@pytest.mark.parametrize('stage,kind', [(0, 0)] + [(stage,kind) for stage in range(1,8) for kind in range(7)])
def test_actual_startup_exactness_failure_latch_and_immutable_readers(decoder, stage, kind):
    exe, paths = decoder
    subprocess.run([str(exe), str(stage), str(kind), *paths], check=True)


def test_reproducible_input_and_no_raw_double_embedding():
    subprocess.run([sys.executable, str(ROOT/'tools/compress_system_cues.py'), '--check'], check=True)
    source = (ROOT/'main/pet_p4_cue_packed.c').read_text()
    for name in ['cue_pcm','cue_temp','cue_inflater']:
        assert re.search(r'static EXT_RAM_BSS_ATTR [^;]*\b'+name+r'\b', source)
    assert 'malloc(' not in source and 'calloc(' not in source
    cmake = (ROOT/'main/CMakeLists.txt').read_text()
    assert '"pet_p4_cue_packed.c"' in cmake and 'compress_system_cues.py' in cmake
    assert 'set(cue_input' not in cmake
    assert 'foreach(cue done error)' not in cmake
    ini=(ROOT/'platformio.ini').read_text()
    for name in NAMES:
        assert name+'.wav' not in ini
        assert '_binary_'+name+'_wav' not in (ROOT/'main/pet_p4_system_cues.c').read_text()
    main = (ROOT/'main/pet_p4_main.c').read_text()
    assert main.index('pet_p4_system_cues_init()') < main.index('pet_p4_audio_init(board_device_id')


def test_internal_ram_fallback_is_rejected(tmp_path):
    (tmp_path/'esp_attr.h').write_text('#pragma once\n#define EXT_RAM_BSS_ATTR\n')
    (tmp_path/'miniz.h').write_text(MINIZ)
    (tmp_path/'sdkconfig.h').write_text('#define CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY 0\n')
    result = subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                             '-I',str(tmp_path),'-I',str(ROOT/'main'),'-fsyntax-only',
                             str(ROOT/'main/pet_p4_cue_packed.c')], capture_output=True, text=True)
    assert result.returncode != 0
    assert 'internal RAM fallback is forbidden' in result.stderr
