"""Track A authoring entry points: shared positive/negative vectors, no device IO."""
import copy
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / 'pc/skills/petui/scripts'
sys.path.insert(0, str(SCRIPTS))
import validate_generated_widget as validator
import texts


def suite(name):
    return json.loads((SCRIPTS.parent / 'contracts' / f'{name}-v1.json').read_text())


def validate(runtime):
    errors = []
    caps = dict(validator.DEFAULT_CAPABILITIES)
    caps.update(widgetCells='p4-cells-v1', widgetTexts='p4-texts-v1')
    validator.validate_runtime(runtime, 'tool', ROOT, caps, errors)
    return errors


@pytest.mark.parametrize('name', ['cells', 'texts'])
def test_shared_vectors_through_public_validator(name):
    vectors = suite(name)
    assert not validate(vectors['runtime'])
    for patch in vectors['reject']:
        runtime = copy.deepcopy(vectors['runtime'])
        parts = patch['path'].strip('/').split('/')
        target = runtime
        for part in parts[:-1]:
            target = target[int(part)] if isinstance(target, list) else target[part]
        key = int(parts[-1]) if isinstance(target, list) else parts[-1]
        target[key] = patch['value']
        assert validate(runtime), patch['name']


def test_text_scalar_and_index_validation_is_total():
    assert not texts.valid_entry('\ud800')
    assert texts.schema_errors({'engine': 'p4-bounded-runtime-v4', 'texts': ['\udfff']})
    for index in [True, 0.5, '0', None, float('inf')]:
        assert texts.text_at(['first'], index) == ''
    assert texts.text_at(['first'], 0) == 'first'


def test_cells_ops_need_capability_and_declaration():
    runtime = suite('cells')['runtime']
    errors = []
    validator.validate_cells_profile(runtime, {}, errors)
    assert any('widgetCells' in e for e in errors)
    runtime.pop('cells')
    errors = []
    validator.validate_cells_profile(runtime, {}, errors)
    assert errors


def test_program_v2_descriptor_vectors():
    import component_program
    vectors = json.loads(
        (SCRIPTS.parent / 'contracts/component-program-v2.json').read_text())
    assert not component_program.errors(vectors['runtime'])
    for patch in vectors['reject']:
        if patch.get('scope') == 'package':
            continue
        runtime = copy.deepcopy(vectors['runtime'])
        parts = patch['path'].strip('/').split('/')
        target = runtime
        for part in parts[:-1]:
            target = target[int(part)] if isinstance(target, list) else target[part]
        target[int(parts[-1]) if isinstance(target, list) else parts[-1]] = patch['value']
        assert component_program.errors(runtime), patch['name']
