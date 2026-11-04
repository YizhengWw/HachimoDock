"""p4-texts-v1: bounded package text table shared by the validator and the preview runtime.

Mirrors parse_texts/text_at in firmware/main/pet_p4_miniapp.c: 1-64 UTF-8 entries without control
characters, <=120 bytes each; the pool stores entries NUL-separated inside 4096 bytes, so the total
counts one separator between entries. An index outside 0..len-1 resolves to the empty string.
"""
ENGINE = 'p4-texts-v1'
MAX_ENTRIES = 64
MAX_ENTRY_BYTES = 120
MAX_TOTAL_BYTES = 4096


def valid_entry(value):
    if not isinstance(value, str):
        return False
    try:
        size = len(value.encode('utf-8'))
    except UnicodeEncodeError:
        return False
    return size <= MAX_ENTRY_BYTES and not any(ord(ch) < 32 or ord(ch) == 127 for ch in value)


def declared(runtime):
    texts = runtime.get('texts') if isinstance(runtime, dict) else None
    return isinstance(texts, list) and len(texts) >= 1


def schema_errors(runtime):
    texts = runtime.get('texts')
    if not isinstance(texts, list) or not 1 <= len(texts) <= MAX_ENTRIES:
        return [f'runtime/widget.json.texts 必须是 1-{MAX_ENTRIES} 个字符串']
    errors = []
    if not all(valid_entry(t) for t in texts):
        errors.append(f'runtime/widget.json.texts 每项必须是不含控制字符、最多 {MAX_ENTRY_BYTES} UTF-8 字节的字符串')
    else:
        used = sum(len(t.encode('utf-8')) for t in texts) + len(texts) - 1
        if used > MAX_TOTAL_BYTES:
            errors.append(f'runtime/widget.json.texts 总字节数超过 {MAX_TOTAL_BYTES}')
    if runtime.get('engine') != 'p4-bounded-runtime-v4':
        errors.append('runtime/widget.json.texts 需要 engine=p4-bounded-runtime-v4')
    return errors


def text_at(texts, index):
    """Resolve a text_at binding; anything outside the table is the empty string."""
    if type(index) is not int or not isinstance(texts, list) or not 0 <= index < len(texts):
        return ''
    return texts[index] if isinstance(texts[index], str) else ''
