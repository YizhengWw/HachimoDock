"""Real install validator: malformed JSON sizes reject without float/int UB.
Compiles only the extracted production validator and its real JSON/hash helpers;
set_error is a logging-only stub. No SDK, device, or filesystem mutation path.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def _function(source, name):
    m = re.search(r"(?m)^static [A-Za-z_][A-Za-z0-9_ \t*]*\b" + name + r"\s*\([^;{}]*\)\s*\{", source)
    assert m, name
    start, i = m.start(), source.index("{", m.start()) + 1
    depth, quote, escaped = 1, None, False
    while depth:
        c = source[i]
        if quote:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return source[start:i]


HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
static void set_error(char *out,size_t n,const char *message){if(out&&n)snprintf(out,n,"%s",message);}
/* ACTUAL_VALIDATOR */
int main(void){
  const uint8_t bytes[]={1,2,3,4};char error[96]={0};
  /* The JSON parser admits an overflowing exponent as a Number. */
  cJSON *p=cJSON_Parse("{\"decodedSize\":1e999}");assert(p);
  assert(cJSON_IsNumber(cJSON_GetObjectItem(p,"decodedSize")));
  assert(!verify_chunk_integrity(p,bytes,sizeof(bytes),error,sizeof(error)));cJSON_Delete(p);
  const char *bad[]={"{\"decodedSize\":1e300}","{\"decodedSize\":-1}",
    "{\"decodedSize\":4.5}","{\"decodedSize\":null}","{\"decodedSize\":true}",
    "{\"decodedSize\":\"4\"}","{\"decodedSize\":3}","{\"decodedSize\":5}"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
    p=cJSON_Parse(bad[i]);assert(p);assert(!verify_chunk_integrity(p,bytes,sizeof(bytes),error,sizeof(error)));cJSON_Delete(p);
  }
  p=cJSON_Parse("{\"decodedSize\":4}");assert(p);
  assert(verify_chunk_integrity(p,bytes,sizeof(bytes),error,sizeof(error)));
  char sum[17];snprintf(sum,sizeof(sum),"%016llx",(unsigned long long)fnv1a64(bytes,sizeof(bytes)));
  cJSON_AddStringToObject(p,"checksum",sum);assert(verify_chunk_integrity(p,bytes,sizeof(bytes),error,sizeof(error)));
  cJSON_ReplaceItemInObject(p,"checksum",cJSON_CreateString("0000000000000000"));
  assert(!verify_chunk_integrity(p,bytes,sizeof(bytes),error,sizeof(error)));cJSON_Delete(p);
  p=cJSON_Parse("{}");assert(verify_chunk_integrity(p,bytes,sizeof(bytes),error,sizeof(error)));cJSON_Delete(p);
  puts("PASS nonfinite/huge/fraction/negative/type/mismatch rejected; exact size/FNV and legacy optional-size preserved");
  return 0;
}
'''


def test_actual_install_chunk_size_contract(tmp_path):
    source = (ROOT / "main/pet_p4_miniapp.c").read_text()
    actual = "\n".join(_function(source, n) for n in
                       ("json_string", "fnv1a64", "verify_chunk_integrity"))
    src, binary = tmp_path / "chunk.c", tmp_path / "chunk"
    src.write_text(HARNESS.replace("/* ACTUAL_VALIDATOR */", actual))
    flags = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined,float-cast-overflow",
             "-fno-sanitize-recover=all", "-I", str(ROOT / "tests/vendor/cjson")]
    vendor = tmp_path / "cJSON.o"
    # New Apple SDK annotates upstream sprintf as deprecated. Isolate that
    # vendor-only host warning; production validator retains full -Werror.
    subprocess.run(flags + ["-Wno-error=deprecated-declarations", "-c",
                           str(ROOT / "tests/vendor/cjson/cJSON.c"), "-o", str(vendor)],
                   check=True, timeout=30)
    subprocess.run(flags + [str(src), str(vendor), "-lm", "-o", str(binary)],
                   check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=10)
