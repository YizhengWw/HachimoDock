"""Contract gating for the new authored touch component, not historical packages."""
import copy
import importlib.util
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'pc/skills/petui/scripts'))
spec = importlib.util.spec_from_file_location('touch_validator', ROOT/'pc/skills/petui/scripts/validate_generated_widget.py')
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)
PACKAGE = ROOT/'pc/builtin-clawpkgs/fresh-fruit-slice'
CAPS = json.loads((ROOT/'pc/tests/fixtures/touch-slice-capabilities.json').read_text())

def test_touch_package_requires_physical_pointer_capability():
    assert validator.validate_widget(PACKAGE, CAPS) == []
    for key, value in [('widgetInput', ''), ('widgetPointer', ''), ('widgetScenes', []), ('widgetSliceRevision', 1), ('touchInput', {'ready': False})]:
        caps=copy.deepcopy(CAPS);caps[key]=value
        assert validator.validate_widget(PACKAGE, caps)

def test_touch_package_cannot_embed_executable_code(tmp_path):
    shutil.copytree(PACKAGE, tmp_path/'package')
    (tmp_path/'package/assets/evil.wasm').write_bytes(b'\0asm')
    assert validator.validate_widget(tmp_path/'package', CAPS)

def test_touch_config_rejects_extra_transitions_and_overbudget_sprites(tmp_path):
    shutil.copytree(PACKAGE, tmp_path/'package')
    path=tmp_path/'package/runtime/widget.json'
    source=json.loads(path.read_text())
    for field,value in [('duration_sec', 121), ('lives',0), ('sound_volume',101),('script','eval()')]:
        runtime=copy.deepcopy(source);runtime['scene'][field]=value
        path.write_text(json.dumps(runtime))
        assert validator.validate_widget(tmp_path/'package',CAPS)
    runtime=copy.deepcopy(source);runtime['scene']['sprites'][0]['frames']=2
    path.write_text(json.dumps(runtime))
    assert validator.validate_widget(tmp_path/'package',CAPS)
