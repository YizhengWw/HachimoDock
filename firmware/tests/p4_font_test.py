"""Check the shipped bitmap font's coverage, packing and UI line metrics."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'main/pet_p4_font_cn.c').read_text(encoding='utf-8')


def array(name, source=SOURCE):
    match = re.search(r'\b' + name + r'\[\d*\]\s*=\s*\{([\s\S]*?)\};', source)
    body = re.sub(r'/\*[\s\S]*?\*/', '', match[1])
    return [int(value, 0) for value in re.findall(r'-?0x[\da-f]+|-?\d+', body, re.I)]


GLYPHS = [tuple(map(int, row)) for row in re.findall(
    r'\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), '
    r'\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}', SOURCE)]
BITMAP = array('glyph_bitmap')
BASE = int(re.search(r'\.range_start = (\d+)', SOURCE)[1])
IDS = {BASE + offset: i + 1 for i, offset in enumerate(array('unicode_list_0'))}


def test_font_covers_gb2312_and_device_ui():
    han = set()
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                han.add(ord(bytes([hi, lo]).decode('gb2312')))
            except UnicodeDecodeError:
                pass
    assert len(han) == 6763
    assert han <= IDS.keys()
    for source in (ROOT / 'main').iterdir():
        if source.suffix not in {'.c', '.h'} or source.name in {
            'pet_p4_font_cn.c', 'pet_p4_builtin_components_data.c'
        }:
            continue
        for literal in re.findall(r'"(?:\\.|[^"\\])*"', source.read_text(encoding='utf-8')):
            assert set(map(ord, re.findall(r'[\u4e00-\u9fff]', literal))) <= IDS.keys()
    assert set(map(ord, '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz，。！？')) <= IDS.keys()


def test_bitmap_packing_and_real_gray_levels():
    assert len(GLYPHS) == len(IDS) + 1
    assert len(set(IDS.values())) == len(IDS)
    assert all(a < b for a, b in zip(IDS, list(IDS)[1:]))
    for i, (offset, _, width, height, _, _) in enumerate(GLYPHS[1:], 1):
        size = (width * height * 2 + 7) // 8
        end = GLYPHS[i + 1][0] if i + 1 < len(GLYPHS) else len(BITMAP)
        assert offset + size == end
    for char in '你好聆听警报':
        offset, _, width, height, _, _ = GLYPHS[IDS[ord(char)]]
        levels = {(BITMAP[offset + bit // 4] >> (6 - (bit % 4) * 2)) & 3
                  for bit in range(width * height)}
        assert levels == {0, 1, 2, 3}


def test_existing_line_metrics_and_kerning_remain_available():
    assert re.search(r'\.bpp = 2\b', SOURCE)
    assert re.search(r'\.bitmap_format = 0\b', SOURCE)
    assert re.search(r'\.line_height = 18\b', SOURCE)
    assert re.search(r'\.base_line = 3\b', SOURCE)
    assert len(array('kern_left_class_mapping')) == len(GLYPHS)
    assert len(array('kern_right_class_mapping')) == len(GLYPHS)
    assert GLYPHS[IDS[ord('你')]][1] == 256


def test_native_32px_subset_covers_catalog_and_fixed_bubbles_without_changing_advances():
    source = (ROOT / 'main/pet_p4_font_cn_32.c').read_text(encoding='utf-8')
    base = int(re.search(r'\.range_start = (\d+)', source)[1])
    codes = [base + offset for offset in array('unicode_list_0', source)]
    catalog = json.loads((ROOT / 'main/pet_p4_builtin_components.json').read_text(encoding='utf-8'))
    required = set(range(32, 127)) | set(map(ord, '组件中心休息中你宠物'))
    for item in catalog['components']:
        required.update(map(ord, item['widget']['dashboard']['title']))
    assert codes == sorted(required)
    assert len(codes) < 512  # A title subset, not a second full Chinese font.
    glyphs = [tuple(map(int, row)) for row in re.findall(
        r'\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), '
        r'\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}', source)]
    bitmap = array('glyph_bitmap', source)
    assert len(glyphs) == len(codes) + 1
    assert len(bitmap) < 40 * 1024
    assert re.search(r'\.bpp = 2\b', source)
    assert re.search(r'\.bitmap_format = 0\b', source)
    assert re.search(r'\.line_height = 36\b', source)
    assert re.search(r'\.base_line = 6\b', source)
    for i, cp in enumerate(codes, 1):
        offset, advance, width, height, _, _ = glyphs[i]
        end = glyphs[i + 1][0] if i + 1 < len(glyphs) else len(bitmap)
        assert offset + (width * height * 2 + 7) // 8 == end
        assert advance == GLYPHS[IDS[cp]][1] * 2
    levels = set()
    for char in '每日新鲜事休息中你宠物':
        offset, _, width, height, _, _ = glyphs[codes.index(ord(char)) + 1]
        pixels = [(bitmap[offset + i // 4] >> (6 - i % 4 * 2)) & 3
                  for i in range(width * height)]
        assert {0, 3} <= set(pixels)
        assert set(pixels) & {1, 2}
        levels.update(pixels)
        assert height > 20
        # Native ink contains detail inside a 2x2 block; a doubled bitmap cannot.
        assert any(len({pixels[y * width + x], pixels[y * width + x + 1],
                        pixels[(y + 1) * width + x], pixels[(y + 1) * width + x + 1]}) > 1
                   for y in range(0, height - 1, 2) for x in range(0, width - 1, 2))
    assert levels == {0, 1, 2, 3}
