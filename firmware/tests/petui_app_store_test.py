"""Production save algorithm with hardware/scheduler adapters, ASAN + UBSAN."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_private_save_generations_audio_deferral_and_corruption(tmp_path):
    stubs={
        'esp_attr.h':'#define EXT_RAM_BSS_ATTR\n',
        'esp_timer.h':'#include <stdint.h>\nstatic int64_t clock_us;\nstatic int64_t esp_timer_get_time(void){return clock_us;}\n',
        'freertos/FreeRTOS.h':'#define portMAX_DELAY 0\n#define pdPASS 1\n#define pdMS_TO_TICKS(x) (x)\n',
        'freertos/semphr.h':'typedef int SemaphoreHandle_t;\n#define xSemaphoreCreateMutex() 1\n#define xSemaphoreTake(a,b) ((void)0)\n#define xSemaphoreGive(a) ((void)0)\n',
        'freertos/task.h':'typedef int TaskHandle_t;\n#define xTaskCreate(a,b,c,d,e,f) ((void)(a),*(f)=1,1)\n#define vTaskDelay(n) ((void)(n))\n',
        'mbedtls/sha256.h':'#include <string.h>\nstatic int mbedtls_sha256(const unsigned char *p,size_t n,unsigned char *o,int b){(void)b;memset(o,0,32);for(size_t i=0;i<n;i++)o[i%32]^=p[i];return 0;}\n'
    }
    for name,body in stubs.items():
        p=tmp_path/name;p.parent.mkdir(exist_ok=True,parents=True);p.write_text(body)
    source=(ROOT/'main/pet_p4_app_store.c').read_text().replace('#include "pet_p4_audio.h"','''static int audio;
static int pet_p4_audio_stream_playing(void){return audio;}
static int pet_p4_audio_music_active(void){return 0;}
static int pet_p4_audio_conversation_active(void){return 0;}
static int pet_p4_audio_active(void){return 0;}
''')
    harness=tmp_path/'save.c'
    harness.write_text(source+r'''
#include <assert.h>
int main(void){
 assert(pet_app_store_init());char out[PET_APP_SAVE_BYTES],path[64];
 assert(!pet_app_store_read("garden",out));
 assert(pet_app_store_update("garden","{\"count\":1}"));
 assert(pet_app_store_read("garden",out)&&!strcmp(out,"{\"count\":1}"));
 clock_us=31000000;audio=1;flush_one();path_for("garden",1,path);assert(access(path,F_OK));
 audio=0;flush_one();assert(!access(path,F_OK));
 assert(pet_app_store_update("garden","{\"count\":99}"));
 assert(pet_app_store_update("garden","{\"count\":2}"));clock_us=62000000;flush_one();
 memset(saves,0,sizeof(saves));assert(pet_app_store_read("garden",out)&&!strcmp(out,"{\"count\":2}"));
 // Torn newer generation falls back to the untouched prior generation.
 path_for("garden",0,path);FILE *f=fopen(path,"wb");assert(f);fputs("torn",f);fclose(f);
 memset(saves,0,sizeof(saves));assert(pet_app_store_read("garden",out)&&!strcmp(out,"{\"count\":1}"));
 assert(!pet_app_store_read("another",out));assert(!pet_app_store_update("../garden","x"));
 pet_app_store_forget("garden");clock_us=93000000;flush_one();memset(saves,0,sizeof(saves));assert(!pet_app_store_read("garden",out));
 // Match the full catalog capacity; visiting the seventeenth saved app must not lose progress.
 memset(saves,0,sizeof(saves));
 for(int i=0;i<32;i++){char id[48];snprintf(id,sizeof(id),"app-%02d",i);assert(pet_app_store_update(id,"{\"count\":7}"));}
 for(int i=0;i<32;i++){char id[48];snprintf(id,sizeof(id),"app-%02d",i);assert(pet_app_store_read(id,out)&&!strcmp(out,"{\"count\":7}"));}
 assert(!pet_app_store_update("overflow","{}"));
 return 0;
}
''')
    exe=tmp_path/'save'
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DPET_APP_SAVE_ROOT="."','-I',str(tmp_path),'-I',str(ROOT/'main'),str(harness),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=tmp_path,check=True)
