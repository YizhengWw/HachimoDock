from pathlib import Path
import importlib.util
import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('budget', ROOT / 'tools/firmware_budget.py')
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


def test_headroom_and_internal_budget():
    sections = fixture()
    result = budget.evaluate(2350000, 0x280000, sections)
    assert result['otaHeadroomBytes'] == 271440
    assert result['internalStaticBytes'] == 345000
    assert result['internalSections']['.dram1.bss'] == 208000
    assert result['memoryAccounting'] == 'p4-allocated-ram-v2'
    with pytest.raises(ValueError, match='OTA headroom'):
        budget.evaluate(0x280000 - 100, 0x280000, sections)
    with pytest.raises(ValueError, match='Internal static'):
        budget.evaluate(2350000, 0x280000, {**sections, '.dram1.bss': {'size':258000,'address':0x4ff40000}})


def fixture():
    return {'.iram0.text': {'size':110000,'address':0x4ff00000},
            '.dram0.data': {'size':25000,'address':0x4ff20000},
            '.dram0.bss': {'size':2000,'address':0x4ff27000},
            '.dram1.bss': {'size':208000,'address':0x4ff40000},
            '.ext_ram.bss': {'size':1600000,'address':0x48240000}}


def test_tcm_rtc_and_new_internal_section_names_cannot_escape_accounting():
    sections = fixture()
    sections.update({'.tcm.text':{'size':44,'address':0x30100000},
                     '.tcm.data':{'size':60,'address':0x3010002c},
                     '.rtc_reserved':{'size':24,'address':0x50108000},
                     '.rtc.text':{'size':104,'address':0x50108018},
                     '.future_ram':{'size':32,'address':0x4ff80000},
                     '.flash_rodata_dummy':{'size':100000,'address':0x48000000},
                     '.ext_ram.dummy':{'size':100000,'address':0x48100000}})
    result = budget.evaluate(2350000,0x280000,sections)
    assert result['internalStaticBytes'] == 345264 and result['externalStaticBytes'] == 1600000
    assert '.future_ram' in result['internalSections'] and '.flash_rodata_dummy' not in result['internalSections']


@pytest.mark.parametrize('record', [
    {'size':-1,'address':0x4ff80000}, {'size':True,'address':0x4ff80000},
    {'size':1.5,'address':0x4ff80000}, {'size':32,'address':0x4ffbfff0},
    {'size':64,'address':0xfffffff0}, {'size':1,'address':0x4ff00000},
    {'size':4,'address':0x4ff80000,'extra':0}, 100,
])
def test_malformed_crossing_and_overlapping_sections_rejected(record):
    with pytest.raises(ValueError):
        budget.evaluate(2350000,0x280000,{**fixture(),'.future_ram':record})


def test_missing_or_forged_internal_section_identity_rejected():
    sections = fixture();del sections['.dram0.bss']
    with pytest.raises(ValueError,match='Missing'):
        budget.evaluate(2350000,0x280000,sections)
    with pytest.raises(ValueError):
        budget.evaluate(2350000,0x280000,{**fixture(),'.dram0.bss':{'size':2000,'address':0}})
    with pytest.raises(ValueError):
        budget.evaluate(2350000,0x280000,{**fixture(),'.private/path':{'size':10,'address':0x4ff80000}})


def test_size_report_malformed_or_duplicate_rows_do_not_produce_budget(tmp_path,monkeypatch):
    binary=tmp_path/'firmware.bin';binary.write_bytes(b'x')
    partitions=tmp_path/'partitions.csv';partitions.write_text('ota_0,app,ota_0,0x10000,0x280000\n')
    for output in ['.dram0.bss 10 1341128704\n.dram0.bss 0 1341128704', '.dram0.bss -1 1341128704', '.dram0.bss 1 missing']:
        monkeypatch.setattr(budget.subprocess,'check_output',lambda *args,**kwargs:output)
        with pytest.raises(ValueError,match='Malformed or duplicate'):
            budget.check(binary,partitions,'unused')
        assert not binary.with_name('firmware-budget.json').exists()


def test_relative_pio_filename_header_is_not_a_memory_section(tmp_path,monkeypatch):
    binary=tmp_path/'firmware.bin';binary.write_bytes(b'x')
    partitions=tmp_path/'partitions.csv';partitions.write_text('ota_0,app,ota_0,0x10000,0x280000\n')
    output='.pio/build/my target/firmware.elf  :\nsection size addr\n'
    output+='\n'.join(f'{name} {row["size"]} {row["address"]}' for name,row in fixture().items())
    monkeypatch.setattr(budget.subprocess,'check_output',lambda *args,**kwargs:output)
    assert budget.check(binary,partitions,'unused')['internalStaticBytes']==345000
