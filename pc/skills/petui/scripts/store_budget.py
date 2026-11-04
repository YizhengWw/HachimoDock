"""Worst-case ordinary app-save admission; pure, no IO or initial-value shortcuts."""
import re


def estimate(runtime):
    p = runtime.get('persist')
    if p is None:
        return 0, 1023
    v3 = runtime.get('quota') == 'p4-quota-v3'
    limit = 2303 if v3 else 1023
    count = 32 if v3 else 16 if runtime.get('quota') == 'p4-quota-v2' else 8
    selected = p.get('vars')
    if not isinstance(selected, list) or not 1 <= len(selected) <= count or any(not isinstance(k, str) for k in selected) or len(set(selected)) != len(selected):
        raise ValueError('persist vars')
    # Envelope includes worst state/version, punctuation and cJSON's print
    # preallocation slack. Alarm serialization is restricted ASCII, <=255B.
    used = 128 + (64 if p.get('page') is True else 0) + (268 if runtime.get('alarms') else 0)
    form = runtime.get('form', {})
    fields = form.get('fields', []) if form.get('engine') in ('p4-app-form-v2', 'p4-app-form-v3') else []
    for name in selected:
        if not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}', name):
            raise ValueError('persist name')
        spec = runtime.get('vars', {}).get(name, {})
        kind = spec.get('type')
        if kind == 'int':
            value_bytes = 11
        elif kind == 'string':
            committed = next((f for f in fields if f.get('kind') == 'text' and f.get('var') == name), None)
            if not v3 and committed is None:
                raise ValueError('persist string ownership')
            cap = committed.get('max_bytes') if committed else 127
            if type(cap) is not int or not 1 <= cap <= (127 if v3 else 63):
                raise ValueError('persist string bound')
            value_bytes = 2 + 6 * cap
        else:
            raise ValueError('persist variable type')
        used += len(name) + 4 + value_bytes
    return used, limit


def errors(runtime):
    try:
        used, limit = estimate(runtime)
        return [] if used <= limit else [f'persist 最坏 JSON 转义尺寸 {used} 超过存档预算 {limit} 字节']
    except (ValueError, TypeError, KeyError, AttributeError):
        return ['persist 需要有界、唯一且可持久化的变量子集']
