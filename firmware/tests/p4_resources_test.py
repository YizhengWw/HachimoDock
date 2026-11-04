"""Compile production resource core/adapter; no device, audio, files or network."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_resource_core_quotas_and_cancelled_physical_leases(tmp_path):
    executable = tmp_path / 'resources'
    subprocess.run([
        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
        '-fsanitize=address,undefined', '-I', str(ROOT / 'main'),
        str(ROOT / 'main/pet_p4_resources.c'), str(ROOT / 'tests/p4_resources_test.c'),
        '-o', str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)


def test_device_adapter_parallel_admission_and_audio_protection(tmp_path):
    (tmp_path / 'freertos').mkdir()
    (tmp_path / 'freertos/FreeRTOS.h').write_text(
        '#pragma once\n#include <pthread.h>\n'
        'typedef pthread_mutex_t portMUX_TYPE;\n'
        '#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER\n'
        '#define portENTER_CRITICAL(m) ((void)pthread_mutex_lock(m))\n'
        '#define portEXIT_CRITICAL(m) ((void)pthread_mutex_unlock(m))\n')
    executable = tmp_path / 'resources_device'
    subprocess.run([
        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-pthread',
        '-fsanitize=address,undefined', '-I', str(tmp_path), '-I', str(ROOT / 'main'),
        str(ROOT / 'main/pet_p4_resources.c'), str(ROOT / 'main/pet_p4_resources_device.c'),
        str(ROOT / 'tests/p4_resources_device_test.c'), '-o', str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)


def test_resource_core_has_no_platform_locks_waits_heap_or_io():
    code = (ROOT / 'main/pet_p4_resources.c').read_text()
    for forbidden in ('malloc(', 'calloc(', 'realloc(', 'free(', 'Semaphore',
                      'portENTER', 'vTaskDelay', 'fopen(', 'printf(', 'esp_'):
        assert forbidden not in code
