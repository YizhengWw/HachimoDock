"""Optional population policy is not permission to skip current drawing."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_cache_population_boundary_zero_and_invalid_clock(tmp_path):
    source=tmp_path/'budget.c';binary=tmp_path/'budget'
    source.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include "pet_p4_app_cache.h"
int main(void){
 assert(pet_app_cache_population_admitted(0,0));
 assert(pet_app_cache_population_admitted(0,28000));
 assert(!pet_app_cache_population_admitted(0,28001));
 assert(!pet_app_cache_population_admitted(-1,0));
 assert(!pet_app_cache_population_admitted(100,99));
 assert(pet_app_cache_population_admitted(INT64_MAX,INT64_MAX));
 assert(!pet_app_cache_population_admitted(0,INT64_MAX));return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
