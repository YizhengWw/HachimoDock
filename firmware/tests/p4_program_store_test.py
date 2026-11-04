"""Portable private program saves only; no target build, FS adapter or capability enable."""
from pathlib import Path
import json
import os
import struct
import zlib

import pytest

from p4_program_test import WARNINGS, INCLUDES, run

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / 'main' / name for name in (
    'pet_p4_program_core.c', 'pet_p4_program_image.c',
    'pet_p4_program_runtime.c', 'pet_p4_program_store.c')]
CASES = ['roundtrip_extremes', 'corrupt_exact_wire', 'invalid_snapshot',
         'arguments_alias_and_span', 'strict_text_envelope', 'real_runtime_seed',
         'mutated_fuzz', 'terminal_revision_seed_without_wrap_or_replay']


@pytest.fixture(scope='module')
def store_binary(tmp_path_factory):
    executable = tmp_path_factory.mktemp('program-store') / 'store-tests'
    run([os.environ.get('CC', 'cc'), *WARNINGS, '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', *INCLUDES, *SOURCES,
         ROOT / 'tests/p4_program_store_test.c', '-o', executable])
    return executable


@pytest.mark.parametrize('case', range(len(CASES)), ids=CASES)
def test_program_store_native(store_binary, case):
    result = json.loads(run([store_binary, case]))
    assert result['ok'] and result['wireBytes'] == 320 and result['textCapacity'] == 652


def test_program_store_golden_wire_independent_little_endian_crc(store_binary):
    expected = bytearray(320)
    struct.pack_into('<4sBBHIII', expected, 0, b'P4S1', 1, 0, 32, 320, 0x12345678, 9)
    expected[20:23] = bytes([3, 8, 4])
    struct.pack_into('<3i', expected, 32, -(2**31), 2**31 - 1, -1)
    struct.pack_into('<8i', expected, 288, *range(-4, 4))
    struct.pack_into('<I', expected, 28, zlib.crc32(expected[:28] + expected[32:]))
    assert bytes.fromhex(run([store_binary, 'dump'])) == expected


def test_program_store_adapter_boundary_does_not_enable_effects_or_storage():
    source = (ROOT / 'main/pet_p4_program_store.c').read_text()
    for forbidden in ('malloc(', 'calloc(', 'fopen(', 'pet_app_store_update(',
                      'pet_app_store_forget(', 'esp_', 'vTaskDelay', 'pet_program_runtime_start('):
        assert forbidden not in source
    assert '[PET_RESOURCE_PROGRAM]={0,0}' in (ROOT / 'main/pet_p4_resources.c').read_text()
    assert '#define PET_APP_SAVE_BYTES 1024' in (ROOT / 'main/pet_p4_app_store.h').read_text()
