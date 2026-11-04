"""Real bounded extent/save reader; local stdio faults, no device/user files.

EOF-request observation is a host adapter, not proof of SPIFFS global errno or
P4 throughput. The unchanged directory retry/error gates remain separately tested.
"""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]

def test_extent_faults_and_actual_save_reader_do_not_probe_beyond_eof(tmp_path):
    source=(ROOT/'main/pet_p4_app_store.c').read_text()
    declarations='\n'.join(line for line in source.splitlines() if line.startswith((
        'typedef struct {char id[48],json[','typedef struct {uint32_t magic,seq,size,checksum;',
        'typedef enum { SAVE_MISSING')))
    code=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_app_store.h"
static unsigned seeks,tells,reads,closes,fail_seek,fail_tell,revoke_at,checks,change_extent;
static bool revoked,eof_requested,read_failed;
static size_t blob_size;
static int checked_seek(FILE *f,long offset,int whence){
  seeks++;if(seeks==fail_seek){errno=EIO;return -1;}return fseek(f,offset,whence);
}
static long checked_tell(FILE *f){
  tells++;if(tells==fail_tell){errno=EIO;return -1;}
  long length=ftell(f);return tells==2&&change_extent?length+1:length;
}
#define fseek checked_seek
#define ftell checked_tell
#include "pet_p4_file_read.h"
#undef fseek
#undef ftell
static size_t checked_read(void *out,size_t size,size_t count,FILE *f){
  reads++;long at=ftell(f);assert(at>=0);
  if((uintmax_t)at+size*count>blob_size)eof_requested=true;
  return fread(out,size,count,f);
}
static int checked_close(FILE *f){closes++;return fclose(f);}
static int checked_error(FILE *f){return read_failed?1:ferror(f);}
static bool pet_resources_device_current(pet_resource_lease_t lease){
  (void)lease;checks++;if(checks==revoke_at)revoked=true;return !revoked;
}
static void epoch_path_for(const char *id,uint32_t domain,unsigned slot,uint32_t epoch,char out[64]){
  assert(epoch==0);
  assert(!strcmp(id,"fixture")&&domain==7&&slot==0);strcpy(out,"private-save.bin");
}
#define PET_FS_ACTIVITY_SCOPE(name) int name __attribute__((unused))=0
#define PET_WIDGET_TRACE_SCOPE(name,kind) int name __attribute__((unused))=0
'''+declarations+'\n'+function(source,'checksum')+r'''
#define fread checked_read
#define fclose checked_close
#define ferror checked_error
'''+function(source,'read_slot')+r'''
#undef fread
#undef fclose
#undef ferror
static const char json[]="{\"counter\":7}";
static unsigned char original[sizeof(header_t)+sizeof(json)-1];
static void prepare(unsigned mode){
  header_t h={.magic=0x41535631,.seq=42,.size=sizeof(json)-1};strcpy(h.id,"fixture");
  h.checksum=checksum(json,sizeof(json)-1);memcpy(original,&h,sizeof(h));
  memcpy(original+sizeof(h),json,sizeof(json)-1);
  FILE *f=fopen("private-save.bin","wb");assert(f);
  blob_size=sizeof(original);
  if(mode==1)blob_size--;if(mode==2)h.magic=0;
  if(mode==3)h.checksum++;if(mode==2||mode==3)memcpy(original,&h,sizeof(h));
  assert(fwrite(original,1,blob_size,f)==blob_size);
  if(mode==4){assert(fputc('x',f)!=EOF);blob_size++;}
  assert(!fclose(f));
}
static void reset(void){
  seeks=tells=reads=closes=fail_seek=fail_tell=revoke_at=checks=change_extent=0;
  revoked=eof_requested=read_failed=false;
}
static void unchanged(void){
  FILE *f=fopen("private-save.bin","rb");assert(f);unsigned char bytes[sizeof(original)+1];
  assert(fread(bytes,1,sizeof(bytes),f)==sizeof(original));assert(!fclose(f));
  assert(!memcmp(bytes,original,sizeof(original)));
}
int main(void){
  save_t out={0};pet_resource_lease_t lease={0};
  prepare(0);reset();
  assert(read_slot("fixture",7,0,&out,lease)==SAVE_VALID);
  assert(out.seq==42&&out.loaded&&!strcmp(out.json,json)&&reads==2&&closes==1&&!eof_requested);
  unchanged();
  for(unsigned i=1;i<=4;i++){
    reset();fail_seek=i;out=(save_t){0};
    assert(read_slot("fixture",7,0,&out,lease)==SAVE_UNAVAILABLE);
    assert(!out.loaded&&closes==1&&!eof_requested);unchanged();
  }
  for(unsigned i=1;i<=2;i++){
    reset();fail_tell=i;out=(save_t){0};
    assert(read_slot("fixture",7,0,&out,lease)==SAVE_UNAVAILABLE);
    assert(!out.loaded&&closes==1&&!eof_requested);unchanged();
  }
  reset();read_failed=true;
  assert(read_slot("fixture",7,0,&out,lease)==SAVE_UNAVAILABLE);unchanged();
  reset();change_extent=1;out=(save_t){0};
  assert(read_slot("fixture",7,0,&out,lease)==SAVE_UNAVAILABLE&&!out.loaded&&!eof_requested);unchanged();
  for(unsigned i=1;i<=4;i++){
    reset();revoke_at=i;
    assert(read_slot("fixture",7,0,&out,lease)==SAVE_UNAVAILABLE&&!eof_requested);unchanged();
  }
  for(unsigned mode=1;mode<=4;mode++){
    prepare(mode);reset();out=(save_t){0};
    assert(read_slot("fixture",7,0,&out,lease)==SAVE_CORRUPT&&!out.loaded&&!eof_requested);
  }
  prepare(0);reset();FILE *f=fopen("private-save.bin","rb");assert(f);
  size_t length=777;assert(pet_file_read_extent(f,2,&length)==PET_FILE_SIZE_BOUNDS&&length==777);
  assert(pet_file_read_extent(f,blob_size,&length)==PET_FILE_SIZE_OK&&length==blob_size&&ftell(f)==0);
  assert(pet_file_read_extent(NULL,blob_size,&length)==PET_FILE_SIZE_BOUNDS);
  assert(!fclose(f));unchanged();
  return 0;
}
'''
    path=tmp_path/'bounded-file.c';path.write_text(code);exe=tmp_path/'bounded-file'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-fsanitize=address,undefined','-I',str(ROOT/'main'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=tmp_path,check=True,timeout=10)
