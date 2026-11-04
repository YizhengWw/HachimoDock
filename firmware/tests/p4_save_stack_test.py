"""Private SAVE budget evidence, never a stack-size or scheduling workaround."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_save_stack_json_allocation_faults_no_partial_values_or_leaks(tmp_path):
    source = tmp_path / 'save-json.c'
    binary = tmp_path / 'save-json'
    vendor = ROOT / 'tests/vendor/cjson'
    source.write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include "pet_p4_save_stack_json.h"
static int remaining=-1,live;
static unsigned reads;
static pet_app_save_stack_t observed;
bool pet_app_store_worker_stack(pet_app_save_stack_t *out){assert(out);*out=observed;reads++;return true;}
static void *allocate(size_t n){if(!remaining)return NULL;if(remaining>0)remaining--;void *p=malloc(n);if(p)live++;return p;}
static void release(void *p){if(p){live--;free(p);}}
int main(void){
 cJSON_Hooks hooks={.malloc_fn=allocate,.free_fn=release};cJSON_InitHooks(&hooks);
 unsigned failures=0,successes=0;
 for(unsigned mode=0;mode<3;mode++)for(int n=0;n<40;n++){
  observed=(pet_app_save_stack_t){.available=mode==1,.observations=mode?UINT32_MAX:0,
   .minimum_free_bytes=1536,.invalid_samples=mode==2?1:0};
  remaining=n;unsigned before=reads;cJSON *v=pet_app_save_stack_json();assert(reads==before+1);
  if(v){
   successes++;assert(cJSON_GetArraySize(v)==(mode==1?5:4));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"available"))==(mode==1));
   assert(cJSON_GetObjectItemCaseSensitive(v,"taskStackBytes")->valuedouble==4096);
   cJSON *free_bytes=cJSON_GetObjectItemCaseSensitive(v,"minimumFreeBytes");
   assert((mode==1)==(free_bytes!=NULL));if(free_bytes)assert(free_bytes->valuedouble==1536);
   remaining=-1;cJSON_Delete(v);
  }else failures++;
  assert(!live);
 }
 assert(failures&&successes);cJSON_InitHooks(NULL);return 0;
}
''')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-error=deprecated-declarations', '-fsanitize=address,undefined',
                    '-DPET_APP_SAVE_OWNED_PENDING_ENABLED=1', '-I', str(ROOT/'main'),
                    '-I', str(vendor), str(source), str(vendor/'cJSON.c'), '-lm',
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_save_stack_default_off_and_sample_only_after_physical_flush(tmp_path):
    source = tmp_path / 'off.c'
    binary = tmp_path / 'off.o'
    source.write_text('#include "pet_p4_save_policy.h"\n'
                      '_Static_assert(!PET_APP_SAVE_OWNED_PENDING_ENABLED,"default off");\n'
                      '_Static_assert(PET_APP_SAVE_TASK_STACK_BYTES==4096,"same stack");\n')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I',
                    str(ROOT/'main'), '-c', str(source), '-o', str(binary)], check=True)
    assert not subprocess.run(['nm', '-u', str(binary)], check=True,
                              capture_output=True, text=True).stdout.strip()
    store = (ROOT/'main/pet_p4_app_store.c').read_text()
    start = store.index('static void save_stack_sample(')
    sample = store[start:store.index('\n#endif\ntypedef struct', start)]
    assert sample.count('uxTaskGetStackHighWaterMark(NULL)') == 1
    assert sample.index('uxTaskGetStackHighWaterMark(NULL)') < sample.index('portENTER_CRITICAL(')
    getter = sample[sample.index('bool pet_app_store_worker_stack('):]
    for forbidden in ('uxTask', 'malloc(', 'printf(', 'fopen(', 'save_stack_sample('):
        assert forbidden not in getter
    worker = store[store.index('static void save_worker('):store.index('bool pet_app_store_init(')]
    assert worker.index('flush_one();') < worker.index('save_stack_sample();') < worker.index('pet_app_store_housekeeping();')
    assert 'if(save_stack_sample_due)' in worker
    assert 'PET_APP_SAVE_TASK_STACK_BYTES,NULL,1,&worker' in store
