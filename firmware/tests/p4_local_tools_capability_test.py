"""Offline target capability admission; no SDK, serial, or Cargo."""
import ast
import importlib.util
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
SCRIPTS=ROOT/'pc/skills/petui/scripts'
spec=importlib.util.spec_from_file_location('local_tools_capability_under_test',SCRIPTS/'local_tools.py')
local=importlib.util.module_from_spec(spec)
spec.loader.exec_module(local)

def test_shared_local_masks_capability_vectors():
    vectors=json.loads((ROOT/'firmware/tests/fixtures/local_tools_masks_capabilities.json').read_text())['vectors']
    for index,case in enumerate(vectors):
        runtime={} if case['kind'] is None else {'local':{'kind':case['kind'],'groups':[{'items':[{}]*n} for n in case['counts']]}}
        errors=local.local_capability_errors(runtime,case['capabilities'])
        assert (not errors)==case['allowed'],(index,errors)
        if errors and case['counts'] and max(case['counts'])>16:
            assert 'p4-local-masks-v2' in errors[0]

def test_python_actual_target_validator_calls_capability_admission():
    tree=ast.parse((SCRIPTS/'validate_generated_widget.py').read_text())
    function=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='validate_runtime')
    assert any(isinstance(n,ast.Call) and isinstance(n.func,ast.Name) and n.func.id=='local_capability_errors'
               and len(n.args)==2 and isinstance(n.args[0],ast.Name) and n.args[0].id=='runtime'
               and isinstance(n.args[1],ast.Name) and n.args[1].id=='capabilities' for n in ast.walk(function))

def test_native_install_gate_precedes_sprites_and_first_transport():
    source=(ROOT/'pc/src-tauri/src/usb_serial.rs').read_text()
    source=source[source.index('    fn install_widget_clawpkg_mode<F>('):]
    compact=''.join(source.split())
    gate=compact.index('ifextended_local_deck&&install_status.capabilities["widgetLocalToolsMasks"]!="p4-local-masks-v2"')
    assert 'items.len()>16' in source
    assert 'crate::app_component_program::validate(&widget_definition)?' in source
    assert gate<compact.index('prepare_p4_widget_sprite_files(widget_dir,widget_source)?')
    assert gate<compact.index('register_widget_ack_waiter(&transfer_id,"begin"')<compact.index('send_bound("widget/begin"')

if __name__=='__main__':
    for test in [test_shared_local_masks_capability_vectors,test_python_actual_target_validator_calls_capability_admission,test_native_install_gate_precedes_sprites_and_first_transport]:
        test();print(test.__name__+' PASSED')
