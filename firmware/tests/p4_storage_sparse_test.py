"""Same allocation/authority with exact compact-middle native page copies."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_sparse_representation_all_contained_slices_boundaries_and_full_fallback(tmp_path):
    source=tmp_path/'sparse.c';binary=tmp_path/'sparse'
    source.write_text(r'''
#include "pet_p4_storage_sparse_page.h"
#include <assert.h>
static uint8_t page[512],slot[514],out[514];
int main(void){
 for(unsigned value=0;value<256;value+=17){
  for(unsigned i=0;i<512;i++)page[i]=(uint8_t)(i*197+value);
  memset(page+32,value,448);assert(pet_sparse_page_eligible(page));
  memset(slot,0x5a,sizeof(slot));pet_sparse_page_store(slot+1,page);
  assert(slot[0]==0x5a&&slot[66]==0x5a&&slot[513]==0x5a);
  for(unsigned offset=0;offset<512;offset++)for(unsigned n=1;n<=512-offset;n++){
   out[0]=0x55;out[n+1]=0xaa;pet_sparse_page_copy(out+1,slot+1,offset,n);
   assert(out[0]==0x55&&out[n+1]==0xaa&&!memcmp(out+1,page+offset,n));
  }
  for(unsigned i=32;i<480;i++){page[i]^=1;assert(!pet_sparse_page_eligible(page));page[i]^=1;}
 }
 return 0;
}
''')
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=30)

def test_actual_sparse_cache_preserves_original_pins_failed_mutations_and_replacement(tmp_path):
    code=(ROOT/'tests/p4_storage_page_cache_test.c').read_text()
    old='if(size==512&&((action==1&&in_page_data(src))||(action==2&&in_page_data(dst))))'
    assert code.count(old)==1
    # Compact copies really are32 bytes; trigger the SAME original nested
    # invalidation scenarios on the actual payload copy, not a never-fired hook.
    code=code.replace(old,'if(size<=512&&((action==1&&in_page_data(src))||(action==2&&in_page_data(dst))))')
    old='else if(action==3&&size==512&&in_page_data(src))'
    assert code.count(old)==1;code=code.replace(old,'else if(action==3&&size<=512&&in_page_data(src))')
    marker=' clock_us+=5000001;before=reads;read_at(a);assert(reads==before+1);'
    assert code.count(marker)==1
    code=code.replace(marker,r'''
 /* Compact/full transitions and colliding way cursor preserve all format bits. */
 for(unsigned n=0;n<100;n++){
  const uint32_t colliding[3]={a,b,c};uint32_t at=colliding[n%3];
  assert(!__wrap_spiffs_api_erase(&fs,at,512));
  for(unsigned i=0;i<32;i++)medium[at+i]=(uint8_t)(n+i*31);
  for(unsigned i=480;i<512;i++)medium[at+i]=(uint8_t)(n+i*17);
  read_at(at);before=reads;read_at(at);assert(reads==before&&!memcmp(data,medium+at,512));
  uint8_t changed=(uint8_t)n;assert(!__wrap_spiffs_api_write(&fs,at+123,1,&changed));
  read_at(at);before=reads;read_at(at);assert(reads==before&&!memcmp(data,medium+at,512));
 }
'''+marker)
    source=tmp_path/'cache.c';source.write_text(code);binary=tmp_path/'cache'
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
      '-fsanitize=address,undefined','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_STORAGE_IO_HOST_TEST=1',
      '-DPET_P4_STORAGE_PAGE_CACHE_ENABLE=1','-DPET_P4_STORAGE_PAGE_CACHE_APPLY=1','-DPET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH=1',
      '-DPET_P4_STORAGE_PAGE_CACHE_SPARSE=1','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/storage_io_stubs'),
      str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=30)

def test_actual_sparse_partial_cache_keeps_all_original_slice_and_nested_fault_vectors(tmp_path,monkeypatch):
    import p4_storage_partial_test as partial
    original_run=subprocess.run;original_read=Path.read_text
    def read(path,*args,**kwargs):
        text=original_read(path,*args,**kwargs)
        if path==ROOT/'tests/p4_storage_page_cache_test.c':
            old='else if(action==3&&size==512&&in_page_data(src))'
            assert text.count(old)==1;text=text.replace(old,'else if(action==3&&size<=512&&in_page_data(src))')
        return text
    def run(args,*pos,**kwargs):
        if args[0]=='cc' and '-DPET_P4_STORAGE_PAGE_CACHE_APPLY=1' in args:
            args=[*args,'-DPET_P4_STORAGE_PAGE_CACHE_SPARSE=1']
        return original_run(args,*pos,**kwargs)
    monkeypatch.setattr(Path,'read_text',read);monkeypatch.setattr(subprocess,'run',run)
    partial.test_actual_partial_cache_all_contained_ranges_guards_and_original_interleavings(tmp_path)

def test_sparse_policy_default_off_and_requires_exact_applied_parent(tmp_path):
    source=tmp_path/'guard.c';obj=tmp_path/'guard.o'
    source.write_text('#include "pet_p4_storage_io_trace.h"\n_Static_assert(!PET_P4_STORAGE_PAGE_CACHE_SPARSE,"default off");\n')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-c',str(source),'-o',str(obj)],check=True)
    source.write_text('#include "pet_p4_storage_io_trace.h"\n')
    r=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_STORAGE_PAGE_CACHE_SPARSE=1','-I',str(ROOT/'main'),str(source)],capture_output=True,text=True)
    assert r.returncode!=0 and 'Sparse exact pages require' in r.stderr
