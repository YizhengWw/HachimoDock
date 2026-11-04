"""Actual stage/cold-prepare validation; temp filesystem/heap/mutex adapters only."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]


def test_big_tiles_require_p4i1_exact_grid_dimensions_and_complete_encoding(tmp_path):
    (tmp_path/'freertos').mkdir()
    (tmp_path/'esp_heap_caps.h').write_text('#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n#define heap_caps_malloc(n,c) malloc(n)\n')
    (tmp_path/'esp_log.h').write_text('#define ESP_LOGW(tag,...) ((void)(tag))\n')
    (tmp_path/'freertos/FreeRTOS.h').write_text('#include <assert.h>\n#define configASSERT(x) assert(x)\n#define pdTRUE 1\n#define portMAX_DELAY 0xffffffffu\n')
    (tmp_path/'freertos/semphr.h').write_text('typedef void *SemaphoreHandle_t;\nstatic inline void *xSemaphoreCreateMutex(void){return (void*)1;}\n#define xSemaphoreTake(a,b) ((void)(a),(void)(b),1)\n#define xSemaphoreGive(a) ((void)(a))\n#define vSemaphoreDelete(a) ((void)(a))\n')
    code=(ROOT/'main/pet_p4_component_media.c').read_text().replace('"/spiffs/','"./')
    body=r'''
#include <assert.h>
static void w16(uint8_t*p,unsigned n){p[0]=n;p[1]=n>>8;}
static void w32(uint8_t*p,unsigned n){for(int i=0;i<4;i++)p[i]=n>>(8*i);}
static unsigned image(uint8_t*p,unsigned w){memset(p,0,1038);memcpy(p,"P4I1",4);p[4]=w;p[5]=1;p[6]=1;w32(p+8,1038);p[1036]=128|(w-1);return 1038;}
int main(void){
 uint8_t asset[2112]={0};memcpy(asset,"P4T1",4);asset[4]=2;asset[5]=1;w16(asset+6,121);w16(asset+8,1);w16(asset+10,241);w16(asset+12,1);asset[14]=2;
 unsigned n=36;for(unsigned i=0;i<2;i++){w32(asset+20+i*8,n);unsigned bytes=image(asset+n,i?120:121);w32(asset+24+i*8,bytes);n+=bytes;}
 pet_cext_runtime_t r={.big_count=1};strcpy(r.bigs[0].id,"map");r.bigs[0].w=241;r.bigs[0].h=1;
 assert(pet_cmedia_stage_chunk(PET_CEXT_MEDIA_BIG,"map",0,asset,n,NULL,0));assert(pet_cmedia_stage_matches(&r,NULL,0));pet_cmedia_stage_reset();
 char path[32];media_path(0,0,PET_CEXT_MEDIA_BIG,0,false,path,sizeof(path));
 for(int bad=0;bad<5;bad++){
  uint8_t changed[2112];memcpy(changed,asset,n);
  if(bad==1)changed[36+3]='2'; /* incompatible wide-header ABI */
  if(bad==2)changed[36+4]=120; /* full tile dimension mismatch */
  if(bad==3)changed[36+1036]=128|122; /* RLE overrun */
  if(bad==4)changed[36+1038+5]=2; /* edge height mismatch */
  assert(pet_cmedia_stage_chunk(PET_CEXT_MEDIA_BIG,"map",0,changed,n,NULL,0));
  assert(pet_cmedia_stage_matches(&r,NULL,0)==(bad==0));pet_cmedia_stage_reset();
  FILE*f=fopen(path,"wb");assert(f);assert(fwrite(changed,1,n,f)==n);assert(!fclose(f));
  pet_cmedia_ticket_t ticket={0};assert(pet_cmedia_prepare(0,0,&r,&ticket)==(bad==0));
  if(!bad)assert(pet_cmedia_abort(&ticket));else assert(!ticket.serial);
 }
 // Truncated stream and missing palette cannot become completed valid tiles.
 asset[36+8]=12;asset[36+9]=asset[36+10]=asset[36+11]=0;
 pet_cext_big_t meta;assert(!pet_cext_big_header(asset,n,&meta));
 return 0;}
'''
    src=tmp_path/'big.c';src.write_text(code+body);exe=tmp_path/'big'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I'+str(tmp_path),'-I'+str(ROOT/'main'),'-I'+str(ROOT/'tests/vendor/cjson'),str(src),str(ROOT/'main/pet_p4_component_ext.c'),str(ROOT/'main/pet_p4_component_events.c'),str(ROOT/'main/pet_p4_sfx_core.c'),str(ROOT/'tests/vendor/cjson/cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=tmp_path,check=True)
