"""Actual passive wrappers: unchanged native arguments/results/errno, no IO."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_native_flash_wrappers_forward_once_and_preserve_failures_errno(tmp_path):
    (tmp_path/'esp_attr.h').write_text('#define IRAM_ATTR\n')
    (tmp_path/'esp_idf_version.h').write_text('#define ESP_IDF_VERSION 50501\n#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n')
    (tmp_path/'esp_flash.h').write_text('#pragma once\n#include <stdint.h>\ntypedef int esp_err_t;typedef struct {int value;} esp_flash_t;\n')
    (tmp_path/'esp_partition.h').write_text('#pragma once\n#include <stddef.h>\n#include "esp_flash.h"\ntypedef struct {int value;} esp_partition_t;\n')
    source=tmp_path/'wrappers.c';binary=tmp_path/'wrappers'
    source.write_text(r'''
#include <assert.h>
#include <errno.h>
#include "pet_p4_flash_phase.c"
static unsigned enters,leaves,calls,expected_phase;
static bool denied;
static const esp_partition_t partition={3};static esp_flash_t chip={4};
pet_reset_phase_scope_t pet_reset_phase_enter(pet_reset_phase_t phase){
 assert((unsigned)phase==expected_phase);enters++;errno=71;
 return (pet_reset_phase_scope_t){.active=!denied};
}
void pet_reset_phase_leave(pet_reset_phase_scope_t *scope){assert(scope->active==!denied);leaves++;errno=72;}
esp_err_t __real_esp_partition_erase_range(const esp_partition_t *p,size_t a,size_t n){assert(p==&partition&&a==17&&n==19&&errno==11);calls++;errno=31;return -7;}
esp_err_t __real_esp_flash_erase_region(esp_flash_t *p,uint32_t a,uint32_t n){assert(p==&chip&&a==17&&n==19&&errno==11);calls++;errno=31;return -8;}
esp_err_t __real_spi_flash_chip_generic_yield(esp_flash_t *p,uint32_t v){assert(p==&chip&&v==17&&errno==11);calls++;errno=31;return -9;}
esp_err_t __real_spi_flash_chip_generic_erase_sector(esp_flash_t *p,uint32_t v){assert(p==&chip&&v==17&&errno==11);calls++;errno=31;return -10;}
esp_err_t __real_spi_flash_chip_winbond_erase_sector(esp_flash_t *p,uint32_t v){assert(p==&chip&&v==17&&errno==11);calls++;errno=31;return -12;}
esp_err_t __real_spi_flash_chip_generic_wait_idle(esp_flash_t *p,uint32_t v){assert(p==&chip&&v==17&&errno==11);calls++;errno=31;return -11;}
bool __real_spi_flash_check_and_flush_cache(size_t a,size_t n){assert(a==17&&n==19&&errno==11);calls++;errno=31;return false;}
#define CASE(phase,expression) do{expected_phase=(phase);errno=11;unsigned old=calls;assert(expression);assert(errno==31&&calls==old+1&&enters==calls&&leaves==calls);}while(0)
int main(void){for(unsigned context=0;context<2;context++){
 denied=context;
 CASE(PET_PHASE_PARTITION_ERASE,__wrap_esp_partition_erase_range(&partition,17,19)==-7);
 CASE(PET_PHASE_FLASH_ERASE,__wrap_esp_flash_erase_region(&chip,17,19)==-8);
 CASE(PET_PHASE_FLASH_YIELD,__wrap_spi_flash_chip_generic_yield(&chip,17)==-9);
 CASE(PET_PHASE_FLASH_SECTOR,__wrap_spi_flash_chip_generic_erase_sector(&chip,17)==-10);
 CASE(PET_PHASE_FLASH_SECTOR,__wrap_spi_flash_chip_winbond_erase_sector(&chip,17)==-12);
 CASE(PET_PHASE_FLASH_WAIT_IDLE,__wrap_spi_flash_chip_generic_wait_idle(&chip,17)==-11);
 CASE(PET_PHASE_FLASH_CACHE_FLUSH,!__wrap_spi_flash_check_and_flush_cache(17,19));
}return 0;}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-DPET_P4_FLASH_PHASE_TRACE=1','-DPET_P4_RESET_PHASE_TRACE=1','-DPET_FLASH_PHASE_HOST_TEST=1',
      '-I',str(tmp_path),'-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_flash_wrappers_default_off_without_imports_and_no_retry_or_grant(tmp_path):
    obj=tmp_path/'off.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-c',str(ROOT/'main/pet_p4_flash_phase.c'),'-o',str(obj)],check=True)
    assert not subprocess.run(['nm','-g',str(obj)],check=True,capture_output=True,text=True).stdout.strip()
    source=(ROOT/'main/pet_p4_flash_phase.c').read_text()
    for forbidden in ('malloc(', 'calloc(', 'printf(', 'portENTER', 'xTaskCreate', 'vTaskDelay', 'wdt_hal_', 'esp_task_wdt_', 'REG_WRITE', 'while(', 'for('):
        assert forbidden not in source
    config=(ROOT/'platformio.ini').read_text()
    assert config.count('-DPET_P4_FLASH_PHASE_TRACE=1')==1
    assert '"pet_p4_flash_phase.c"' in (ROOT/'main/CMakeLists.txt').read_text()
