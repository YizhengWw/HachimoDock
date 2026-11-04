"""p4-logic-v1 shared vectors: Python model semantics, validator rejections, firmware parser/report parity.

Offline evidence only. It does not prove device timing, saves or input handling.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/skills/petui/scripts'))
from bounded_app import initial, action, advance  # noqa: E402
from validate_generated_widget import validate_logic_profile, DEFAULT_CAPABILITIES  # noqa: E402

SUITE = json.loads((ROOT / 'pc/skills/petui/contracts/logic-v1.json').read_text())


def test_python_runtime_matches_logic_vectors():
    assert SUITE['schemaVersion'] == 1 and SUITE['profile'] == 'p4-logic-v1'
    for item in SUITE['cases']:
        runtime = item['runtime']
        state = initial(runtime)
        for event in item['events']:
            state = action(runtime, state, event['action']) if 'action' in event else advance(runtime, state, event['ms'])
        assert {k: state[k] for k in ('state', 'page', 'vars')} == item['expected'], item['name']


def test_validator_accepts_cases_and_rejects_vectors():
    for item in SUITE['cases']:
        errors = []
        validate_logic_profile(item['runtime'], DEFAULT_CAPABILITIES, errors)
        assert errors == [], (item['name'], errors)
    for item in SUITE['rejections']:
        errors = []
        validate_logic_profile(item['runtime'], DEFAULT_CAPABILITIES, errors)
        assert errors, item['name']


def test_validator_requires_device_capability():
    runtime = SUITE['cases'][0]['runtime']
    errors = []
    validate_logic_profile(runtime, {**DEFAULT_CAPABILITIES, 'widgetLogic': None}, errors)
    assert any('widgetLogic' in e for e in errors)


def test_firmware_declares_profile_and_parses_every_vector_shape():
    protocol = (ROOT / 'firmware/main/pet_p4_protocol.c').read_text()
    runtime = (ROOT / 'firmware/main/pet_p4_miniapp.c').read_text()
    game = (ROOT / 'firmware/main/pet_p4_game.h').read_text()
    assert 'cJSON_AddStringToObject(capabilities, "widgetLogic", "p4-logic-v1");' in protocol
    assert int(re.search(r'cJSON_AddNumberToObject\(widget_limits, "maxConditions", (\d+)\)', protocol).group(1)) == 2
    assert '#define MINIAPP_MAX_CONDITIONS 2' in runtime
    assert '"logic",' in runtime and 'parse_condition(' in runtime and 'conditions_hold(' in runtime
    assert 'PET_P4_GAME_OP_VAR_SET' in game and 'PET_P4_GAME_MAX_VARS 8' in game
    # The else branch must share the bounded effect array, not grow the runtime struct.
    assert 'uint8_t else_effect_count;' in runtime and 'else_effects[' not in runtime


def test_c_compare_table_matches_python_model():
    """Compile pet_p4_game_compare from the real source and compare against the Python table."""
    import subprocess
    import tempfile
    from bounded_app import _COMPARE
    source = (ROOT / 'firmware/main/pet_p4_game.c').read_text()
    header = (ROOT / 'firmware/main/pet_p4_game.h').read_text()
    match = re.search(r'bool pet_p4_game_compare\(int32_t left, unsigned op, int32_t right\) \{.*?\n\}\n', source, re.S)
    table = re.search(r'typedef enum \{\s*PET_P4_GAME_CMP_LTE = 0,.*?\} pet_p4_game_cmp_t;', header, re.S)
    assert match and table
    program = '#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n' + table.group(0) + '\n' + match.group(0) + '''
int main(void){int ops=6;int vals[]={-3,0,2,5};for(int o=0;o<ops;o++)for(int i=0;i<4;i++)for(int j=0;j<4;j++)printf("%d %d %d %d\\n",o,vals[i],vals[j],pet_p4_game_compare(vals[i],o,vals[j]));return 0;}
'''
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'cmp.c'
        exe = Path(tmp) / 'cmp'
        src.write_text(program)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    names = ['lte', 'lt', 'gte', 'gt', 'eq', 'ne']
    for line in out.strip().splitlines():
        o, a, b, r = (int(x) for x in line.split())
        assert bool(r) == _COMPARE[names[o]](a, b), line


def test_logic_profile_is_admitted_beside_a_native_game_preset():
    """A native preset (blocks/snake/flappy/gomoku) never writes the mirrored vars, so
    guards, {var} sources and tick conditions are usable beside it; the exclusivity
    list keeps local/media/instrument/host/program. Firmware, Python and Rust agree."""
    runtime = (ROOT / 'firmware/main/pet_p4_miniapp.c').read_text()
    admission = runtime.split('if(cJSON_HasObjectItem(root,"logic")){', 1)[1].split('parsed->logic=true;', 1)[0]
    assert 'legacy_game' not in admission
    for key in ('local', 'media', 'instrument', 'host', 'program'):
        assert f'cJSON_HasObjectItem(root,"{key}")' in admission
    # the mirror is inert for presets: pushed before the engine step, pulled after, guarded by game.active
    assert 'if (!runtime->logic || !runtime->game.active) return;' in runtime
    assert 'if (!runtime->logic || !runtime->game.active) return false;' in runtime
    rust = (ROOT / 'pc/src-tauri/src/clawpkg.rs').read_text()
    assert '["local", "media", "instrument", "host", "program"].iter().any(|k| widget.get(*k).is_some())' in rust
    assert '["game", "local", "media"' not in rust

    gomoku = {
        'engine': 'p4-bounded-runtime-v4', 'logic': 'p4-logic-v1',
        'vars': {'score': {'type': 'int', 'init': 0}, 'half': {'type': 'int', 'init': 2}},
        'states': ['ready', 'playing', 'result'], 'initial_state': 'ready',
        'transitions': [{'from': 'ready', 'on': 'gomoku.place', 'to': 'playing'}],
        'tick': [{'every_ms': 100, 'while_state': 'playing',
                  'when': {'var': 'score', 'op': 'gte', 'value': {'var': 'half'}},
                  'then': {'inc': {'half': 2}}}],
        'game': {'type': 'gomoku', 'tick_ms': 300, 'playing_state': 'playing', 'result_state': 'result',
                 'score_var': 'score', 'actions': {'place': 'gomoku.place', 'left': 'gomoku.left', 'right': 'gomoku.right',
                                                   'up': 'gomoku.up', 'down': 'gomoku.down', 'undo': 'gomoku.undo'}},
    }
    errors = []
    validate_logic_profile(gomoku, DEFAULT_CAPABILITIES, errors)
    assert errors == [], errors
    for key in ('local', 'media', 'instrument', 'host', 'program'):
        errors = []
        validate_logic_profile({**gomoku, key: {}}, DEFAULT_CAPABILITIES, errors)
        assert any('混用' in e for e in errors), key
    # the preview model runs the same tick condition: an even stone count past `half` fires once
    state = initial(gomoku)
    state = action(gomoku, state, 'gomoku.place')
    state['vars']['score'] = 2
    state = advance(gomoku, state, 100)
    assert state['vars']['half'] == 4
    state = advance(gomoku, state, 100)
    assert state['vars']['half'] == 4
