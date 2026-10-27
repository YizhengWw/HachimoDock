"""Host-executed production image decoder rejects truncation/overflow."""
import subprocess
import os
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def test_image_decoder(tmp_path):
    source=tmp_path/"test.c"
    source.write_text('''#include "pet_p4_image.h"
#include <assert.h>
#include <string.h>
int main(void){
 unsigned char b[1040]={0},out[64];memcpy(b,"P4I1",4);
 b[4]=8;b[5]=8;b[6]=1;b[8]=14;b[9]=4;
 b[1036]=191;b[1037]=3;
 assert(pet_image_decode(b,1038,out,64));
 for(int i=0;i<64;i++)assert(out[i]==3);
 assert(!pet_image_decode(b,1037,out,64));
 assert(!pet_image_decode(b,1038,out,63));
 b[1036]=192;assert(!pet_image_decode(b,1038,out,64));
 b[1036]=190;assert(!pet_image_decode(b,1038,out,64));
 b[1036]=191;b[4]=255;assert(!pet_image_decode(b,1038,out,64));
 memcpy(b,"P4I2",4);b[4]=8;b[5]=0;b[6]=8;b[7]=0;
 assert(pet_image_decode(b,1038,out,64));
 b[4]=1;b[5]=2;assert(!pet_image_decode(b,1038,out,64));
 b[4]=8;b[5]=0;b[6]=241;assert(!pet_image_decode(b,1038,out,64));
 return 0;
}''')
    exe=tmp_path/"decode"
    subprocess.run(["cc","-std=c11","-fsanitize=address,undefined","-I",str(ROOT/"firmware/main"),str(source),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

def test_full_resolution_jpeg_envelope(tmp_path):
    source=tmp_path/'jpeg.c'
    source.write_text('''#include "pet_p4_image.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){
 unsigned char b[100001];FILE *f=fopen(argv[1],"rb");assert(f);size_t n=fread(b,1,sizeof(b),f);fclose(f);
 assert(pet_image_jpeg(b,n));assert(pet_image_decode(b,n,NULL,0));
 assert(!pet_image_jpeg(b,n-1));b[4]^=1;assert(!pet_image_jpeg(b,n));b[4]^=1;
 b[n-1]=0;assert(!pet_image_jpeg(b,n));return 0;
}''')
    exe=tmp_path/'jpeg';subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),str(source),'-o',str(exe)],check=True)
    if os.environ.get('PET_APP_ENCODED_DIR'):
        paths=[p for p in Path(os.environ['PET_APP_ENCODED_DIR']).glob('*/*.p4i') if p.read_bytes().startswith(b'P4J1')]
        assert paths, "No production JPEGs exported"
        for path in paths:subprocess.run([str(exe),str(path)],check=True)
    else:
        # A bounded SOF0 header is enough for the envelope test; the real JPEG
        # hardware/software decoder still validates the entropy stream.
        import struct
        data=b'\xff\xd8\xff\xc0'+struct.pack('>HBHHB',17,8,480,640,3)+bytes([1,17,0,2,17,1,3,17,1])+b'\xff\xda\x00\x02\xff\xd9'
        packed=b'P4J1'+struct.pack('<HHI',640,480,12+len(data))+data
        path=tmp_path/'image.p4i';path.write_bytes(packed);subprocess.run([str(exe),str(path)],check=True)

def test_exported_palette_images_decode_without_overflow(tmp_path):
    import pytest
    if not os.environ.get('PET_APP_ENCODED_DIR'):
        pytest.skip("Requires exported production palette assets")
    source=tmp_path/'palette.c'
    source.write_text('''#include "pet_p4_image.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){
  unsigned char data[131073];FILE*f=fopen(argv[1],"rb");assert(f);
  size_t n=fread(data,1,sizeof(data),f);fclose(f);
  size_t w=data[4],h=data[5];if(!memcmp(data,"P4I2",4)){w|=(size_t)data[5]<<8;h=data[6]|((size_t)data[7]<<8);}
  size_t size=w*h;assert(size<=98304);unsigned char*out=malloc(size+1);assert(out);out[size]=42;
  assert(pet_image_decode(data,n,out,size));assert(out[size]==42);
  assert(!pet_image_decode(data,n-1,out,size));
  assert(!pet_image_decode(data,n,out,size-1));assert(out[size]==42);
  free(out);return 0;
}''')
    exe=tmp_path/'palette'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),str(source),'-o',str(exe)],check=True)
    paths=[p for p in Path(os.environ['PET_APP_ENCODED_DIR']).glob('*/*.p4i') if p.read_bytes()[:4] in (b'P4I1',b'P4I2')]
    assert any(p.read_bytes().startswith(b'P4I2') for p in paths), "Wide atlas must be covered"
    for path in paths:subprocess.run([str(exe),str(path)],check=True)
