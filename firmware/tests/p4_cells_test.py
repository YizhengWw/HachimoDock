"""p4-cells-v1: the pure grid core via ctypes against the shared contract, plus firmware/report parity.

Offline evidence only: the C core, the parser/renderer source shape and the hello capability string.
It does not prove device frame time or install behaviour.
"""
import ctypes as C
import json
import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SUITE = json.loads((ROOT / 'pc/skills/petui/contracts/cells-v1.json').read_text())
FLAGS = ('-DPET_P4_CELLS_V1=1', '-DPET_P4_TEXT_TABLE=1', '-DPET_P4_APP_DIRTY_CACHE=1', '-DPET_P4_QUOTA_V3=1')


class Cells(C.Structure):
    _fields_ = [('enabled', C.c_bool), ('cols', C.c_uint8), ('rows', C.c_uint8), ('bits', C.c_uint8), ('values', C.c_uint8 * 256)]


@pytest.fixture(scope='module')
def core(tmp_path_factory):
    out = tmp_path_factory.mktemp('cells') / 'cells.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'firmware/main'),
                    str(ROOT / 'firmware/main/pet_p4_cells.c'), '-o', str(out)], check=True)
    lib = C.CDLL(str(out))
    P = C.POINTER(Cells)
    lib.pet_cells_init.argtypes = [P, C.c_uint, C.c_uint, C.c_uint]; lib.pet_cells_init.restype = C.c_bool
    lib.pet_cells_init_row.argtypes = [P, C.c_uint, C.c_char_p]; lib.pet_cells_init_row.restype = C.c_bool
    lib.pet_cells_max_value.argtypes = [P]; lib.pet_cells_max_value.restype = C.c_int32
    lib.pet_cells_clamp.argtypes = [P, C.c_int32]; lib.pet_cells_clamp.restype = C.c_int32
    lib.pet_cells_in_range.argtypes = [P, C.c_int32, C.c_int32]; lib.pet_cells_in_range.restype = C.c_bool
    lib.pet_cells_get.argtypes = [P, C.c_int32, C.c_int32]; lib.pet_cells_get.restype = C.c_int32
    lib.pet_cells_set.argtypes = [P, C.c_int32, C.c_int32, C.c_int32]; lib.pet_cells_set.restype = C.c_bool
    lib.pet_cells_fill.argtypes = [P, C.c_int32]; lib.pet_cells_fill.restype = C.c_bool
    lib.pet_cells_count.argtypes = [P, C.c_int32]; lib.pet_cells_count.restype = C.c_int32
    lib.pet_cells_run.argtypes = [P, C.c_int32, C.c_int32, C.c_int32]; lib.pet_cells_run.restype = C.c_int32
    lib.pet_cells_export.argtypes = [P, C.POINTER(C.c_int32), C.c_uint]; lib.pet_cells_export.restype = C.c_uint
    lib.pet_cells_import.argtypes = [P, C.POINTER(C.c_int32), C.c_uint, C.POINTER(C.c_bool)]; lib.pet_cells_import.restype = C.c_uint
    return lib


def grid(lib, spec):
    g = Cells()
    assert lib.pet_cells_init(g, spec['cols'], spec['rows'], spec['bits'])
    for y, row in enumerate(spec.get('init', [])):
        assert lib.pet_cells_init_row(g, y, row.encode()), row
    return g


def rows(g):
    return [''.join('%x' % g.values[y * g.cols + x] for x in range(g.cols)) for y in range(g.rows)]


def test_contract_core_vectors_match_the_c_core(core):
    assert SUITE['profile'] == 'p4-cells-v1' and SUITE['limits'] == {'maxSide': 16, 'maxCells': 256, 'bits': [2, 4], 'maxOpsPerBranch': 4, 'outOfRangeRead': -1}
    for item in SUITE['core']:
        g = grid(core, item['grid'])
        if 'fill' in item:
            core.pet_cells_fill(g, item['fill'])
        assert core.pet_cells_run(g, item['x'], item['y'], item['value']) == item['run'], item
        assert core.pet_cells_count(g, item['value']) == item['count'], item


