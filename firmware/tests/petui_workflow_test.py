"""Shared asynchronous declaration vectors. No provider/network/device execution."""
import copy
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/skills/petui/scripts'))
from app_composition import composition_errors
from app_services import application_errors


def test_v3_shared_contract_and_rejections():
    suite = json.loads((ROOT / 'pc/skills/petui/contracts/composition-v3.json').read_text())
    runtime, declaration = suite['runtime'], suite['declaration']
    assert not composition_errors(declaration, runtime)
    assert not application_errors(declaration, runtime)
    for case in suite['rejections']:
        bad = copy.deepcopy(declaration)
        parts = case['pointer'].lstrip('/').split('/')
        owner = bad
        for part in parts[:-1]:
            owner = owner[int(part)] if isinstance(owner, list) else owner[part]
        owner[parts[-1]] = case['value']
        assert composition_errors(bad, runtime), case
        assert application_errors(bad, runtime), case
    for key in ('host_context', 'input'):
        bad = copy.deepcopy(runtime)
        bad.pop(key)
        assert composition_errors(declaration, bad)


def test_v3_budget_unique_output_and_v1_v2_reject_new_fields():
    suite = json.loads((ROOT / 'pc/skills/petui/contracts/composition-v3.json').read_text())
    runtime, declaration = suite['runtime'], suite['declaration']
    for engine in ('pc-app-composition-v1', 'pc-app-composition-v2'):
        assert composition_errors({**declaration, 'engine': engine}, runtime)
    for change in ('duplicate_result', 'too_many_steps', 'unknown_step'):
        bad = copy.deepcopy(declaration)
        steps = bad['content'][0]['asyncWorkflows']['fetch']
        if change == 'duplicate_result':
            steps.append(copy.deepcopy(steps[1]))
        elif change == 'too_many_steps':
            steps.extend([steps[0]] * 5)
        else:
            steps[0]['script'] = 'not-executable'
        assert composition_errors(bad, runtime), change


def test_v3_package_capability_gate_and_private_journal_exclusion(tmp_path):
    from validate_generated_widget import validate_widget, load_capabilities
    suite = json.loads((ROOT / 'pc/skills/petui/contracts/composition-v3.json').read_text())
    runtime = suite['runtime']
    runtime['dashboard'].update(visualStyle='clean', visualPalette='ocean', visualLayout='tool', visualSprite='gauge')
    controls = [('host.start', 'button.sw1.short_press'), ('host.cancel', 'button.sw2.short_press'),
                ('host.resume', 'joystick.up'), ('host.increment', 'joystick.down')]
    files = {'component.json': dict(id='workflow-gate', name='Workflow gate', version='1.0.0', kind='tool', description='Public GET with durable local checkpoints.'),
             'negative-screen.json': {'dashboard': runtime['dashboard']}, 'runtime/widget.json': runtime,
             'pc/application.json': suite['declaration'], 'share.json': {'title': 'Workflow gate'},
             'buttons.json': [dict(action=action, control='test input', event=event, label=action[5:]) for action, event in controls]}
    (tmp_path / 'assets').mkdir()
    for relative, value in files.items():
        path = tmp_path / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, ensure_ascii=False))
    assert not validate_widget(tmp_path)
    caps = load_capabilities(None)
    caps.pop('widgetHostContext')
    assert any('上下文' in error for error in validate_widget(tmp_path, caps))
    (tmp_path / 'composition-v3-state.json').write_text('{"synthetic":"private journal"}')
    assert any('PC 应用包含额外文件' in error for error in validate_widget(tmp_path))


def test_local_only_integration_fixture_is_valid_and_declares_no_network(tmp_path):
    from validate_generated_widget import validate_widget
    suite = json.loads((ROOT / 'pc/skills/petui/contracts/workflow-local-v3.json').read_text())
    runtime, declaration = suite['runtime'], suite['declaration']
    assert not application_errors(declaration, runtime)
    assert all(source['kind'] != 'json-get' for source in declaration['content'][0]['sources'].values())
    assert all('get' not in step for steps in declaration['content'][0]['asyncWorkflows'].values() for step in steps)
    (tmp_path / 'assets').mkdir()
    files = {'component.json': suite['manifest'], 'runtime/widget.json': runtime,
             'negative-screen.json': {'dashboard': runtime['dashboard']}, 'pc/application.json': declaration,
             'buttons.json': suite['buttons'], 'share.json': suite['share']}
    for relative, value in files.items():
        path = tmp_path / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, ensure_ascii=False))
    assert not validate_widget(tmp_path)
