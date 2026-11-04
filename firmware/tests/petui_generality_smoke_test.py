"""Generic same-page tools must explore value changes, not only state names."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/skills/petui/scripts'))
from bounded_app import smoke
from validate_generated_widget import validate_claimed_mechanics


def counter_runtime():
    return {
        'states': ['ready'], 'initial_state': 'ready',
        'vars': {'count': {'type': 'int', 'init': 0}},
        'transitions': [
            {'from': '*', 'on': 'add', 'inc': {'count': 1}},
            {'from': '*', 'on': 'clear', 'set': {'count': 0}},
        ], 'tick': [], 'dashboard': {'metricValue': {'var': 'count'}},
        'surface': {'elements': [{'type': 'text', 'bind': 'metricValue'}]},
    }


def test_same_state_counter_clear_is_reachable_and_visible():
    r = counter_runtime()
    report = smoke(r, [{'action': 'add'}, {'action': 'clear'}])
    assert report['ok'], report
    assert report['visibleActions'] == ['add', 'clear']


def test_hidden_effect_is_not_mistaken_for_visible_feedback():
    r = counter_runtime()
    r['surface']['elements'] = [{'type': 'text', 'text': 'Static'}]
    report = smoke(r, [{'action': 'add'}, {'action': 'clear'}])
    assert not report['ok']
    assert report['visibleActions'] == []
    assert report['explorationTruncated'] is True


def test_still_reports_unreachable_state_with_unbounded_counter():
    r = counter_runtime()
    r['states'].append('unreachable')
    report = smoke(r, [{'action': 'add'}, {'action': 'clear'}])
    assert not report['ok']
    assert any('unreachable' in error for error in report['errors'])


def test_valid_continuous_motion_counts_as_implemented_movement():
    r = {
        'vars': {'charge': {'type': 'int', 'init': 0}},
        'input': {'hold': 'charge'},
        'scene': {
            'tick_ms': 20, 'grid': {'width': 16, 'height': 12},
            'entities': [
                {'id': 'body', 'x': 2, 'y': 6, 'height': 2},
                {'id': 'support', 'x': 0, 'y': 8, 'width': 5},
            ],
            'motion': {'engine': 'p4-motion-v1', 'body': 'body',
                'platforms': ['support'], 'gravity': 18, 'lift': 7,
                'speed': [2, 13], 'charge_ms': 1200, 'charge_var': 'charge'},
            'rules': [{'on': 'charge', 'do': [{'op': 'charge', 'entity': 'body'}]}],
        },
    }
    errors = []
    validate_claimed_mechanics({'description': 'Falling ends the round.'}, {}, r, {}, errors)
    assert errors == []
    r['scene']['motion']['body'] = 'missing'
    errors = []
    validate_claimed_mechanics({'description': 'Falling ends the round.'}, {}, r, {}, errors)
    assert errors