def test_shape_bounds_and_init_rows(core):
    g = Cells()
    for cols, rows_, bits in ((0, 1, 2), (17, 1, 2), (1, 17, 2), (16, 16, 3), (16, 16, 8), (16, 16, 1)):
        assert not core.pet_cells_init(g, cols, rows_, bits) and not g.enabled
    assert core.pet_cells_init(g, 16, 16, 4) and g.enabled and core.pet_cells_max_value(g) == 15
    assert core.pet_cells_init(g, 3, 2, 2) and core.pet_cells_max_value(g) == 3
    assert core.pet_cells_init_row(g, 0, b'123') and core.pet_cells_init_row(g, 1, b'0A0') is False  # 'A' = 10 > 3
    assert not core.pet_cells_init_row(g, 1, b'12')    # too short
    assert not core.pet_cells_init_row(g, 1, b'1230')  # too long
    assert not core.pet_cells_init_row(g, 1, b'12g')   # not hex
    assert not core.pet_cells_init_row(g, 2, b'000')   # row out of range
    assert core.pet_cells_init_row(g, 1, b'3Ab'[:0] + b'301')
    assert rows(g) == ['123', '301']
    # init rows do not count as changes for the import helper's semantics; later zeroing init resets everything.
    assert core.pet_cells_init(g, 3, 2, 2) and rows(g) == ['000', '000']


def test_reads_writes_clamp_and_out_of_range_semantics(core):
    g = grid(core, {'cols': 4, 'rows': 2, 'bits': 2, 'init': ['0123', '3210']})
    assert core.pet_cells_get(g, 3, 0) == 3 and core.pet_cells_get(g, 0, 1) == 3
    for x, y in ((-1, 0), (4, 0), (0, -1), (0, 2), (99, 99)):
        assert core.pet_cells_get(g, x, y) == -1 and not core.pet_cells_in_range(g, x, y)
        assert not core.pet_cells_set(g, x, y, 1)
    assert rows(g) == ['0123', '3210']
    assert core.pet_cells_set(g, 0, 0, 9) and core.pet_cells_get(g, 0, 0) == 3  # clamped to 2^bits-1
    assert core.pet_cells_set(g, 0, 0, -5) and core.pet_cells_get(g, 0, 0) == 0
    assert not core.pet_cells_set(g, 0, 0, 0)  # unchanged value reports no change
    assert core.pet_cells_clamp(g, 100) == 3 and core.pet_cells_clamp(g, -1) == 0 and core.pet_cells_clamp(g, 2) == 2
    assert core.pet_cells_fill(g, 2) and rows(g) == ['2222', '2222'] and not core.pet_cells_fill(g, 2)
    assert core.pet_cells_count(g, 2) == 8 and core.pet_cells_count(g, 9) == 0 and core.pet_cells_count(g, -1) == 0
    assert core.pet_cells_run(g, 0, 0, 2) == 4 and core.pet_cells_run(g, 0, 0, 1) == 0 and core.pet_cells_run(g, 7, 7, 2) == 0


def test_vm_mirror_export_import(core):
    g = grid(core, {'cols': 2, 'rows': 2, 'bits': 2, 'init': ['12', '30']})
    out = (C.c_int32 * 8)()
    assert core.pet_cells_export(g, out, 8) == 4 and list(out)[:4] == [1, 2, 3, 0]
    assert core.pet_cells_export(g, out, 2) == 2
    changed = C.c_bool(False)
    values = (C.c_int32 * 4)(0, 7, -1, 2)
    assert core.pet_cells_import(g, values, 4, C.byref(changed)) == 4 and changed.value and rows(g) == ['03', '02']
    assert core.pet_cells_import(g, values, 4, C.byref(changed)) == 4 and not changed.value
    disabled = Cells()
    assert core.pet_cells_export(disabled, out, 8) == 0 and core.pet_cells_import(disabled, values, 4, None) == 0


