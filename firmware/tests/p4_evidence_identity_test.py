"""Boot evidence is read-only, bounded and cannot imply execution permission."""
import subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def test_native_boot_identity_and_bounded_reply(tmp_path):
    src=tmp_path/'identity.c';exe=tmp_path/'identity';vendor=ROOT/'firmware/tests/vendor/cjson'
    src.write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include "pet_p4_evidence_identity.h"
static unsigned allocations,fail_at;
static void *allocation(size_t n){return allocations++==fail_at?NULL:malloc(n);}
int main(void){
 pet_boot_identity_t boot={0};assert(pet_boot_identity_start(&boot,0x01020304,0xaabbccdd));assert(!strcmp(boot.id,"01020304aabbccdd"));
 pet_boot_identity_t old=boot;assert(!pet_boot_identity_start(&boot,1,2));assert(!memcmp(&boot,&old,sizeof(boot)));assert(!pet_boot_identity_start(NULL,1,2));
 assert(!pet_evidence_source_valid("{\"requestId\":\"a\\u0000b\"}"));assert(pet_evidence_source_valid("{\"requestId\":\"a\\\\u0000b\"}"));
 const char *bad[]={"{}","[]","null","{\"requestId\":5}","{\"requestId\":\"a/b\"}","{\"requestId\":\"a\",\"requestId\":\"b\"}","{\"requestId\":\"a\",\"extra\":1}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){cJSON*r=cJSON_Parse(bad[i]);assert(!pet_evidence_request(r));cJSON_Delete(r);}
 cJSON*r=cJSON_Parse("{\"requestId\":\"request-1\"}");assert(!strcmp(pet_evidence_request(r),"request-1"));cJSON_Delete(r);
 const char *elf="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
 r=pet_evidence_identity(&boot,"request-1","synthetic-board","0.7.103-p4-r27+abc-dirty","abc",elf,true,7,123456);
 assert(r&&cJSON_GetArraySize(r)==10);assert(!strcmp(cJSON_GetObjectItemCaseSensitive(r,"bootId")->valuestring,boot.id));cJSON_Delete(r);
 assert(!pet_evidence_identity(&boot,"request-1","bad/board","version","abc",elf,false,7,0));
 assert(!pet_evidence_identity(&boot,"request-1","board","version","abc","bad",false,7,0));
 assert(!pet_evidence_identity(&boot,"request-1","board","version","abc",elf,false,0,0));
 assert(!pet_evidence_identity(&boot,"request-1","board","version","abc",elf,false,7,UINT64_MAX));
 for(fail_at=0;fail_at<50;fail_at++){
  allocations=0;cJSON_Hooks hooks={.malloc_fn=allocation,.free_fn=free};cJSON_InitHooks(&hooks);
  r=pet_evidence_identity(&boot,"request-1","board","version","abc",elf,false,7,0);
  if(r){assert(cJSON_GetArraySize(r)==10);cJSON_Delete(r);}assert(!memcmp(&boot,&old,sizeof(boot)));
 }
 cJSON_InitHooks(NULL);return 0;
}''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined','-I'+str(ROOT/'firmware/main'),'-I'+str(vendor),str(src),str(vendor/'cJSON.c'),'-o',str(exe)],check=True,timeout=30)
    subprocess.run([str(exe)],check=True,timeout=10)

def test_actual_startup_and_read_only_protocol_identity():
    main=(ROOT/'firmware/main/pet_p4_main.c').read_text();protocol=(ROOT/'firmware/main/pet_p4_protocol.c').read_text()
    assert main.index('pet_p4_boot_identity_start(esp_random(),esp_random())')<main.index('xTaskCreate',main.index('pet_p4_state_init(&g_state'))
    start=protocol.index('} else if (strcmp(topic,"debug/identity")==0)')
    branch=protocol[start:protocol.index('} else if',start+len('} else if'))]
    for forbidden in ('fopen(', 'nvs_', 'pet_p4_audio_', 'pet_p4_miniapp_', 'bind', 'desktop_device_id', 'command('):assert forbidden not in branch
    assert 'pet_evidence_source_valid(line)' in branch and 'app->app_elf_sha256' in branch
    assert 'debugEvidenceIdentity' in protocol and 'debugBootId' in protocol
