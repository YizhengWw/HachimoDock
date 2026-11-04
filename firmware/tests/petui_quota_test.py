"""p4-quota-v2: shared vectors against the Python validator and firmware constant parity.

Offline evidence only: it does not measure device RAM, frame time or save/restore.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/skills/petui/scripts'))
from validate_generated_widget import DEFAULT_CAPABILITIES, QUOTA_V1, QUOTA_V2, quota_caps  # noqa: E402
import validate_generated_widget as v  # noqa: E402

SUITE = json.loads((ROOT / 'pc/skills/petui/contracts/quota-v2.json').read_text())


def _runtime_errors(runtime):
    errors = []
    v.validate_runtime(runtime, 'tool', ROOT, DEFAULT_CAPABILITIES, errors)
    return errors


def test_vectors_match_python_validator():
    assert SUITE['profile'] == 'p4-quota-v2'
    for item in SUITE['vectors']:
        errors = _runtime_errors(item['runtime'])
        assert (not errors) == item['ok'], (item['name'], errors)


def test_contract_limits_match_validator_tables():
    limits = SUITE['limits']
    assert QUOTA_V1['vars'] == limits['v1']['vars'] == 8 and QUOTA_V2['vars'] == limits['v2']['vars'] == 16
    assert QUOTA_V1['nodes'] == limits['v1']['surfaceNodes'] == 32 and QUOTA_V2['nodes'] == limits['v2']['surfaceNodes'] == 48
    assert QUOTA_V1['localItems'] == 16 and QUOTA_V2['localItems'] == 24
    assert quota_caps({'quota': 'p4-quota-v2'}) is QUOTA_V2 and quota_caps({}) is QUOTA_V1


def test_firmware_constants_and_capability_report_match_contract():
    source = (ROOT / 'firmware/main/pet_p4_miniapp.c').read_text()
    protocol = (ROOT / 'firmware/main/pet_p4_protocol.c').read_text()
    surface = (ROOT / 'firmware/main/pet_p4_surface.h').read_text()
    local = (ROOT / 'firmware/main/pet_p4_local_tools.h').read_text()
    v2 = SUITE['limits']['v2']
    v1 = SUITE['limits']['v1']

    def define(text, name):
        return int(re.search(r'#define ' + name + r' (\d+)', text).group(1))

    # r150: the v2 ceilings keep their own named constants; the array capacity (MINIAPP_MAX_*) is v2 unless PET_P4_QUOTA_V3 is on.
    assert define(source, 'MINIAPP_V2_MAX_VARS') == v2['vars'] and define(source, 'MINIAPP_V1_MAX_VARS') == v1['vars']
    assert define(source, 'MINIAPP_V2_MAX_STATES') == v2['states'] and define(source, 'MINIAPP_V1_MAX_STATES') == v1['states']
    assert define(source, 'MINIAPP_V2_MAX_PAGES') == v2['pages'] and define(source, 'MINIAPP_V1_MAX_PAGES') == v1['pages']
    assert define(source, 'MINIAPP_V2_MAX_TRANSITIONS') == v2['transitions'] and define(source, 'MINIAPP_V1_MAX_TRANSITIONS') == v1['transitions']
    assert define(source, 'MINIAPP_V2_MAX_TICKS') == v2['ticks'] and define(source, 'MINIAPP_V1_MAX_TICKS') == v1['ticks']
    assert define(source, 'MINIAPP_V2_MAX_CHOICES') == v2['switchChoices'] and define(source, 'MINIAPP_V1_MAX_CHOICES') == v1['switchChoices']
    assert define(source, 'MINIAPP_V2_SURFACE_NODES') == v2['surfaceNodes'] and define(source, 'MINIAPP_V1_SURFACE_NODES') == v1['surfaceNodes']
    assert '#else\n#define PET_SURFACE_NODES 48' in surface and '#else\n#define PET_LOCAL_ITEMS 24' in local
    assert define(source, 'MINIAPP_V2_LOCAL_ITEMS') == v2['localItems'] and define(source, 'MINIAPP_V1_LOCAL_ITEMS') == v1['localItems']
    assert define(source, 'MINIAPP_V1_SAVE_VARS') == v1['saveVars'] and define(source, 'MINIAPP_V2_SAVE_VARS') == v2['saveVars']
    assert '#define MINIAPP_MAX_VARS MINIAPP_V2_MAX_VARS' in source
    # Saved-var mask must cover the v2 (and v3) ceiling.
    assert 'uint32_t save_vars;' in source
    assert 'cJSON_AddStringToObject(capabilities, "widgetQuota", "p4-quota-v2");' in protocol  # the non-v3 build still reports v2
    block = protocol[protocol.index('cJSON *quota = cJSON_CreateObject();'):protocol.index('cJSON_AddItemToObject(widget_limits, "v2", quota);')]
    for key, expected in (('maxVars', v2['vars']), ('maxStates', v2['states']), ('maxPages', v2['pages']), ('maxTransitions', v2['transitions']), ('maxTicks', v2['ticks']), ('maxSwitchChoices', v2['switchChoices']), ('maxSurfaceNodes', v2['surfaceNodes']), ('maxLocalItems', v2['localItems']), ('maxSaveVars', v2['saveVars']), ('maxWidgetJsonBytes', v2['widgetJsonBytes'])):
        assert int(re.search(r'"' + key + r'", (\d+)\)', block).group(1)) == expected, key
    # v1 report values stay untouched for clients that do not understand profiles.
    assert 'cJSON_AddNumberToObject(widget_limits, "maxVars", 8);' in protocol
    assert 'cJSON_AddNumberToObject(widget_limits, "maxSurfaceNodes", 32);' in protocol
