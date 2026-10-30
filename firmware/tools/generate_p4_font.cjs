// Rebuild the uncompressed MiSans subsets with lv_font_conv 1.5.3.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');

async function main() {
  const args = process.argv.slice(2);
  if (args.length !== 4 || args[0] !== '--font' || args[2] !== '--converter') {
    throw Error('Usage: node firmware/tools/generate_p4_font.cjs --font MiSans-Medium.otf --converter path/to/lv_font_conv');
  }
  const converter = path.resolve(args[3]);
  assert.equal(require(path.join(converter, 'package.json')).version, '1.5.3');
  const ft = require(path.join(converter, 'lib/freetype'));
  const LvGlyf = require(path.join(converter, 'lib/writers/lvgl/lv_table_glyf'));
  const target = path.resolve(__dirname, '../main/pet_p4_font_cn.c');
  const source = fs.readFileSync(target, 'utf8').replace(/\r\n/g, '\n');
  const array = name => {
    const body = source.match(new RegExp(`\\b${name}\\[\\d*\\]\\s*=\\s*\\{([\\s\\S]*?)\\};`))?.[1];
    if (!body) throw Error(`Missing font array: ${name}`);
    return (body.replace(/\/\*[\s\S]*?\*\//g, '').match(/-?0x[\da-f]+|-?\d+/gi) ?? []).map(Number);
  };
  const oldGlyphs = [...source.matchAll(/\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}/g)].map(m => m.slice(1).map(Number));
  const oldIds = new Map();
  for (const m of source.matchAll(/\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*\.unicode_list = (\w+), \.glyph_id_ofs_list = NULL, \.list_length = \d+, \.type = (\w+)/g)) {
    assert(['LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY', 'LV_FONT_FMT_TXT_CMAP_SPARSE_TINY'].includes(m[5]));
    const [base, length, first] = m.slice(1, 4).map(Number);
    const offsets = m[4] === 'NULL' ? Array.from({ length }, (_, i) => i) : array(m[4]);
    offsets.forEach((offset, i) => oldIds.set(base + offset, first + i));
  }
  assert.equal(oldIds.size, oldGlyphs.length - 1, 'Incomplete input cmap');
  const left = array('kern_left_class_mapping'), right = array('kern_right_class_mapping');
  const chars = new Set();
  const decoder = new TextDecoder('gbk', { fatal: true });
  for (let hi = 0xb0; hi <= 0xf7; hi++) {
    for (let lo = 0xa1; lo <= 0xfe; lo++) {
      if (hi === 0xd7 && lo >= 0xfa) continue; // Five unassigned GB2312 cells.
      chars.add(decoder.decode(Uint8Array.of(hi, lo)).codePointAt(0));
    }
  }
  assert.equal(chars.size, 6763);
  for (const cp of oldIds.keys()) if (cp < 0x4e00 || cp > 0x9fff) chars.add(cp);
  for (const name of fs.readdirSync(path.dirname(target))) {
    if (!/\.[ch]$/.test(name) || ['pet_p4_font_cn.c', 'pet_p4_builtin_components_data.c'].includes(name)) continue;
    const code = fs.readFileSync(path.join(path.dirname(target), name), 'utf8');
    for (const [literal] of code.matchAll(/"(?:\\.|[^"\\])*"/g)) {
      for (const [han] of literal.matchAll(/[\u4e00-\u9fff]/g)) chars.add(han.codePointAt(0));
    }
  }
  const codes = [...chars].sort((a, b) => a - b);
  assert(codes.at(-1) - codes[0] < 65535);
  const font = { opts: { bpp: 2, no_compress: true } };
  font.glyf = new LvGlyf(font);
  const rows = [], descriptions = [], leftSubset = [0], rightSubset = [0];
  let offset = 0;
  await ft.init();
  const face = ft.fontface_create(fs.readFileSync(path.resolve(args[1])), 16);
  try {
    for (const cp of codes) {
      assert(ft.glyph_exists(face, cp), `Source font is missing U+${cp.toString(16)}`);
      const g = ft.glyph_render(face, cp, { mono: false });
      const data = font.glyf.lv_bitmap({ bbox: { width: g.width, height: g.height }, pixels: g.pixels });
      assert.equal(data.length, Math.ceil(g.width * g.height / 4));
      const old = oldIds.get(cp);
      const advance = old === undefined ? Math.round(g.advance_x * 16) : oldGlyphs[old][1];
      rows.push(`    /* U+${cp.toString(16).toUpperCase().padStart(4, '0')} ${JSON.stringify(String.fromCodePoint(cp))} */\n${dump([...data], true)}${data.length ? ',' : ''}`);
      descriptions.push(`    {.bitmap_index = ${offset}, .adv_w = ${advance}, .box_w = ${g.width}, .box_h = ${g.height}, .ofs_x = ${g.x}, .ofs_y = ${g.y - g.height}}`);
      leftSubset.push(old === undefined ? 0 : left[old]);
      rightSubset.push(old === undefined ? 0 : right[old]);
      offset += data.length;
    }
  } finally { ft.fontface_destroy(face); ft.destroy(); }
  assert(offset < 1024 * 1024, 'Bitmap exceeds the small LVGL font format');
  const bitmapsStart = source.indexOf('/*-----------------\n *    BITMAPS');
  const kerningStart = source.indexOf('/*-----------------\n *    KERNING');
  assert(bitmapsStart >= 0 && kerningStart > bitmapsStart);
  const bitmapSection = `/*-----------------\n *    BITMAPS\n *----------------*/\n\nstatic LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n${rows.join('\n\n')}\n};\n\nstatic const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,\n${descriptions.join(',\n')}\n};\n\nstatic const uint16_t unicode_list_0[] = {\n${dump(codes.map(cp => cp - codes[0]), true)}\n};\n\nstatic const lv_font_fmt_txt_cmap_t cmaps[] = {\n    {\n        .range_start = ${codes[0]}, .range_length = ${codes.at(-1) - codes[0] + 1}, .glyph_id_start = 1,\n        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = ${codes.length}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n    }\n};\n\n`;
  let output = source.slice(0, bitmapsStart) + bitmapSection + source.slice(kerningStart);
  for (const [name, values] of [['kern_left_class_mapping', leftSubset], ['kern_right_class_mapping', rightSubset]]) {
    output = output.replace(new RegExp(`(\\b${name}\\[\\]\\s*=\\s*\\{)[\\s\\S]*?(\\};)`), `$1\n${dump(values)}\n$2`);
  }
  output = output.replace(/ \* Bpp: \d+/, ' * Bpp: 2')
    .replace(/ \* Opts:.*\n/, ` * Charset: GB2312 Hanzi, firmware UI Hanzi, existing Latin/punctuation; ${codes.length} glyphs.\n * Rebuild: node firmware/tools/generate_p4_font.cjs --font MiSans-Medium.otf --converter path/to/lv_font_conv\n`)
    .replace(/\.cmap_num = \d+/, '.cmap_num = 1')
    .replace(/\.bpp = \d+/, '.bpp = 2');
  // Avoid duplicating the generated charset/rebuild header on subsequent runs.
  output = output.replace(/( \* Charset:[^\n]*\n \* Rebuild:[^\n]*\n)(?: \* Charset:[^\n]*\n \* Rebuild:[^\n]*\n)+/, '$1');
  if (/ \* Charset:/.test(output)) output = output.replace(/ \* Charset:[^\n]*/, ` * Charset: GB2312 Hanzi, firmware UI Hanzi, existing Latin/punctuation; ${codes.length} glyphs.`);
  fs.writeFileSync(target, output);
  console.log(`Generated ${codes.length} glyphs, ${offset} bitmap bytes; original advances, kerning, and 18/3 line metrics retained.`);
  await generateTitleFont(ft, LvGlyf, args[1], target, oldIds, oldGlyphs);
}

