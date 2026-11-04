"""Pure static-path guard, including actual cross-module form/capture paths.

Synthetic compiler reports only. Does not run PIO or equate static frame sums
with physical stack high-water, latency, interrupts or voice/AEC acceptance.
"""
from pathlib import Path
import importlib.util
import re
import json
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('renderer_stack_budget', ROOT / 'tools/renderer_stack_budget.py')
STACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STACK)


def fixture():
    names = {m: {} for m in STACK.MODULES}
    for path in STACK.PATHS.values():
        for module, function in path:
            names[module][function] = 32
    return {m: '\n'.join(f'main/{m}.c:1:1:{name}\t{size}\tstatic' for name, size in functions.items())
            for m, functions in names.items()}


def test_cross_module_paths_are_bounded_and_bound_to_an_exact_elf():
    result = STACK.check_frames(fixture(), 'a' * 64)
    assert result['elfSha256'] == 'a' * 64 and result['budgetBytes'] == 4096 and not result['allCallEdges']
    assert len(result['paths']) == 17
    assert result['schema'] == 4 and result['engine'] == 'p4-render-stack-v4'
    paths = {p['id']: p for p in result['paths']}
    assert any(f['module'] == 'pet_p4_miniapp' for f in paths['snapshot_form_text']['frames'])
    assert any(f['module'] == 'pet_p4_form_v2' for f in paths['snapshot_form_text']['frames'])
    assert any(f['module'] == 'pet_p4_surface' for f in paths['snapshot_host_collection']['frames'])
    assert any(f['module'] == 'pet_p4_host_collection' for f in paths['snapshot_host_collection']['frames'])
    assert all(p['bytes'] == sum(f['bytes'] for f in p['frames']) <= 4096 for p in result['paths'])
    reports = fixture()
    reports[STACK.R] = reports[STACK.R].replace('draw_app_label\t32', 'draw_app_label.constprop.0\t1536')
    result = STACK.check_frames(reports, 'b' * 64)
    assert result['maxReviewedBytes'] > 1536
    # Only actual emitted frames are summed; an optional inlined helper is
    # accounted for by its compiler-emitted caller, not counted twice.
    reports[STACK.R] = '\n'.join(row for row in reports[STACK.R].splitlines() if 'app_data_label\t' not in row)
    path = next(p for p in STACK.check_frames(reports, 'b' * 64)['paths'] if p['id'] == 'data_label_width')
    assert any(f['function'] == 'app_data_label' and not f['emitted'] and f['bytes'] == 0 for f in path['frames'])


def test_actual_python_report_is_accepted_by_the_desktop_evidence_consumer():
    report = STACK.check_frames(fixture(), 'a' * 64)
    module = (ROOT.parent / 'pc/scripts/framework-stack-evidence.mjs').as_uri()
    code = (f'import {{rendererStackArtifact}} from {json.dumps(module)};'
            'let input="";for await(const c of process.stdin)input+=c;'
            'const report=JSON.parse(input);'
            'console.log(JSON.stringify(rendererStackArtifact(report,report.elfSha256)));')
    result = subprocess.run(['node', '--input-type=module', '-e', code],
                            input=json.dumps(report), text=True, capture_output=True, check=True)
    assert json.loads(result.stdout) == report


@pytest.mark.parametrize('mutation', ['missing_module','missing_entry','oversized','dynamic','hidden_dynamic_clone','new_visibility_oversized','malformed','bad_hash'])
def test_missing_dynamic_or_oversized_frames_cannot_pass(mutation):
    reports = fixture()
    sha = 'a' * 64
    if mutation == 'missing_module':
        del reports[STACK.M]
    elif mutation == 'missing_entry':
        reports[STACK.M] = reports[STACK.M].replace('pet_p4_miniapp_render_snapshot', 'renamed_entry')
    elif mutation == 'oversized':
        reports[STACK.R] = reports[STACK.R].replace('draw_app_label\t32', 'draw_app_label\t4096')
    elif mutation == 'dynamic':
        reports[STACK.F] = reports[STACK.F].replace('pet_form2_text\t32\tstatic', 'pet_form2_text\t32\tdynamic,bounded')
    elif mutation == 'hidden_dynamic_clone':
        reports[STACK.F] += '\nmain/core.c:2:1:pet_form2_text.constprop.1\t16\tdynamic'
    elif mutation == 'new_visibility_oversized':
        reports[STACK.R] = reports[STACK.R].replace('pet_app_frame_eligible_at\t32', 'pet_app_frame_eligible_at\t4096')
    elif mutation == 'malformed':
        reports[STACK.R] += '\ninvalid'
    else:
        sha = 'x' * 64
    with pytest.raises(ValueError):
        STACK.check_frames(reports, sha)


def test_image_hook_checks_stack_before_creating_an_image_and_does_not_raise_task_limits():
    source = (ROOT / 'scripts/platformio_chip_images.py').read_text()
    assert source.index('stack.check_build(') < source.index('result = env.Execute(command)')
    assert '.stack-budget.json' in source
    assert 'CONFIG_ESP_MAIN_TASK_STACK_SIZE=10240' in (ROOT / 'sdkconfig.defaults').read_text()
    assert all('pet_p4_renderer_render' in dict((n, 1) for _, n in path) for path in STACK.PATHS.values())


def test_reviewed_helpers_still_exist_so_renames_cannot_silently_look_like_inlining():
    for module, function in set(entry for path in STACK.PATHS.values() for entry in path):
        source = (ROOT / 'main' / (module + '.c')).read_text()
        if module==STACK.R:
            for include in ['pet_p4_h264_prefetch.inc','pet_p4_app_cache.h']:
                source+='\n'+(ROOT/'main'/include).read_text()
        assert re.search(r'^[^\n;{}]*\b' + re.escape(function) + r'\s*\([^;]*\)\s*\{', source, re.M), (module, function)
