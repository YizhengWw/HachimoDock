"""Exact slices of prior complete physical pages; no read-ahead or filenames."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_actual_partial_cache_all_contained_ranges_guards_and_original_interleavings(tmp_path):
    original=(ROOT/'tests/p4_storage_page_cache_test.c').read_text()
    old='before=reads;assert(!__wrap_spiffs_api_read(&fs,a+1,511,data)&&reads==before+1);'
    assert original.count(old)==1
    original=original.replace(old,'before=reads;assert(!__wrap_spiffs_api_read(&fs,a+1,511,data)&&reads==before+(PET_P4_STORAGE_PAGE_CACHE_APPLY?0:1));')
    old_hook='if(size==512&&((action==1&&in_page_data(src))||(action==2&&in_page_data(dst))))'
    assert original.count(old_hook)==1
    original=original.replace(old_hook,'if(size<=512&&((action==1&&in_page_data(src))||(action==2&&in_page_data(dst))))')
    marker=' clock_us+=5000001;before=reads;read_at(a);assert(reads==before+1);'
    assert original.count(marker)==1
    vectors=r'''
 /* Every byte range contained in the already read full page. */
 read_at(a);before=reads;
 for(unsigned offset=0;offset<512;offset++)for(unsigned n=1;n<=512-offset;n++){
   data[0]=0x55;data[n+1]=0xaa;
   assert(!__wrap_spiffs_api_read(&fs,a+offset,n,data+1));
   assert(data[0]==0x55&&data[n+1]==0xaa&&!memcmp(data+1,medium+a+offset,n));
 }
 assert(reads==before+(PET_P4_STORAGE_PAGE_CACHE_APPLY?0:512*513/2));
 before=reads;assert(!__wrap_spiffs_api_read(&fs,a+1,512,data)&&reads==before+1);
 before=reads;assert(!__wrap_spiffs_api_read(&fs,a,0,data)&&reads==before+1);
 before=reads;assert(__wrap_spiffs_api_read(&fs,UINT32_MAX,1,data)==-91&&reads==before+1);
 read_at(a);
 pet_storage_physical_scope_t only_physical={0};assert(pet_storage_physical_begin(&only_physical));
 before=reads;assert(!__wrap_spiffs_api_read(&fs,a,32,data)&&reads==before+1);
 assert(pet_storage_physical_finish(&only_physical));
 if(PET_P4_STORAGE_PAGE_CACHE_APPLY){
   assert(!__wrap_spiffs_api_erase(&fs,a,512));read_at(a);
   uint8_t prior[32];memcpy(prior,medium+a,sizeof(prior));hook=1;
   assert(!__wrap_spiffs_api_read(&fs,a,32,data)&&!hook&&!memcmp(data,prior,32));
   read_at(a);assert(data[0]==0);
 }
'''
    source=tmp_path/'partial.c';source.write_text(original.replace(marker,vectors+marker))
    for apply in (0,1):
        binary=tmp_path/('partial-'+str(apply))
        subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
            '-fsanitize=address,undefined','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_STORAGE_IO_HOST_TEST=1',
            '-DPET_P4_STORAGE_PAGE_CACHE_ENABLE=1',f'-DPET_P4_STORAGE_PAGE_CACHE_APPLY={apply}',
            '-DPET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH=1','-DPET_P4_STORAGE_PAGE_CACHE_PARTIAL=1','-DPET_P4_STORAGE_PRESERVE_UNCHANGED=1',
            '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/storage_io_stubs'),str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=20)
