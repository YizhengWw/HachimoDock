"""Real catalog writer + actual SDK/NOR regression, with public synthetic data.

No serial, flash device, dump, credentials, network or dependency installation.
SDK filesystem code runs only over a private memory array (NOR write = old&new).
The historical writer is a runtime negative, not a replacement production model.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import pytest
from p4_catalog_membership_reboot_test import function

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = Path(__file__).resolve().parent / 'fixtures'


def synthetic_catalog():
    items = [dict(widgetId=f'fixture-{i:02d}', title='Item', slot=i,
                  packageGeneration=i % 2, widgetChecksum=f'{0x1000+i:08x}',
                  buttonsChecksum=f'{0x2000+i:08x}', spritesChecksum=f'{0x3000+i:08x}')
             for i in range(32)]
    catalog = dict(version=3, sequence=486, check='00000000',
                   activeWidgetId='fixture-31', seedPending=False,
                   selectedWidgetId='fixture-00', items=items)
    encode = lambda: json.dumps(catalog, separators=(',', ':'), ensure_ascii=False).encode()
    remaining = 5625 - len(encode())
    assert remaining >= 0
    for item in items:
        take = min(remaining, 63 - len(item['title']))
        item['title'] += 'x' * take
        remaining -= take
    assert remaining == 0
    data = encode()
    start = data.index(b'"check":"') + len(b'"check":"')
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    data = data[:start] + f'{value:08x}'.encode() + data[start+8:]
    assert len(data) == 5625 and len(json.loads(data)['items']) == 32
    return data


def sdk_source():
    explicit = os.environ.get('PET_TEST_SPIFFS_SRC')
    packages = Path(os.environ.get('PLATFORMIO_PACKAGES_DIR',
                    str(Path(os.environ.get('PLATFORMIO_CORE_DIR', str(Path.home()/'.platformio'))) / 'packages')))
    src = Path(explicit) if explicit else packages/'framework-espidf/components/spiffs/spiffs/src'
    if not (src/'spiffs_nucleus.c').is_file():
        pytest.fail('Existing SDK required: set PET_TEST_SPIFFS_SRC; no download/install is attempted')
    return src


@pytest.fixture(scope='module')
def writer_hosts(tmp_path_factory):
    work = tmp_path_factory.mktemp('catalog-zero-nor')
    source = (ROOT/'main/pet_p4_miniapp.c').read_text()
    template = (FIXTURES/'catalog_zero_rewrite_host.c.in').read_text()
    sdk = sdk_source()
    inc = work/'include'
    inc.mkdir()
    # Original production SDK configuration header; only logging/locking host
    # adapters and the existing explicit target settings are provided locally.
    (inc/'spiffs_config.h').write_bytes((sdk.parents[1]/'include/spiffs_config.h').read_bytes())
    (inc/'sdkconfig.h').write_text('''#pragma once
#define CONFIG_SPIFFS_MAX_PARTITIONS 3
#define CONFIG_SPIFFS_CACHE 1
#define CONFIG_SPIFFS_CACHE_WR 1
#define CONFIG_SPIFFS_PAGE_CHECK 1
#define CONFIG_SPIFFS_GC_MAX_RUNS 64
#define CONFIG_SPIFFS_PAGE_SIZE 512
#define CONFIG_SPIFFS_OBJ_NAME_LEN 32
#define CONFIG_SPIFFS_USE_MAGIC 1
#define CONFIG_SPIFFS_USE_MAGIC_LENGTH 1
#define CONFIG_SPIFFS_META_LENGTH 4
#define CONFIG_SPIFFS_USE_MTIME 1
''')
    (inc/'esp_log.h').write_text('#pragma once\n#define ESP_LOGD(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n')
    (inc/'esp_assert.h').write_text('#pragma once\n#define ESP_STATIC_ASSERT(c,m) _Static_assert(c,m)\n')
    (inc/'pet_p4_file_read.h').write_bytes((ROOT/'main/pet_p4_file_read.h').read_bytes())
    vendor = ROOT/'tests/vendor/cjson'
    (work/'home').mkdir()
    (work/'tmp').mkdir()
    env = dict(HOME=str(work/'home'), TMPDIR=str(work/'tmp'), PATH='/usr/bin:/bin', LC_ALL='C',
               ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    common = [shutil.which('cc') or 'cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-Wno-error=deprecated-declarations', '-fsanitize=address,undefined',
              '-fno-omit-frame-pointer', '-include', 'inttypes.h',
              '-I', str(inc), '-I', str(sdk), '-I', str(vendor)]
    commands = []

    def invoke(argv, name):
        run = subprocess.run(argv, cwd=work, env=env, capture_output=True, timeout=60)
        (work/(name+'.stdout')).write_bytes(run.stdout)
        (work/(name+'.stderr')).write_bytes(run.stderr)
        commands.append(dict(argv=argv, exitCode=run.returncode))
        assert run.returncode == 0, (name, run.stderr.decode(errors='replace'))

    objects = []
    for name in ['spiffs_cache.c', 'spiffs_check.c', 'spiffs_gc.c', 'spiffs_hydrogen.c', 'spiffs_nucleus.c']:
        obj = work/(name+'.o')
        # Exact SDK uses packed, unaligned index tables. Exempt only SDK
        # alignment instrumentation; other UBSan+ASan stay enabled. The host
        # and extracted application writer retain full UBSan+ASan.
        invoke(common+['-fno-sanitize=alignment', '-c', str(sdk/name), '-o', str(obj)], name)
        objects.append(str(obj))
    start = source.index('typedef struct {\n  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];\n  char title[64];')
    end = source.index('} miniapp_catalog_item_t;', start) + len('} miniapp_catalog_item_t;')
    declarations = source[start:end] + '\ntypedef struct {uint8_t generation;uint32_t sequence;} miniapp_catalog_commit_t;\n'
    common_names = ['catalog_find', 'catalog_snapshot_path', 'catalog_hex_digit',
                    'catalog_check_digits', 'catalog_check_value', 'catalog_check_seal']
    for label in ['current', 'historical']:
        body = declarations + ''.join(function(source, name) for name in common_names)
        if label == 'current':
            body += function(source, 'catalog_file_matches') + function(source, 'write_catalog_in_place')
        else:
            body += (FIXTURES/'catalog_in_place_legacy.c').read_text()
        body += function(source, 'write_catalog_snapshot_at')
        path = work/(label+'.c')
        path.write_text(template.replace('/* ACTUAL_CATALOG_SOURCE */', body))
        # Historical mode does not call readback adapters; unused test adapters
        # are expected. No behavior/result assertions are disabled.
        invoke(common+['-Wno-unused-function', str(path), str(vendor/'cJSON.c'), *objects,
                       '-o', str(work/label)], label)
    target = work/'synthetic-5625.json'
    target.write_bytes(synthetic_catalog())
    (work/'source-binding.json').write_text(json.dumps(dict(
        sourceSHA256=hashlib.sha256(source.encode()).hexdigest(), commands=commands,
        SDKSourceSHA256={name: hashlib.sha256((sdk/name).read_bytes()).hexdigest()
                        for name in ['spiffs_nucleus.c', 'spiffs_hydrogen.c']},
        data='PUBLIC_SYNTHETIC_32_ITEMS_NO_DEVICE_PROFILE_OR_RAW_DUMP'), indent=2))
    return work, target, env


def test_actual_writer_cross_page_shrink_grow_and_failure_quarantine(writer_hosts):
    work, target, env = writer_hosts
    run = subprocess.run([str(work/'current'), str(target), '1'], cwd=work, env=env,
                         capture_output=True, timeout=60)
    (work/'current-run.stdout').write_bytes(run.stdout)
    (work/'current-run.stderr').write_bytes(run.stderr)
    assert run.returncode == 0, run.stderr.decode(errors='replace')
    rows = [json.loads(line) for line in run.stdout.splitlines()]
    assert len(rows) == 17 and all(row['activeGenerationUnchanged'] for row in rows)
    assert rows[0]['coldTargetDiffBytes'] == 0 and rows[0]['commitSequence'] == 486
    assert rows[0]['readbackCalls'] > 0 and rows[0]['maxReadbackChunk'] <= 64
    assert all(not row['ok'] and row['commitSequence'] == 999 for row in rows[1:])
    assert all(row['quarantine'] == (row['fault'] not in (1, 16)) for row in rows[1:])


def test_historical_success_ack_is_not_a_cold_read_integrity_proof(writer_hosts):
    work, target, env = writer_hosts
    run = subprocess.run([str(work/'historical'), str(target), '2'], cwd=work, env=env,
                         capture_output=True, timeout=60)
    (work/'historical-negative.stdout').write_bytes(run.stdout)
    (work/'historical-negative.stderr').write_bytes(run.stderr)
    assert run.returncode != 0
    assert b'diffs==0' in run.stderr  # runtime assertion, not compile failure
    assert b'AddressSanitizer' not in run.stderr and b'runtime error:' not in run.stderr
    row = json.loads(run.stdout.splitlines()[0])
    assert row['ok'] and row['commitSequence'] == 486 and row['coldTargetDiffBytes'] > 0
