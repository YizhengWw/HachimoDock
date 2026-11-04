"""Actual bounded numeric exception observer; never induces a device fault."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_native_panic_forwards_once_retains_only_numeric_same_build_faults(tmp_path):
    for folder in ('esp_private','riscv'):(tmp_path/folder).mkdir()
    (tmp_path/'esp_attr.h').write_text('#define IRAM_ATTR\n#define DRAM_ATTR\n#define RTC_NOINIT_ATTR\n')
    (tmp_path/'esp_idf_version.h').write_text('#define ESP_IDF_VERSION 50501\n#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n')
    (tmp_path/'esp_app_desc.h').write_text('#include <stdint.h>\ntypedef struct {uint8_t app_elf_sha256[32];} esp_app_desc_t;const esp_app_desc_t *esp_app_get_description(void);\n')
    (tmp_path/'esp_random.h').write_text('#include <stdint.h>\nuint32_t esp_random(void);\n')
    (tmp_path/'esp_system.h').write_text('typedef enum {ESP_RST_SW,ESP_RST_PANIC,ESP_RST_INT_WDT,ESP_RST_TASK_WDT,ESP_RST_WDT,ESP_RST_POWERON} esp_reset_reason_t;esp_reset_reason_t esp_reset_reason(void);\n')
    (tmp_path/'esp_private/panic_internal.h').write_text('#include <stdbool.h>\ntypedef struct {int core,exception;const void *frame;bool pseudo_excause;} panic_info_t;extern bool g_panic_abort;\n')
    (tmp_path/'riscv/rvruntime-frames.h').write_text('#include <stdint.h>\ntypedef struct {uint32_t mepc,ra,mcause;} RvExcFrame;\n')
    source=tmp_path/'native.c';binary=tmp_path/'native'
    source.write_text(r'''
#include <assert.h>
#include "pet_p4_native_panic.c"
static esp_app_desc_t app={{1,2,3}};static esp_reset_reason_t reason=ESP_RST_SW;
static unsigned calls;static panic_info_t *expected;
bool g_panic_abort;
const esp_app_desc_t *esp_app_get_description(void){return &app;}
uint32_t esp_random(void){return 17;}
esp_reset_reason_t esp_reset_reason(void){return reason;}
void __real_esp_panic_handler(panic_info_t *info){assert(info==expected);calls++;}
static void boot(void){panic_ready=false;panic_entered[0]=panic_entered[1]=false;memset(&previous_panic,0,sizeof(previous_panic));pet_native_panic_boot();}
int main(void){
 pet_native_panic_snapshot_t out;
 assert(!pet_native_panic_previous(&out));
 RvExcFrame frame={0x40001234,0x4ff00020,5};panic_info_t info={.core=1,.exception=4,.frame=&frame,.pseudo_excause=true};
 expected=&info;__wrap_esp_panic_handler(&info);assert(calls==1&&!panic_records[1].commit);
 boot();assert(pet_native_panic_previous(&out)&&!out.valid_mask);
 g_panic_abort=true;__wrap_esp_panic_handler(&info);assert(calls==2);
 panic_record_t good=panic_records[1];assert(good.site.pc==frame.mepc&&good.site.return_pc==frame.ra&&good.site.cause==5&&good.site.flags==3);
 frame.mepc=0x40005678;__wrap_esp_panic_handler(&info);assert(calls==3&&panic_records[1].site.pc==good.site.pc);
 reason=ESP_RST_PANIC;boot();assert(pet_native_panic_previous(&out)&&out.same_build&&out.valid_mask==2&&out.sites[1].pc==good.site.pc);
 assert(!panic_records[1].commit);pet_native_panic_boot();assert(previous_panic.valid_mask==2);
 for(unsigned word=0;word<sizeof(good)/4;word++){
  panic_records[1]=good;((uint32_t*)&panic_records[1])[word]^=1;boot();assert(!previous_panic.valid_mask);
 }
 for(unsigned cut=0;cut<sizeof(good)/4;cut++){
  memset(panic_records,0,sizeof(panic_records));
  for(unsigned i=0;i<cut;i++)((uint32_t*)&panic_records[1])[i]=((uint32_t*)&good)[i];
  boot();assert(!previous_panic.valid_mask);
 }
 panic_records[1]=good;app.app_elf_sha256[0]^=1;boot();assert(!previous_panic.valid_mask);app.app_elf_sha256[0]^=1;
 panic_records[1]=good;reason=ESP_RST_POWERON;boot();assert(!previous_panic.valid_mask);
 reason=ESP_RST_PANIC;panic_records[0]=good;boot();assert(!previous_panic.valid_mask);
 expected=NULL;__wrap_esp_panic_handler(NULL);assert(calls==4);
 expected=&info;info.core=2;__wrap_esp_panic_handler(&info);assert(calls==5);
 info.core=-1;__wrap_esp_panic_handler(&info);assert(calls==6);
 info.core=0;info.frame=NULL;__wrap_esp_panic_handler(&info);assert(calls==7);
 assert(!pet_native_panic_previous(NULL));return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-DPET_P4_NATIVE_PANIC_TRACE=1','-DPET_NATIVE_PANIC_HOST_TEST=1','-I',str(tmp_path),
      '-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_native_panic_default_off_has_no_imports_or_runtime_cost(tmp_path):
    obj=tmp_path/'off.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),
      '-c',str(ROOT/'main/pet_p4_native_panic.c'),'-o',str(obj)],check=True)
    assert not subprocess.run(['nm','-u',str(obj)],check=True,capture_output=True,text=True).stdout.strip()
    source=(ROOT/'main/pet_p4_native_panic.c').read_text()
    wrapper=source[source.index('void IRAM_ATTR __wrap_'):]
    assert wrapper.count('__real_esp_panic_handler(info);')==1
    for forbidden in ('printf(','malloc(','reason','description','g_panic_abort_details','frame->sp','frame->mtval','vTaskDelay','xSemaphore','uart_'):
        assert forbidden not in wrapper
