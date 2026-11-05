"""Offline trusted program lifecycle adapter; no device, cloud or audio IO.

Synthetic successful admission is in the test C backend only. The actual device
factory uses the real production resource broker, whose quotas remain zero.
No installed package, P4 timing/scheduling or acoustic gate is implied.
"""
from pathlib import Path
import json
import os
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
MODULES = [ROOT / "main" / ("pet_p4_program_" + name + ".c")
           for name in ("core", "image", "runtime", "host")]
CASES = ["owned_staging_quantum", "report_backpressure_restart", "audio_cancel",
         "instruction_cancel_reentry", "failed_start_preserves_committed",
         "binding_faults", "alias_and_arguments", "trap_no_retry", "exhausted_revision_restore",
         "fresh_semantic_zero_with_padding", "transactional_image_publication", "transactional_image_aliases",
         "receiver_output_contract", "projection_commit_and_copy", "projection_latched_revocation",
         "projection_transactional_restart", "projection_invalid_and_aliases", "projection_all_ports", "copied_host_rejected"]


def run(command):
    result = subprocess.run([str(p) for p in command], check=False, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stdout + result.stderr
    assert not result.stderr, result.stderr
    return result.stdout


@pytest.fixture(scope="module")
def executable(tmp_path_factory):
    path = tmp_path_factory.mktemp("program-host") / "host"
    run([os.environ.get("CC", "cc"), *FLAGS, "-I", ROOT / "main", *MODULES,
         ROOT / "tests/p4_program_host_test.c", "-o", path])
    return path


@pytest.mark.parametrize("case", range(len(CASES)), ids=CASES)
def test_host_lifecycle(executable, case):
    result = json.loads(run([executable, case]))
    assert result["ok"] and result["hostBytes"] < 6500


def test_device_factory_real_broker_quota_closed(tmp_path):
    path = tmp_path / "device-host"
    run([os.environ.get("CC", "cc"), *FLAGS, "-I", ROOT / "main",
         "-I", ROOT / "tests/program_probe_stubs",
         "-I", ROOT / "managed_components/espressif__cjson/cJSON", *MODULES,
         ROOT / "main/pet_p4_resources.c", ROOT / "main/pet_p4_resources_device.c",
         ROOT / "main/pet_p4_program_host_device.c",
         ROOT / "tests/p4_program_host_device_test.c", "-o", path])
    assert "quota closed" in run([path])


def test_no_production_exposure_or_ambient_effects():
    for name in ("pet_p4_main.c", "pet_p4_protocol.c", "pet_p4_miniapp.c"):
        text = (ROOT / "main" / name).read_text()
        assert "pet_program_runtime_start(" not in text
        if name!="pet_p4_miniapp.c":assert "pet_program_host_" not in text
    # The opt-in component parent is now a real caller; source-name absence is
    # no longer the boundary. Default/UI/Windows admission stays closed; only
    # the separately measured private foreground/background experiments may
    # opt in. Neither is a production/UI/Windows ancestor.
    # The real factory test above compiles
    # the ordinary broker and proves a program lease cannot be acquired.
    features=(ROOT/'main/pet_p4_component_features.h').read_text()
    assert '#define PET_COMPONENT_PROGRAM_ENABLED 0' in features
    assert '#define PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED 0' in features
    protocol=(ROOT/'main/pet_p4_protocol.c').read_text()
    assert '#if PET_COMPONENT_PROGRAM_ENABLED && PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED' in protocol
    import configparser
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    private='env:esp32_p4_program_candidate'
    background='env:esp32_p4_background_candidate'
    parent='env:esp32_p4_program_parent_candidate'
    acceptance='env:esp32_p4_program_parent_acceptance'
    mixed='env:esp32_p4_framework_mixed_acceptance'
    public='env:esp32_p4_program_public_candidate'  # explicit public profile table; see p4_public_program_profile_test
    assert config['platformio']['default_envs']=='esp32_p4_evboard'
    for name in config.sections():
        if name in (private,background,parent,acceptance,mixed,public):continue
        assert 'PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1' not in config[name].get('build_flags','')
        assert private not in config[name].get('extends','')
        assert background not in config[name].get('extends','')
        assert parent not in config[name].get('extends','')
        assert acceptance not in config[name].get('extends','')
        assert 'PET_COMPONENT_BACKGROUND_ENABLED=1' not in config[name].get('build_flags','')
    assert config[private]['extends']=='env:esp32_p4_canvas_candidate'
    assert config[background]['extends']==private
    assert config[parent]['extends']==background
    assert 'PET_COMPONENT_PROGRAM_PARENT_UNIFIED=1' in config[parent]['build_flags']
    assert config[acceptance]['extends']==parent
    assert 'PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1' in config[acceptance]['build_flags']
    assert config[mixed]['extends']==acceptance
    assert 'PET_P4_STORAGE_CACHE_SCOPE_ENABLED=1' in config[mixed]['build_flags']
    assert 'PET_P4_SERIAL_CONTROL_INTEGRITY=1' in config[mixed]['build_flags']
    assert 'PET_COMPONENT_BACKGROUND_ENABLED=1' in config[background]['build_flags']
    assert '#define PET_COMPONENT_BACKGROUND_ENABLED 0' in features
    limits = (ROOT / "main/pet_p4_resources.c").read_text()
    assert "[PET_RESOURCE_PROGRAM]={0,0}" in limits
    assert "[PET_RESOURCE_PERIPHERAL]={0,0}" in limits
    for name in ("pet_p4_program_host.c", "pet_p4_program_host_device.c"):
        text = (ROOT / "main" / name).read_text()
        for forbidden in ("malloc(", "calloc(", "fopen(", "vTaskDelay(", "while (",
                          "gpio_", "i2c_", "spi_", "dma_", "capture_start(",
                          "stream_begin(", "music_begin(", "pet_program_run("):
            assert forbidden not in text
    adapter = (ROOT / "main/pet_p4_program_host_device.c").read_text()
    assert "pet_p4_audio_active()" in adapter
    assert "pet_p4_audio_playback_active()" in adapter
    assert "pet_p4_audio_conversation_active()" in adapter
    assert "pet_p4_audio_music_active()" in adapter


def test_host_projection_p4_static_stack_and_closed_dependency_surface(tmp_path):
    from p4_program_test import p4_compiler, WARNINGS, FREESTANDING, INCLUDES
    compiler = p4_compiler()
    if not compiler:
        pytest.skip('requires existing P4 compiler, never invokes PlatformIO')
    obj = tmp_path / 'host.o'
    run([compiler, *WARNINGS, '-Os', '-march=rv32imafc_zicsr_zifencei_xesppie', '-mabi=ilp32f',
         *FREESTANDING, '-fstack-usage', *INCLUDES, '-c', ROOT/'main/pet_p4_program_host.c', '-o', obj])
    prefix = str(compiler).removesuffix('gcc')
    imports = {line.split()[-1] for line in run([prefix+'nm', '-u', obj]).splitlines()}
    assert imports <= {'memcpy', 'memset', 'pet_program_image_program', 'pet_program_output_contract_valid',
                       'pet_program_runtime_init', 'pet_program_runtime_start_with_publication',
                       'pet_program_runtime_enqueue', 'pet_program_runtime_pump', 'pet_program_runtime_pump_with_checkpoint', 'pet_program_runtime_cancel',
                       'pet_program_runtime_take_report', 'pet_program_runtime_snapshot',
                        'pet_program_runtime_snapshot_if_changed',
                       'pet_program_state_read'}  # Pure bounded committed-state accessor; no grants or IO.
    # This edge is the cursor-aware read-only companion of snapshot. Its callee
    # is independently covered by test_runtime_p4_object_only's closed core
    # imports and static <=256B frames; no IO/allocator/service grant is added.
    assert 'pet_program_runtime_snapshot_if_changed' in imports
    rows = obj.with_suffix('.su').read_text().splitlines()
    assert any('pet_program_host_project_current' in row for row in rows)
    for row in rows:
        _, amount, kind = row.split('\t')
        assert kind == 'static' and int(amount) <= 256


def test_activation_projection_p4_static_stack_and_worker_boundary(tmp_path):
    from p4_program_test import p4_compiler, WARNINGS, FREESTANDING, INCLUDES
    compiler = p4_compiler()
    if not compiler:
        pytest.skip('requires existing P4 compiler, never invokes PlatformIO')
    obj = tmp_path / 'activation.o'
    run([compiler, *WARNINGS, '-Os', '-march=rv32imafc_zicsr_zifencei_xesppie', '-mabi=ilp32f',
         *FREESTANDING, '-fstack-usage', *INCLUDES, '-c', ROOT/'main/pet_p4_program_activation.c', '-o', obj])
    prefix = str(compiler).removesuffix('gcc')
    imports = {line.split()[-1] for line in run([prefix+'nm', '-u', obj]).splitlines()}
    assert imports <= {'memcpy', 'memset', 'strcmp', 'strlen', 'memcmp',
                       'pet_program_image_decode', 'pet_program_persistence_load_with_lease',
                       'pet_resources_device_current', 'pet_program_host_start_image_with_contract',
                       'pet_program_host_start_image_with_projection'}
    rows = obj.with_suffix('.su').read_text().splitlines()
    assert any('pet_program_activation_publish_with_projection' in row for row in rows)
    for row in rows:
        _, amount, kind = row.split('\t')
        assert kind == 'static' and int(amount) <= 256
    source = (ROOT/'main/pet_p4_program_activation.c').read_text()
    publish = source[source.index('static pet_program_runtime_result_t publish('):]
    for forbidden in ('persistence_load', 'fopen(', 'malloc(', 'vTaskDelay(', 'pet_app_store_'):
        assert forbidden not in publish