def test_core_has_no_rtos_heap_or_io_and_builds_for_wasm(tmp_path):
    source = (ROOT / 'firmware/main/pet_p4_cells.c').read_text() + (ROOT / 'firmware/main/pet_p4_cells.h').read_text()
    for forbidden in ('#include <stdio', 'malloc', 'freertos', 'esp_', 'cJSON', 'printf'):
        assert forbidden not in source, forbidden
    out = tmp_path / 'cells.wasm'
    subprocess.run(['clang', '--target=wasm32', '-O2', '-ffreestanding', '-fno-builtin', '-nostdlib', '-Wl,--no-entry', '-Wl,--export-all',
                    '-I', str(ROOT / 'firmware/main'), str(ROOT / 'firmware/main/pet_p4_cells.c'), '-o', str(out)], check=True)
    assert out.stat().st_size > 0


def test_firmware_parser_renderer_and_capability_are_flag_gated():
    runtime = (ROOT / 'firmware/main/pet_p4_miniapp.c').read_text()
    surface = (ROOT / 'firmware/main/pet_p4_surface.c').read_text()
    renderer = (ROOT / 'firmware/main/pet_p4_renderer.c').read_text()
    protocol = (ROOT / 'firmware/main/pet_p4_protocol.c').read_text()
    features = (ROOT / 'firmware/main/pet_p4_component_features.h').read_text()
    assert '#ifndef PET_P4_CELLS_V1\n#define PET_P4_CELLS_V1 0\n#endif' in features
    # Capability string only behind the flag.
    block = protocol[protocol.index('#if PET_P4_CELLS_V1\n  cJSON_AddStringToObject(capabilities, "widgetCells"'):]
    assert block.split('#endif')[0].count('widgetCells') == 1 and 'PET_CELLS_ENGINE' in block.split('#endif')[0]
    assert (ROOT / 'firmware/main/pet_p4_cells.h').read_text().count('#define PET_CELLS_ENGINE "p4-cells-v1"') == 1
    # Parser: root key, exclusions, init rows, the four ops, cell conditions, program ownership.
    assert '"texts", "cells",' in runtime and 'strcmp(json_string(cells,"engine"),PET_CELLS_ENGINE)' in runtime
    for name in ('"cell", "cellfill", "cellcount", "cellrun"', 'parse_cell_ops(', 'apply_cell_ops(', 'parse_cell_arg(', 'the component program owns cell writes'):
        assert name in runtime, name
    assert runtime.count('apply_cell_ops(&g_runtime') == 3  # transition branch, tick body, tick completion
    assert 'pet_cells_export(&parsed->cells,parsed->program_seed.values,PET_PROGRAM_CELLS)' in runtime
    assert runtime.count('pet_cells_import(') == 2  # seed apply + committed collect
    # Grid state and the op arrays are inside the PSRAM runtime struct, never heap.
    assert '  pet_cells_t cells;\n  bool cells_program_owned;' in runtime and 'static EXT_RAM_BSS_ATTR miniapp_runtime_t g_runtime;' in runtime
    assert '#define MINIAPP_MAX_CELL_OPS 4' in runtime
    # Surface: node type 9 gated, atlas columns divide the image, exact cursor keys.
    assert '"button","cells"' in surface and 'PET_P4_CELLS_V1?10:9' in surface and '!(PET_P4_CELLS_V1&&type==PET_SURFACE_NODE_CELLS)' in surface
    assert '!number(o,"columns",2,16,&v)||out->image_w[n->image]%v' in surface and '{"x_var","y_var","color"}' in surface
    # Renderer: value 0 draws nothing, atlas frame = value, cursor outline, bounded by the grid.
    draw = renderer[renderer.index('static void draw_app_cells('):renderer.index('static bool render_package_app(')]
    assert 'if(!v)continue;' in draw and 'if(v>=n->tile_cols)continue;' in draw and 'tile.tile_index=(uint8_t)v' in draw
    assert draw.count('n->cursor_color)') == 4 and 'malloc' not in draw and 'heap_caps' not in draw
    assert 'else if(n->type==PET_SURFACE_NODE_CELLS)draw_app_cells(n,&g_miniapp_render_snapshot.cells);' in renderer


