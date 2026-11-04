"""Real SDK section proof for internal zero metadata; no link or device IO."""
import re
import pytest
import p4_storage_zero_bss_test as sdk


@pytest.mark.parametrize('profile', [sdk.V1, sdk.V3])
@pytest.mark.parametrize('filename,wanted', [
    ('pet_p4_spiffs_hint.c', {'hints':4096, 'replace':64}),
    ('pet_p4_storage_io_trace.c', {'io_status':40}),
])
def test_aux_zero_metadata_stays_internal_nobits(tmp_path, monkeypatch, profile, filename, wanted):
    monkeypatch.setattr(sdk, 'SOURCE', sdk.ROOT / 'main' / filename)
    target = tmp_path / (filename + '.o')
    compiler, base, directory = sdk._compile_arguments(sdk._database(profile), target)
    sdk._run([compiler, *base, *sdk._profile_cpp(profile), '-c', sdk.SOURCE, '-o', target], directory)
    readelf = compiler.with_name('riscv32-esp-elf-readelf')
    sections = sdk._run([readelf, '-SW', target], directory)
    symbols = sdk._run([readelf, '-sW', target], directory)
    index = {int(i):(name,kind) for i,name,kind in re.findall(
        r'^\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)', sections, re.MULTILINE)}
    found = {}
    for size, section_index, name in re.findall(
        r'^\s*\d+:\s+[0-9a-fA-F]+\s+(\d+)\s+OBJECT\s+LOCAL\s+\w+\s+(\d+)\s+(\S+)\s*$',
        symbols, re.MULTILINE):
        if name not in wanted:
            continue
        section, kind = index[int(section_index)]
        assert kind == 'NOBITS'
        assert section.startswith(('.bss', '.sbss')) and 'ext_ram' not in section
        found[name] = int(size)
    assert found == wanted
