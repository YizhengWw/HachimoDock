"""Exact SDK guard matrix and opt-in real RV32 SDK compilation, never PIO/device."""
import os
import re
import subprocess
from pathlib import Path
import pytest
ROOT = Path(__file__).resolve().parents[1]
FILES = ('pet_p4_reset_phase.c', 'pet_p4_flash_phase.c', 'pet_p4_native_panic.c')


@pytest.mark.parametrize('name', FILES)
def test_exact_version_revision_target_and_tee_guard(tmp_path, name):
    source = (ROOT/'main'/name).read_text()
    start = source.index('#if !defined(CONFIG_IDF_TARGET_ESP32P4)') if name == FILES[0] else source.index('#if !defined(PET_')
    guard = source[start:source.index('#endif', start)+6]
    for version, lo, hi, target, tee, accepted in [
        (50501,1,199,True,0,True), (50504,300,399,True,0,True),
        (50504,1,199,True,0,False),(50504,300,400,True,0,False),
        (50504,0,399,True,0,False),(50504,300,399,False,0,False),
        (50504,300,399,True,1,False),(50503,300,399,True,0,False),
        (50505,300,399,True,0,False)]:
        unit = tmp_path/'gate.c'
        unit.write_text(f'#define ESP_IDF_VERSION {version}\n#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n'
                        f'#define CONFIG_ESP_REV_MIN_FULL {lo}\n#define CONFIG_ESP_REV_MAX_FULL {hi}\n#define CONFIG_SECURE_ENABLE_TEE {tee}\n'
                        + ('#define CONFIG_IDF_TARGET_ESP32P4 1\n' if target else '') + guard)
        result = subprocess.run(['cc','-E',str(unit)],capture_output=True,text=True)
        assert (result.returncode == 0) == accepted, (name,version,lo,hi,target,tee,result.stderr)


@pytest.mark.parametrize('family', ('v1','v3'))
def test_real_pinned_sdk_rv32_types_and_production_objects(tmp_path, family):
    sdk_value = os.environ.get('PET_V3_SDK_ROOT') if family == 'v3' else os.environ.get('PET_V1_SDK_ROOT', str(Path.home()/'.platformio/packages/framework-espidf'))
    cc_value = os.environ.get('PET_V3_SDK_CC') if family == 'v3' else os.environ.get('PET_V1_SDK_CC', str(Path.home()/'.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-gcc'))
    if not sdk_value or not cc_value or not Path(sdk_value).is_dir() or not Path(cc_value).is_file():
        pytest.skip('set PET_V3_SDK_ROOT and PET_V3_SDK_CC to exact installed SDK5.5.4/toolchain20260121')
    sdk, cc = Path(sdk_value), Path(cc_value)
    assert ('set(IDF_VERSION_PATCH 4)' if family == 'v3' else 'set(IDF_VERSION_PATCH 1)') in (sdk/'tools/cmake/version.cmake').read_text()
    config = (ROOT/('sdkconfig.esp32_p4_program_public_candidate_v3' if family == 'v3' else 'sdkconfig.esp32_p4_program_public_candidate')).read_text()
    header = []
    for line in config.splitlines():
        m = re.fullmatch(r'(CONFIG_\w+)=(.+)', line)
        if m: header.append(f'#define {m[1]} {"1" if m[2] == "y" else m[2]}')
    (tmp_path/'sdkconfig.h').write_text('\n'.join(header)+'\n')
    paths = [('soc/esp32p4/register/hw_ver3' if family == 'v3' else 'soc/esp32p4/register/hw_ver1'),'soc/esp32p4/include','soc/esp32p4/register','hal/esp32p4/include','esp_rom/esp32p4/include',
             'esp_rom/include/esp32p4','esp_hw_support/port/esp32p4/include','esp_hw_support/port/include','freertos/config/include','freertos/config/include/freertos','freertos/config/riscv/include',
             'freertos/FreeRTOS-Kernel/include','freertos/FreeRTOS-Kernel/portable/riscv/include',
             'freertos/FreeRTOS-Kernel/portable/riscv/include/freertos','freertos/esp_additions/include','newlib/platform_include','esp_libc/platform_include']
    includes = [tmp_path, ROOT/'main'] + [sdk/'components'/p for p in paths]
    includes += [p for p in sorted((sdk/'components').glob('*/include')) if p.parent.name != 'linux']
    includes += sorted((sdk/'components').glob('*/private_include'))
    flags = ['-std=gnu11','-march=rv32imafc_zicsr_zifencei','-mabi=ilp32f','-ffunction-sections','-fdata-sections',
             '-Werror=implicit-function-declaration','-Werror=incompatible-pointer-types',
             '-DPET_P4_RESET_PHASE_TRACE=1','-DPET_P4_FLASH_PHASE_TRACE=1','-DPET_P4_NATIVE_PANIC_TRACE=1']
    flags += [item for p in includes if p.is_dir() for item in ('-I',str(p))]
    objects = []
    for name in FILES:
        obj = tmp_path/(name+'.o')
        result = subprocess.run([str(cc),*flags,'-c',str(ROOT/'main'/name),'-o',str(obj)],capture_output=True,text=True)
        assert result.returncode == 0, result.stderr
        objects.append(obj)
    # All actual SDK header types above are target-compiled with production
    # wrappers. Check SDK provider definitions and the original wrap flags too.
    panic = (sdk/'components/esp_system/panic.c').read_text()
    assert re.search(r'void\s+(?:IRAM_ATTR\s+)?esp_panic_handler\s*\(panic_info_t\s*\*\s*info\)',panic)
    ini = (ROOT/'platformio.ini').read_text()
    wraps = ['esp_partition_erase_range','esp_flash_erase_region','spi_flash_chip_generic_yield',
             'spi_flash_chip_generic_erase_sector','spi_flash_chip_winbond_erase_sector',
             'spi_flash_chip_generic_wait_idle','spi_flash_check_and_flush_cache','esp_panic_handler']
    for name in wraps: assert '-Wl,--wrap='+name in ini
    # Relocatable target link retains the real wrapper code and external SDK
    # references, without pretending to certify final ELF placement/coverage.
    linked = tmp_path/'hooks.o'
    subprocess.run([str(cc),'-nostdlib','-r',*[str(p) for p in objects],
                    *['-Wl,--wrap='+n for n in wraps],'-o',str(linked)],check=True,capture_output=True,text=True)
    nm = str(cc).replace('gcc','nm')
    symbols = subprocess.run([nm,'-S',str(linked)],check=True,capture_output=True,text=True).stdout
    for name in wraps:
        assert re.search(r'\bT\s+__wrap_'+name+r'$',symbols,re.M), name
        assert re.search(r'\bU\s+'+name+r'$',symbols,re.M), name
    assert re.search(r'\bT\s+pet_flash_phase_coverage$',symbols,re.M)
    # Section placement is asserted on actual compiled objects, not host macros.
    objdump = str(cc).replace('gcc','objdump')
    table = subprocess.run([objdump,'-t',str(linked)],check=True,capture_output=True,text=True).stdout
    for name in wraps:
        assert any('.iram' in line and line.rstrip().endswith('__wrap_'+name) for line in table.splitlines()), name
    for name in ('retained','panic_records'):
        assert any('.rtc_noinit' in line and line.rstrip().endswith(name) for line in table.splitlines()), name
