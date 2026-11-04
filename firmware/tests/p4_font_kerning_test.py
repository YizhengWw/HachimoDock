"""Startup expansion keeps every original kerning class byte exact."""
from pathlib import Path
import re
import subprocess
import pytest
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]
@pytest.mark.parametrize("corrupt",[False,True])
def test_sparse_maps_expand_to_original_bytes_and_bad_hash_refuses(tmp_path,corrupt):
    text=(ROOT/"main/pet_p4_font_cn.c").read_text()
    original=[]
    for name in ["kern_left_class_mapping","kern_right_class_mapping"]:
        m=re.search(r"static const uint8_t "+name+r"\[\] =\s*\{(.*?)\};",text,re.S)
        original.extend(int(x) for x in re.findall(r"\b\d+\b",m[1]))
    header=(ROOT/"main/pet_p4_font_kerning_sparse.h").read_text()
    if corrupt:header=re.sub(r"0x[0-9a-f]{8}","0x00000000",header,count=1)
    (tmp_path/"maps.h").write_text(header)
    prepare=function((ROOT/"main/pet_p4_font_bitmap.c").read_text(),"prepare_kerning")
    src=tmp_path/"maps.c"
    src.write_text('''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "maps.h"
static uint8_t pet_font_kern_left[PET_FONT_KERN_RECORDS],pet_font_kern_right[PET_FONT_KERN_RECORDS];
'''+prepare+'''
int main(void){
 bool ready=prepare_kerning();
#if CORRUPT
 assert(!ready);
#else
 assert(ready);
 assert(fwrite(pet_font_kern_left,1,sizeof(pet_font_kern_left),stdout)==sizeof(pet_font_kern_left));
 assert(fwrite(pet_font_kern_right,1,sizeof(pet_font_kern_right),stdout)==sizeof(pet_font_kern_right));
#endif
 return 0;
}
''')
    exe=tmp_path/"maps"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",f"-DCORRUPT={int(corrupt)}",str(src),"-o",str(exe)],check=True)
    result=subprocess.run([str(exe)],check=True,capture_output=True)
    if not corrupt:assert result.stdout==bytes(original)

def test_generated_native_maps_are_reproducible():
    import sys
    subprocess.run([sys.executable,str(ROOT/"tools/compress_font_kerning.py"),"--check"],check=True)