def test_runtime_struct_sizes_with_track_a_flags(tmp_path):
    """ptype-style growth estimate of the self-contained runtime structs, both flag states."""
    source = (ROOT / 'firmware/main/pet_p4_miniapp.c').read_text()
    start = source.index('typedef enum {\n  MINIAPP_VAR_INT,')
    end = source.index('typedef struct {\n  bool active;\n  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];\n  bool resource_incomplete;')
    defines = source[source.index('/* p4-quota-v2 ceilings'):source.index('#define MINIAPP_WIDGET_PATH')]
    body = defines + source[start:end]
    code = ('#include <stdio.h>\n#include <stdint.h>\n#include <stdbool.h>\n#include "pet_p4_component_features.h"\n#include "pet_p4_app_navigation.h"\n'
            '#include "pet_p4_game.h"\n#include "pet_p4_miniapp.h"\n#if PET_P4_CELLS_V1\n#include "pet_p4_cells.h"\n#endif\n' + body +
            'int main(void){printf("{\\"var\\":%zu,\\"effect\\":%zu,\\"condition\\":%zu,\\"transition\\":%zu,\\"tick\\":%zu,\\"rule\\":%zu,\\"node\\":%zu,\\"surface\\":%zu,'
            '\\"maxVars\\":%d,\\"maxTransitions\\":%d,\\"maxTicks\\":%d}\\n",sizeof(miniapp_var_t),sizeof(miniapp_effect_t),sizeof(miniapp_condition_t),sizeof(miniapp_transition_t),'
            'sizeof(miniapp_tick_t),sizeof(miniapp_rule_t),sizeof(pet_surface_node_t),sizeof(pet_surface_t),MINIAPP_MAX_VARS,MINIAPP_MAX_TRANSITIONS,MINIAPP_MAX_TICKS);return 0;}\n')
    src = tmp_path / 'sizes.c'; src.write_text(code)
    sizes = {}
    for label, flags in (('off', ()), ('on', FLAGS)):
        exe = tmp_path / ('sizes-' + label)
        result = subprocess.run(['cc', '-std=c11', '-w', *flags, '-I', str(ROOT / 'firmware/main'), '-I', str(ROOT / 'firmware/tests/vendor/cjson'),
                                 '-I', str(ROOT / 'firmware/tests/program_probe_stubs'), str(src), '-o', str(exe)], capture_output=True, text=True)
        if result.returncode:
            pytest.skip('host toolchain cannot compile the runtime struct extract: ' + result.stderr[:400])
        sizes[label] = json.loads(subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout)
    off, on = sizes['off'], sizes['on']
    assert on['maxVars'] == 32 and off['maxVars'] == 16 and on['maxTransitions'] == 48 and on['maxTicks'] == 16
    # Each cell op is a few bytes; four per branch keep the per-rule growth bounded.
    assert on['transition'] - off['transition'] <= 4 * 16 + 16 + 4 * 64  # cell ops + counts + four wider string effects
    assert on['tick'] - off['tick'] <= 8 * 16 + 8 + 3 * 4 * 64
    assert on['node'] - off['node'] <= 32 + 64 + 8
    total_off = off['var'] * off['maxVars'] + off['transition'] * off['maxTransitions'] + off['tick'] * off['maxTicks'] + off['surface']
    total_on = on['var'] * on['maxVars'] + on['transition'] * on['maxTransitions'] + on['tick'] * on['maxTicks'] + on['surface']
    # Documented in quota.md: the capability parent grows the PSRAM runtime by well under 128 KiB.
    assert 0 < total_on - total_off < 128 * 1024, (total_off, total_on)