async function generateTitleFont(ft, LvGlyf, fontPath, baseTarget, oldIds, oldGlyphs) {
  const catalog = JSON.parse(fs.readFileSync(path.join(path.dirname(baseTarget), 'pet_p4_builtin_components.json'), 'utf8'));
  const chars = new Set(Array.from({ length: 95 }, (_, i) => i + 32));
  // Fixed 32 px bubble labels; body hints use the separate 24 px rendering path.
  for (const title of ['组件中心', '休息中', '你', '宠物', ...catalog.components.map(item => item.widget.dashboard.title)]) {
    assert.equal(typeof title, 'string');
    for (const char of title) chars.add(char.codePointAt(0));
  }
  const codes = [...chars].sort((a, b) => a - b);
  const font = { opts: { bpp: 2, no_compress: true } };
  font.glyf = new LvGlyf(font);
  const rows = [], descriptions = [];
  let offset = 0;
  await ft.init();
  const face = ft.fontface_create(fs.readFileSync(path.resolve(fontPath)), 32);
  try {
    for (const cp of codes) {
      assert(ft.glyph_exists(face, cp), `Source font is missing U+${cp.toString(16)}`);
      assert(oldIds.has(cp), `16 px font is missing U+${cp.toString(16)}`);
      const g = ft.glyph_render(face, cp, { mono: false });
      const data = font.glyf.lv_bitmap({ bbox: { width: g.width, height: g.height }, pixels: g.pixels });
      assert.equal(data.length, Math.ceil(g.width * g.height / 4));
      rows.push(`    /* U+${cp.toString(16).toUpperCase().padStart(4, '0')} ${JSON.stringify(String.fromCodePoint(cp))} */\n${dump([...data], true)}${data.length ? ',' : ''}`);
      descriptions.push(`    {.bitmap_index = ${offset}, .adv_w = ${oldGlyphs[oldIds.get(cp)][1] * 2}, .box_w = ${g.width}, .box_h = ${g.height}, .ofs_x = ${g.x}, .ofs_y = ${g.y - g.height}}`);
      offset += data.length;
    }
  } finally { ft.fontface_destroy(face); ft.destroy(); }
  const output = `/*******************************************************************************
 * Size: 32 px
 * Bpp: 2
 * Font: MiSans Medium, downloaded from https://hyperos.mi.com/font/zh/
 * MiSans is used under the MiSans Fonts Intellectual Property License Agreement.
 * The source font is intentionally not redistributed with this repository.
 * Charset: built-in component titles, launcher/fixed bubble labels and printable ASCII; ${codes.length} glyphs.
 * Rebuild: node firmware/tools/generate_p4_font.cjs --font MiSans-Medium.otf --converter path/to/lv_font_conv
 ******************************************************************************/

#include "lvgl.h"

static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
${rows.join('\n\n')}
};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
${descriptions.join(',\n')}
};

static const uint16_t unicode_list_0[] = {
${dump(codes.map(cp => cp - codes[0]), true)}
};

static const lv_font_fmt_txt_cmap_t cmaps[] = {
    {
        .range_start = ${codes[0]}, .range_length = ${codes.at(-1) - codes[0] + 1}, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = ${codes.length}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};

static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .cmap_num = 1,
    .bpp = 2,
    .bitmap_format = 0,
};

const lv_font_t pet_p4_font_cn_32 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 36,
    .base_line = 6,
    .dsc = &font_dsc,
};
`;
  fs.writeFileSync(path.join(path.dirname(baseTarget), 'pet_p4_font_cn_32.c'), output);
  console.log(`Generated 32 px title subset: ${codes.length} glyphs, ${offset} bitmap bytes.`);
}

function dump(values, hex = false) {
  const lines = [];
  for (let i = 0; i < values.length; i += 8) lines.push('    ' + values.slice(i, i + 8).map(v => hex ? '0x' + v.toString(16) : String(v)).join(', '));
  return lines.join(',\n');
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
