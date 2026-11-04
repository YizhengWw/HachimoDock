import test from 'node:test';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';
import {runInNewContext} from 'node:vm';
import {deflateSync} from 'node:zlib';

// Normal repository resolution after copying this file beside CellsLayer.jsx.
// The private mirror uses only the explicitly supplied existing dependency root.
const require = createRequire(process.env.PREVIEW_TEST_MODULE_ROOT
  ? resolve(process.env.PREVIEW_TEST_MODULE_ROOT, 'package.json') : import.meta.url);
const React = require('react');
const {renderToStaticMarkup} = require('react-dom/server');
const {buildSync} = require('esbuild');
const module = {exports: {}};
const bundle = buildSync({entryPoints: [fileURLToPath(new URL('./CellsLayer.jsx', import.meta.url))],
  bundle: true, write: false, platform: 'node', format: 'cjs', external: ['react'], logLevel: 'silent'});
runInNewContext(bundle.outputFiles[0].text, {module, exports: module.exports,
  require: name => {assert.equal(name, 'react'); return React;}}, {filename: 'CellsLayer.actual.cjs'});
const CellsLayer = module.exports.default;
const render = props => {
  const html = renderToStaticMarkup(React.createElement('svg', null, React.createElement(CellsLayer, props)));
  return html.slice(html.indexOf('>') + 1, html.lastIndexOf('</svg>'));
};
// Parse rendered output, never the JSX source. SVG attributes are the contract.
const elements = (html, tag) => [...html.matchAll(new RegExp(`<${tag}\\b([^>]*)>`, 'g'))]
  .map(([, attrs]) => Object.fromEntries([...attrs.matchAll(/([\w:-]+)="([^"]*)"/g)].map(([, k, v]) => [k, v])));
const geometry = attrs => Object.fromEntries(['x', 'y', 'width', 'height', 'rx'].filter(k => k in attrs).map(k => [k, Number(attrs[k])]));
const node = {x: 10, y: 20, w: 48, h: 16, color: 0xf800, foreground: 0x07e0, stroke: 0x001f};
const grid = {cols: 4, rows: 1, bits: 2};

// A real bounded RGBA PNG; no image decoding or network is needed for SSR.
function png(width, height) {
  const crc = bytes => {let n = 0xffffffff; for (const b of bytes) {n ^= b;
    for (let i = 0; i < 8; i++) n = (n >>> 1) ^ (n & 1 ? 0xedb88320 : 0);} return (n ^ 0xffffffff) >>> 0;};
  const chunk = (type, data) => {const body = Buffer.concat([Buffer.from(type), data]);
    const length = Buffer.alloc(4), checksum = Buffer.alloc(4); length.writeUInt32BE(data.length); checksum.writeUInt32BE(crc(body));
    return Buffer.concat([length, body, checksum]);};
  const header = Buffer.alloc(13); header.writeUInt32BE(width); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 6;
  return 'data:image/png;base64,' + Buffer.concat([Buffer.from('89504e470d0a1a0a', 'hex'), chunk('IHDR', header),
    chunk('IDAT', deflateSync(Buffer.alloc((width * 4 + 1) * height))), chunk('IEND', Buffer.alloc(0))]).toString('base64');
}

test('zero cells are transparent; 1/2 are round pieces and 3 is a square with independent colors', () => {
  assert.equal(elements(render({node, grid, cells: ['0000']}), 'rect').length, 0);
  const rects = elements(render({node, grid, cells: ['0123']}), 'rect');
  assert.deepEqual(rects.map(geometry), [
    {x: 24, y: 22, width: 8, height: 12, rx: 4},
    {x: 36, y: 22, width: 8, height: 12, rx: 4},
    {x: 48, y: 22, width: 8, height: 12, rx: 0},
  ]);
  assert.deepEqual(rects.map(r => r.fill), ['rgb(255,0,0)', 'rgb(0,255,0)', 'rgb(0,0,255)']);
});

test('missing foreground uses the piece color, and a 4-bit high value remains square', () => {
  const rects = elements(render({node: {...node, foreground: undefined}, grid: {...grid, bits: 4}, cells: ['012f']}), 'rect');
  assert.equal(rects[1].fill, 'rgb(255,0,0)');
  assert.equal(rects[2].fill, 'rgb(0,0,255)');
  assert.equal(rects[2].rx, '0');
});

test('small tiles retain one-pixel inset rather than losing their pieces', () => {
  const rects = elements(render({node: {...node, w: 24, h: 7}, grid, cells: ['0123']}), 'rect');
  assert.deepEqual(geometry(rects[0]), {x: 17, y: 21, width: 4, height: 5, rx: 2});
});

test('atlas uses the independent package image ID and clips each whole-pixel tile', () => {
  const source = png(48, 19), unrelated = png(1, 1);
  const html = render({node: {...node, image: 'tile-atlas', columns: 4, w: 50, h: 19}, grid, cells: ['0123'],
    images: [{id: 'tile-atlas:1', dataUrl: unrelated}, {id: 'big:tile-atlas', dataUrl: unrelated}, {id: 'tile-atlas', dataUrl: source}]});
  assert.deepEqual(elements(html, 'svg').map(geometry), [
    {x: 22, y: 20, width: 12, height: 19}, {x: 34, y: 20, width: 12, height: 19}, {x: 46, y: 20, width: 12, height: 19},
  ]);
  for (const viewport of elements(html, 'svg')) {
    assert.equal(viewport.viewBox, '0 0 12 19'); assert.equal(viewport.overflow, 'hidden');
  }
  const images = elements(html, 'image');
  assert.deepEqual(images.map(geometry), [
    {x: -12, y: 0, width: 48, height: 19}, {x: -24, y: 0, width: 48, height: 19}, {x: -36, y: 0, width: 48, height: 19},
  ]);
  assert.ok(images.every(im => im.href === source && im.preserveAspectRatio === 'none'));
  assert.equal(elements(html, 'rect').length, 0);
});

test('missing independent atlas ID or an out-of-atlas value never selects an unrelated frame', () => {
  const source = png(36, 16), atlasNode = {...node, image: 'tiles', columns: 3};
  assert.equal(elements(render({node: atlasNode, grid, cells: ['0123'], images: [{id: 'tiles:1', dataUrl: source}]}), 'image').length, 0);
  const images = elements(render({node: atlasNode, grid, cells: ['0123'], images: [{id: 'tiles', dataUrl: source}]}), 'image');
  assert.equal(images.length, 2); assert.deepEqual(images.map(im => Number(im.x)), [-12, -24]);
});

const cursorNode = {...node, w: 40, h: 30, cursor: {x_var: 'cx', y_var: 'cy', color: 0xffff}};
const cursorGrid = {cols: 2, rows: 2, bits: 2};
test('cursor is inset within its selected cell as four exact 2px edges', () => {
  const html = render({node: cursorNode, grid: cursorGrid, cells: ['00', '00'], vars: {cx: 1, cy: 1}});
  assert.deepEqual(elements(html, 'rect').map(geometry), [
    {x: 30, y: 35, width: 20, height: 2}, {x: 30, y: 48, width: 20, height: 2},
    {x: 30, y: 35, width: 2, height: 15}, {x: 48, y: 35, width: 2, height: 15},
  ]);
  assert.equal(elements(html, 'g').find(g => g.fill)?.fill, 'rgb(255,255,255)');
});
test('out-of-grid, fractional, nonnumeric and absent cursor coordinates hide every edge', () => {
  for (const vars of [{cx: -1, cy: 0}, {cx: 2, cy: 0}, {cx: 0, cy: -1}, {cx: 0, cy: 2},
    {cx: 0.5, cy: 0}, {cx: 0, cy: 0.5}, {cx: '1', cy: 0}, {cx: NaN, cy: 0}, {cx: Infinity, cy: 0}, {}]) {
    assert.equal(elements(render({node: cursorNode, grid: cursorGrid, cells: ['00', '00'], vars}), 'rect').length, 0, String(vars.cx));
  }
});

test('malformed or oversized grids and invalid snapshots are rejected before rendering', () => {
  const invalid = [null, {cols: 0, rows: 1, bits: 2}, {cols: 17, rows: 1, bits: 2},
    {cols: 1.5, rows: 1, bits: 2}, {cols: 4, rows: 0, bits: 2}, {cols: 4, rows: 17, bits: 2},
    {cols: '4', rows: 1, bits: 2}, {cols: 4, rows: 1, bits: 1}, {cols: 4, rows: 1, bits: 8}];
  for (const grid of invalid) assert.equal(render({node, grid, cells: ['0123']}), '', JSON.stringify(grid));
  for (const cells of [null, [], ['012'], ['01230'], ['012g'], ['0124'], ['0123', '0000'], [123]]) {
    assert.equal(render({node, grid, cells}), '', JSON.stringify(cells));
  }
  for (const patch of [{x: NaN}, {y: Infinity}, {w: 0}, {h: -1}, {w: 3}]) {
    assert.equal(render({node: {...node, ...patch}, grid, cells: ['0123']}), '');
  }
  for (const columns of [0, 1, 17, 2.5, '4']) {
    assert.equal(render({node: {...node, image: 'tiles', columns}, grid, cells: ['0123']}), '');
  }
});

test('maximum 16x16 4-bit grid stays bounded at 256 visible pieces', () => {
  const html = render({node: {...node, w: 160, h: 160}, grid: {cols: 16, rows: 16, bits: 4}, cells: Array(16).fill('f'.repeat(16))});
  assert.equal(elements(html, 'rect').length, 256);
});
