"""Actual row kernel, alignment fallback and bounded private word IO."""
from pathlib import Path
import configparser
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_actual_pack_rows_exact_planes_alignments_padding_splits_and_tails(tmp_path):
    from p4_render_trace_test import function
    actual=function((ROOT/'main/pet_p4_renderer.c').read_text(),'pack_h264_i420_rows')
    harness=(ROOT/'tests/p4_h264_pack_word_test.c').read_text()
    assert harness.count('/* ACTUAL_ROW_KERNEL */')==1
    path=tmp_path/'pack.c';path.write_text(harness.replace('/* ACTUAL_ROW_KERNEL */',actual))
    for enabled in (0,1):
        binary=tmp_path/f'pack-{enabled}'
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            f'-DPET_P4_H264_PACK_WORD_IO={enabled}','-I',str(ROOT/'main'),str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=25)

def test_pack_word_stays_private_and_keeps_original_worker_and_fence():
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_render_pack_probe':
            assert config[section]['extends']=='env:esp32_p4_render_queue_probe'
            assert '-DPET_P4_H264_PACK_WORD_IO=1' in flags
            assert 'ROTATE_TILE' not in flags
        else:assert 'PET_P4_H264_PACK_WORD_IO=1' not in flags
    helper=(ROOT/'main/pet_p4_h264_pack_word.h').read_text()
    assert '#define PET_P4_H264_PACK_WORD_IO 0' in helper
    assert helper.index('&3U))return false')<helper.index('__builtin_assume_aligned')
    for forbidden in ('malloc(', 'uint32_t *', 'may_alias', 'vTaskDelay(', 'portENTER_', 'esp_cache_'):assert forbidden not in helper
    from p4_render_trace_test import function
    source=(ROOT/'main/pet_p4_renderer.c').read_text()
    job=function(source,'pack_h264_i420_for_ppa')
    assert job.index('xTaskNotifyGive(')<job.index('xSemaphoreTake(g_h264_pack_done, portMAX_DELAY)')<job.index('*packed =')
    rows=function(source,'pack_h264_i420_rows')
    assert 'memcpy(&y_lo, source_y + x, sizeof(y_lo));' in rows
    assert 'for (; x < src_w; x += 2)' in rows
