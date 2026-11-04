"""Real v1/v3 SDK objects: internal zero metadata must not occupy flash data.

Reuses existing compile databases and the exact profile CPP flags. Outputs only
pytest tmp_path; never runs PlatformIO, changes SDK/config or links firmware.
Full-image internal placement/headroom remain the parent build's responsibility.
"""
import configparser
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'main/pet_p4_storage_io_trace.c'
V1 = 'esp32_p4_program_public_candidate'
V3 = 'esp32_p4_program_public_candidate_v3'


def _database(profile):
    override = os.environ.get('PET_P4_V3_COMPILE_COMMANDS' if profile == V3 else 'PET_P4_V1_COMPILE_COMMANDS')
    if override:
        path = Path(override).resolve()
    else:
        path = ROOT / '.pio/build' / profile / 'compile_commands.json'
        if not path.is_file() and profile == V3:
            # Existing isolated SDK tree; ambiguous environments require an
            # explicit path, never an arbitrary newest/stale compile database.
            candidates = sorted(ROOT.parents[2].glob(
                '*/v3-isolated-exact-*/v3-build-tree/' + profile + '/compile_commands.json'))
            assert len(candidates) == 1, 'set PET_P4_V3_COMPILE_COMMANDS to the existing exact SDK database'
            path = candidates[0]
    assert path.is_file(), f'existing SDK compile database required: {path}'
    return path


def _profile_cpp(profile):
    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / 'platformio.ini')

    def expand(section, chain=()):
        assert section not in chain, 'build_flags reference cycle'
        assert section in config, section
        own = config[section]
        if 'build_flags' not in own:
            parents = [p.strip() for p in own.get('extends', '').split(',') if p.strip()]
            assert len(parents) == 1, f'exact inherited build_flags required: {section}'
            return expand(parents[0], (*chain, section))
        text = '\n'.join(line.split(';', 1)[0] for line in own['build_flags'].splitlines())
        text = re.sub(r'\$\{([^}]+)\.build_flags\}',
                      lambda m: expand(m[1], (*chain, section)), text)
        assert '${' not in text, f'unresolved build_flags expression: {section}'
        return text

    tokens = shlex.split(expand('env:' + profile))
    # Retain every CPP definition/undefinition in order. Only -Wl options
    # are link-only (this test does not link); reject unknown compiler switches
    # instead of silently changing the actual SDK object configuration.
    assert tokens and all((t.startswith(('-D', '-U')) and len(t) > 2)
                          or t.startswith('-Wl,') for t in tokens), tokens
    return [t for t in tokens if not t.startswith('-Wl,')]


def _compile_arguments(database, output):
    entries = json.loads(database.read_text())
    matches = [e for e in entries if Path(e['file']).resolve() == SOURCE.resolve()]
    assert len(matches) == 1, 'database must contain the actual production source exactly once'
    entry = matches[0]
    argv = entry.get('arguments') or shlex.split(entry['command'])
    compiler = Path(argv[0])
    assert compiler.is_file() and compiler.name == 'riscv32-esp-elf-gcc', compiler
    args = []
    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg in ('-o', '-MF', '-MT', '-MQ'):
            i += 2
            continue
        if arg in ('-MD', '-MMD', '-MP'):
            i += 1
            continue
        if arg == '-c' or arg == entry['file']:
            i += 1
            continue
        args.append(arg)
        i += 1
    assert not any('PET_STORAGE_IO_HOST_TEST' in a for a in args), 'real SDK only, no host macro'
    return compiler, args, Path(entry['directory'])


def _run(argv, directory):
    result = subprocess.run([str(a) for a in argv], cwd=directory, capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


@pytest.mark.parametrize('profile,revision,patch', [(V1, (1, 199), 1), (V3, (300, 399), 4)])
def test_actual_sdk_page_cache_metadata_is_zero_nobits_with_exact_profile(tmp_path, profile, revision, patch):
    database = _database(profile)
    output = tmp_path / (profile + '.o')
    compiler, base, directory = _compile_arguments(database, output)
    cpp = _profile_cpp(profile)
    args = [*base, *cpp]
    macro_text = _run([compiler, *args, '-E', '-dM', SOURCE], directory)
    macros = dict(re.findall(r'^#define[ \t]+(\w+)[ \t]+([^\n]+)', macro_text, re.MULTILINE))
    expected = {
        'CONFIG_IDF_TARGET_ESP32P4': '1',
        'CONFIG_ESP_REV_MIN_FULL': str(revision[0]),
        'CONFIG_ESP_REV_MAX_FULL': str(revision[1]),
        'ESP_IDF_VERSION_MAJOR': '5', 'ESP_IDF_VERSION_MINOR': '5', 'ESP_IDF_VERSION_PATCH': str(patch),
        'PET_P4_STORAGE_IO_TRACE_ENABLE': '1',
        'PET_P4_STORAGE_IO_RETAIN_COMPLETED': '1',
        'PET_P4_STORAGE_PAGE_CACHE_ENABLE': '1',
        'PET_P4_STORAGE_PAGE_CACHE_APPLY': '1',
        'PET_P4_PUBLIC_PROGRAM_PROFILE': '1',
    }
    assert {key: macros.get(key) for key in expected} == expected
    assert 'PET_STORAGE_IO_HOST_TEST' not in macros
    _run([compiler, *args, '-c', SOURCE, '-o', output], directory)
    readelf = compiler.with_name('riscv32-esp-elf-readelf')
    assert readelf.is_file()
    section_text = _run([readelf, '-SW', output], directory)
    symbol_text = _run([readelf, '-sW', output], directory)
    sections = {int(index): (name, kind) for index, name, kind in re.findall(
        r'^\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)', section_text, re.MULTILINE)}
    found = {}
    for value, size, binding, index, name in re.findall(
            r'^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+OBJECT\s+(\w+)\s+\w+\s+(\d+)\s+(page_entries|page_replace|counters|completed_io)\s*$',
            symbol_text, re.MULTILINE):
        assert name not in found
        section, kind = sections[int(index)]
        assert binding == 'LOCAL'
        assert kind == 'NOBITS', (name, section, kind)
        assert section == '.bss' or section.startswith(('.bss.', '.sbss.')), (name, section)
        assert 'ext_ram' not in section and 'dram1.' not in section
        found[name] = {'size': int(size), 'section': section, 'type': kind}
    assert {name: entry['size'] for name, entry in found.items()} == {'page_entries': 16384, 'page_replace': 1024, 'counters': 576, 'completed_io': 1848}
    (tmp_path / 'proof.json').write_text(json.dumps({
        'profile': profile, 'database': str(database), 'compiler': str(compiler),
        'sourceSha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        'profileCppFlags': cpp, 'actualMacros': expected, 'symbols': found,
        'objectOnly': True, 'finalInternalAddressAndOtaBudgetRequireParentLink': True,
    }, indent=2) + '\n')
