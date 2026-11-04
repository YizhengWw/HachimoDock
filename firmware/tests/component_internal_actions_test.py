"""Scoped internal triggers through real package validation and production C parser.
No fake physical bindings, device connection, Cargo or firmware build.
"""
import copy
import json
from pathlib import Path
import sys

import pytest
from petui_capability_ext_test import harness

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from capability_ext import internal_actions
from validate_generated_widget import validate_widget,DEFAULT_CAPABILITIES

SUITE=json.loads((ROOT/'pc/skills/petui/contracts/internal-actions-v1.json').read_text())
CAPS={**DEFAULT_CAPABILITIES,'widgetSequence':'p4-sequence-v1','widgetSamples':'p4-samples-v1','widgetClip':'p4-clip-v1','widgetBigImage':'p4-big-image-v1','widgetEvents':'p4-events-v1'}


def package(path,runtime,buttons):
    (path/'runtime').mkdir(parents=True);(path/'assets').mkdir()
    files={'component.json':{'id':'private-internal-scope','name':'Scope','version':'0.0.1','kind':'tool','description':'Private internal-action package regression.'},
           'negative-screen.json':{'dashboard':{'title':runtime['dashboard']['title'],'visualStyle':'clean','visualPalette':'mono','visualLayout':'tool','visualSprite':'timer'}},
           'buttons.json':buttons,'runtime/widget.json':runtime,'share.json':{'title':'Scope'}}
    for filename,value in files.items():(path/filename).write_text(json.dumps(value,ensure_ascii=False,separators=(',',':')))
    (path/'assets/.keep').write_bytes(b'')


@pytest.mark.parametrize('item',SUITE['vectors'],ids=lambda v:v['name'])
def test_real_package_internal_scope_matches_production_c(harness,tmp_path,item):
    runtime=item['runtime'];buttons=item['buttons']
    package(tmp_path,runtime,buttons)
    errors=validate_widget(tmp_path,CAPS)
    assert (not errors)==item['ok'],errors
    assert internal_actions(runtime)==set(item['internal'])
    result=harness({'runtime':runtime,'buttons':buttons,'queries':SUITE['queries']})
    assert result['ok']==item['ok'],result
    expected=sum(1<<i for i,name in enumerate(SUITE['queries']) if name in item['internal'])
    assert result['internal_mask']==expected,result


def test_private_candidate_packages_all_pass_without_fabricated_bindings():
    root=ROOT/'firmware/tests/fixtures/track_c_private'
    manifest=json.loads((root/'manifest.json').read_text())
    for item in manifest['packages']:
        assert not validate_widget(root/item['path'],CAPS),item['id']
    receiver=json.loads((root/'private-cext-event-recv/buttons.json').read_text())
    alarm=json.loads((root/'private-cext-alarm/buttons.json').read_text())
    assert not any(b['action'].startswith('event:') for b in receiver)
    assert not any(b['action']=='ring' for b in alarm)
