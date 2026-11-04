"""Shared PC composition contract; simulation does not prove USB or audio behavior."""
import copy
import json
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/skills/petui/scripts'))
from app_services import application_errors

def test_shared_composition_contract():
    suite = json.loads((ROOT / 'pc/skills/petui/contracts/composition-v1.json').read_text())
    runtime, declaration = suite['runtime'], suite['declaration']
    assert application_errors(declaration, runtime) == []
    for case in suite['rejections']:
        bad = copy.deepcopy(declaration)
        parts = case['pointer'].lstrip('/').split('/')
        owner = bad
        for part in parts[:-1]: owner = owner[int(part)] if isinstance(owner, list) else owner[part]
        owner[parts[-1]] = case['value']
        assert application_errors(bad, runtime), case
    for change in ({'tick':[{}]}, {'persist':{}}, {'transitions':[runtime['transitions'][0]] * 2}):
        assert application_errors(declaration, {**runtime, **change})

def test_collection_shared_acceptance_rejections_and_no_transient_dependencies():
    suite=json.loads((ROOT/'pc/skills/petui/contracts/collection-v1.json').read_text())
    runtime,declaration=suite['runtime'],suite['declaration']
    assert not application_errors(declaration,runtime)
    for case in suite['rejections']:
        bad=copy.deepcopy(declaration);parts=case['pointer'].lstrip('/').split('/');owner=bad
        for part in parts[:-1]:owner=owner[int(part)] if isinstance(owner,list) else owner[part]
        owner[parts[-1]]=case['value']
        assert application_errors(bad,runtime),case
    bad=copy.deepcopy(declaration)
    bad['content'][0]['sources']['records']={'kind':'computed','expression':{'ref':'/event/rows'}}
    assert application_errors(bad,runtime)

def test_shared_ledger_contract():
    suite=json.loads((ROOT/'pc/skills/petui/contracts/ledger-v1.json').read_text())
    runtime,declaration=suite['runtime'],suite['declaration'];assert not application_errors(declaration,runtime)
    for case in suite['rejections']:
        bad=copy.deepcopy(declaration);bad['content'][0]['sources']['records'][case['field']]=case['value'];assert application_errors(bad,runtime)
    bad=copy.deepcopy(runtime);bad['input'].pop('confirm');assert application_errors(declaration,bad)

def test_expression_vectors_and_resource_rejection():
    from app_expr import evaluate,validate
    cases=json.loads((ROOT/'pc/skills/petui/contracts/expressions-v1.json').read_text())
    for case in cases:
        try:got=evaluate(case['expression'],case['context'],set(case['sources']))
        except ValueError:assert case.get('reject'),case['name']
        else:assert not case.get('reject') and got==case['expected'],case['name']
    import pytest
    with pytest.raises(ValueError):evaluate({'literal':[0]*129},{},set())
    expression=True
    for _ in range(14):expression={'op':'not','args':[expression]}
    with pytest.raises(ValueError):validate(expression,set())
    expression={'op':'map','args':[{'literal':[0]*128}],'value':{'op':'map','args':[{'literal':[0]*128}],'value':{'ref':'/row'}}}
    with pytest.raises(ValueError):evaluate(expression,{},set())

def test_composition_v2_shared_acceptance_and_rejections():
    suite=json.loads((ROOT/'pc/skills/petui/contracts/composition-v2.json').read_text())
    runtime,declaration=suite['runtime'],suite['declaration']
    assert not application_errors(declaration,runtime)
    for case in suite['rejections']:
        bad=copy.deepcopy(declaration);parts=case['pointer'].lstrip('/').split('/');owner=bad
        for part in parts[:-1]:owner=owner[int(part)] if isinstance(owner,list) else owner[part]
        owner[parts[-1]]=case['value']
        assert application_errors(bad,runtime),case
    for changes in ({'host_context':'unknown'},{'host_context':None},{'tick':[{}]},{'persist':{}}):
        assert application_errors(declaration,{**runtime,**changes})
    bad=copy.deepcopy(runtime);bad['input'].pop('confirm');assert application_errors(declaration,bad)

def test_v2_private_store_is_not_a_distributable_component_file(tmp_path):
    import subprocess
    (tmp_path/'assets').mkdir()
    suite=json.loads((ROOT/'pc/skills/petui/contracts/composition-v2.json').read_text());runtime=suite['runtime']
    runtime['dashboard'].update(visualStyle='clean',visualPalette='ocean',visualLayout='tool',visualSprite='gauge')
    files={'component.json':dict(id='private-store-gate',name='Store gate',version='1.0.0',kind='tool',description='Explicit test records and confirmed removal.'),'negative-screen.json':{'dashboard':runtime['dashboard']},'runtime/widget.json':runtime,'pc/application.json':suite['declaration'],'buttons.json':[dict(action='host.save',control='SW1',event='button.sw1.short_press',label='Save'),dict(action='host.remove',control='SW2',event='button.sw2.short_press',label='Remove')],'share.json':{'title':'Store gate'}}
    for name,value in files.items():
        file=tmp_path/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_text(json.dumps(value,ensure_ascii=False))
    command=[sys.executable,str(ROOT/'pc/skills/petui/scripts/validate_generated_widget.py'),str(tmp_path)]
    good=subprocess.run(command,capture_output=True,text=True);assert good.returncode==0,good.stdout+good.stderr
    (tmp_path/'composition-v2-state.json').write_text('{"synthetic":"not user data"}')
    bad=subprocess.run(command,capture_output=True,text=True);assert bad.returncode!=0 and 'PC 应用包含额外文件' in bad.stdout+bad.stderr
