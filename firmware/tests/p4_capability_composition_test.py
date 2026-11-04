"""Cross-profile regressions: the real authoring validator must compose negotiated features."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / 'pc/skills/petui/scripts'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('capability_composition_validator', SCRIPTS / 'validate_generated_widget.py')
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


def _declaration():
    runtime = json.loads((ROOT / 'pc/skills/petui/contracts/cells-v1.json').read_text())['runtime']
    runtime['quota'] = 'p4-quota-v3'
    runtime['texts'] = ['Ready', 'Grid changed']
    runtime['vars']['line'] = {'type': 'int', 'init': 0}
    runtime['vars']['memo'] = {'type': 'string', 'init': 'a' * 127}
    while len(runtime['vars']) < 32:
        runtime['vars'][f'extra{len(runtime["vars"])}'] = {'type': 'int', 'init': 0}
    runtime['dashboard']['headline'] = {'text_at': 'line'}
    runtime['surface']['elements'].append({'type': 'text', 'x': 20, 'y': 70, 'w': 600, 'h': 40,
                                           'color': 65535, 'text_at': 'line'})
    caps = deepcopy(validator.DEFAULT_CAPABILITIES)
    caps.update(widgetCells='p4-cells-v1', widgetTexts='p4-texts-v1', widgetQuota='p4-quota-v3',
                widgetQuotas=['p4-quota-v2', 'p4-quota-v3'])
    return runtime, caps


def _errors(runtime, caps, tmp_path):
    errors = []
    validator.validate_runtime(runtime, 'tool', tmp_path, caps, errors)
    return errors


def test_cells_texts_and_v3_capacity_share_the_real_validation_path(tmp_path):
    runtime, caps = _declaration()
    assert not _errors(runtime, caps, tmp_path)
    for key in ('widgetCells', 'widgetTexts'):
        missing = deepcopy(caps)
        missing.pop(key)
        assert _errors(runtime, missing, tmp_path), key
    over = deepcopy(runtime)
    over['vars']['overflow'] = {'type': 'int', 'init': 0}
    assert _errors(over, caps, tmp_path)
    over = deepcopy(runtime)
    over['vars']['memo']['init'] += 'b'
    assert _errors(over, caps, tmp_path)
    downgraded = deepcopy(runtime)
    downgraded['quota'] = 'p4-quota-v2'
    assert _errors(downgraded, caps, tmp_path)
