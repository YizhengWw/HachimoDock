"""Shared semantic vectors, not component-specific examples or hardware proof."""
import json
import re
import sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from bounded_app import initial,action,advance

def test_python_runtime_matches_shared_semantic_vectors():
    suite=json.loads((ROOT/'pc/skills/petui/contracts/runtime-semantics-v1.json').read_text())
    assert suite['schemaVersion']==1
    for item in suite['cases']:
        runtime=item['runtime'];state=initial(runtime)
        for event in item['events']:
            state=action(runtime,state,event['action']) if 'action' in event else advance(runtime,state,event['ms'])
        assert {k:state[k] for k in ('state','page','vars')}==item['expected'],item['name']

def test_capability_report_distinguishes_conditional_limits():
    protocol=(ROOT/'firmware/main/pet_p4_protocol.c').read_text()
    runtime=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text()
    def declared(key):
        return int(re.search(r'cJSON_AddNumberToObject\(widget_limits, "'+key+r'", (\d+)\)',protocol).group(1))
    assert declared('maxInputTransitions')==int(re.search(r'#define MINIAPP_V1_MAX_TRANSITIONS (\d+)',runtime).group(1))==24
    assert int(re.search(r'#define MINIAPP_V2_MAX_TRANSITIONS (\d+)',runtime).group(1))==32  # v2 remains 32
    assert int(re.search(r'#define MINIAPP_V3_MAX_TRANSITIONS (\d+)',runtime).group(1))==48  # v3 is explicit and negotiated
    assert declared('maxTransitions')==declared('maxLegacyTransitions')==12
    assert declared('maxWidgetJsonBytes')+1==int(re.search(r'#define MINIAPP_V2_WIDGET_JSON_MAX (\d+)',runtime).group(1))
    assert declared('maxLegacyWidgetJsonBytes')==4095
    assert declared('maxSurfaceNodes')==32
    assert declared('maxLegacySurfaceNodes')==20
