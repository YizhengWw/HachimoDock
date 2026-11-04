"""Actual bounded boot recorder/JSON; no device or acoustic acceptance."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_actual_boot_milestones_once_missing_duplicate_backward_and_range(tmp_path):
    source=tmp_path/'boot.c';binary=tmp_path/'boot'
    source.write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_boot_trace.c"
unsigned io_meta_depth;
static int64_t stamp=100;
static unsigned clock_calls;
int64_t esp_timer_get_time(void){assert(!io_meta_depth);clock_calls++;return stamp;}
int main(int argc,char **argv){
 assert(argc==2);unsigned mode=(unsigned)atoi(argv[1]);pet_boot_trace_t v;
 assert(pet_boot_trace_observe(&v)&&!v.available&&!v.mask);
 assert(!pet_boot_trace_observe(NULL));
 assert(!pet_boot_trace_observe((pet_boot_trace_t*)&boot_trace));
 assert(!pet_boot_trace_observe((pet_boot_trace_t*)&boot_mux));
 assert(!pet_boot_trace_mark(PET_BOOT_STORAGE_DONE));
 if(mode==1)stamp=-1;
 bool begun=pet_boot_trace_begin();assert(begun==(mode!=1));
 stamp=110;assert(!pet_boot_trace_begin());
 if(mode==1){assert(pet_boot_trace_observe(&v)&&!v.available&&v.invalid_clock&&!v.mask);return 0;}
 if(mode==3){stamp=100LL+UINT32_MAX+1;assert(!pet_boot_trace_mark(PET_BOOT_STORAGE_DONE));}
 else {
  assert(pet_boot_trace_mark(PET_BOOT_STORAGE_DONE));
  assert(pet_boot_trace_observe(&v)&&v.elapsed_us[0]==10&&v.mask==1);
  pet_boot_trace_t before=v;stamp=120;assert(!pet_boot_trace_mark(PET_BOOT_STORAGE_DONE));
  assert(pet_boot_trace_observe(&v)&&!memcmp(&v,&before,sizeof(v)));
  if(mode==2){stamp=90;assert(!pet_boot_trace_mark(PET_BOOT_CATALOG_DONE));}
 }
 if(mode==2||mode==3){
  assert(pet_boot_trace_observe(&v)&&v.invalid_clock);
  assert(v.mask==(mode==2?1:0));stamp=200;assert(!pet_boot_trace_mark(PET_BOOT_CATALOG_DONE));return 0;
 }
 unsigned calls=clock_calls;assert(!pet_boot_trace_mark((pet_boot_mark_t)-1));
 assert(!pet_boot_trace_mark(PET_BOOT_MARK_COUNT)&&clock_calls==calls);
 /* Image validity can precede transport on a non-pending boot. IDs are not
  * a claim about chronological order; timestamps are actual elapsed values. */
 stamp=130;assert(pet_boot_trace_mark(PET_BOOT_IMAGE_VALID));
 for(unsigned i=1;i<7;i++){stamp+=10;assert(pet_boot_trace_mark((pet_boot_mark_t)i));}
 assert(pet_boot_trace_observe(&v)&&v.mask==255&&v.available&&!v.invalid_clock);
 assert(v.elapsed_us[7]<v.elapsed_us[6]&&!io_meta_depth);return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-DPET_P4_BOOT_TRACE_ENABLE=1','-I',str(ROOT/'tests/storage_io_stubs'),'-I',str(ROOT/'main'),
        str(source),'-o',str(binary)],check=True)
    for mode in range(4):subprocess.run([str(binary),str(mode)],check=True,timeout=10)

def test_boot_json_allocation_failures_never_publish_partial_or_leak(tmp_path):
    source=tmp_path/'json.c';binary=tmp_path/'json';vendor=ROOT/'tests/vendor/cjson'
    source.write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include "pet_p4_boot_trace_json.h"
static int remaining=-1,live;
static void *allocate(size_t n){if(!remaining)return NULL;if(remaining>0)remaining--;void*p=malloc(n);if(p)live++;return p;}
static void release(void *p){if(p){live--;free(p);}}
int main(void){
 cJSON_Hooks hooks={.malloc_fn=allocate,.free_fn=release};cJSON_InitHooks(&hooks);
 pet_boot_trace_t b={.mask=255,.available=true};b.elapsed_us[7]=UINT32_MAX;
 unsigned failures=0,successes=0;
 for(int n=0;n<40;n++){
  remaining=-1;cJSON *root=cJSON_CreateObject();assert(root&&cJSON_AddBoolToObject(root,"keep",true));
  int baseline=live;remaining=n;bool ok=pet_boot_trace_add_json(root,&b,true);
  cJSON *v=cJSON_GetObjectItemCaseSensitive(root,"bootTrace");
  if(ok){
   successes++;assert(cJSON_IsObject(v)&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"complete")));
   cJSON *times=cJSON_GetObjectItemCaseSensitive(v,"elapsedUs");assert(cJSON_GetArraySize(times)==8);
   assert(cJSON_GetArrayItem(times,7)->valuedouble==4294967295.0);
   assert(!pet_boot_trace_add_json(root,&b,true)&&cJSON_GetObjectItemCaseSensitive(root,"bootTrace")==v);
  }else{failures++;assert(!v&&live==baseline);}
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"keep")));
  remaining=-1;cJSON_Delete(root);assert(!live);
 }
 assert(failures&&successes);cJSON_InitHooks(NULL);return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations',
        '-fsanitize=address,undefined','-DPET_P4_BOOT_TRACE_ENABLE=1','-I',str(ROOT/'main'),
        '-I',str(vendor),str(source),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_boot_trace_default_off_and_explicit_query_only(tmp_path):
    obj=tmp_path/'off.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),
        '-c',str(ROOT/'main/pet_p4_boot_trace.c'),'-o',str(obj)],check=True)
    assert not subprocess.run(['nm','-u',str(obj)],check=True,capture_output=True,text=True).stdout.strip()
    code=(ROOT/'main/pet_p4_boot_trace.c').read_text()
    for effect in ('malloc(','fopen(','printf(','nvs_','audio_','wdt_','vTaskPrioritySet','REG_WRITE'):
        assert effect not in code
    diag=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    assert diag.index('"includeBootTrace"')<diag.index('pet_boot_trace_observe(')<diag.index('pet_boot_trace_add_json(')
    import configparser
    c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
    for name in c.sections():
        flags=c[name].get('build_flags','')
        if name=='env:esp32_p4_boot_trace_control':assert 'PET_P4_BOOT_TRACE_ENABLE=1' in flags
        else:assert 'PET_P4_BOOT_TRACE_ENABLE=1' not in flags
