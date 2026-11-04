import test from 'node:test';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';
import {runInNewContext} from 'node:vm';
import {deflateSync} from 'node:zlib';

const require = createRequire(process.env.PREVIEW_TEST_MODULE_ROOT
  ? resolve(process.env.PREVIEW_TEST_MODULE_ROOT, 'package.json') : import.meta.url);
const React = require('react');
const {renderToStaticMarkup} = require('react-dom/server');
const {buildSync} = require('esbuild');
const module = {exports: {}};
const bundle = buildSync({entryPoints: [fileURLToPath(new URL('./SurfaceMediaLayer.jsx', import.meta.url))],
  bundle: true, write: false, platform: 'node', format: 'cjs', external: ['react'], logLevel: 'silent'});
runInNewContext(bundle.outputFiles[0].text, {module, exports: module.exports,
  require: name => {assert.equal(name, 'react'); return React;}}, {filename: 'SurfaceMediaLayer.actual.cjs'});
const SurfaceMediaLayer = module.exports.default;
function render(props) {
  // React 18 emits this expected warning for the real clip useLayoutEffect in
  // SSR. Do not stub any hook; reject other warnings and restore the console.
  const errors = [], original = console.error;
  console.error = (...args) => errors.push(args);
  let html;
  try {html = renderToStaticMarkup(React.createElement('svg', null, React.createElement(SurfaceMediaLayer, props)));}
  finally {console.error = original;}
  for (const [message] of errors) assert.match(String(message), /^Warning: useLayoutEffect does nothing on the server/);
  return html.slice(html.indexOf('>') + 1, html.lastIndexOf('</svg>'));
}
const elements = (html, tag) => [...html.matchAll(new RegExp(`<${tag}\\b([^>]*)>`, 'g'))]
  .map(([, attrs]) => Object.fromEntries([...attrs.matchAll(/([\w:-]+)="([^"]*)"/g)].map(([, k, v]) => [k, v])));
const geometry = attrs => Object.fromEntries(['x', 'y', 'width', 'height'].map(k => [k, Number(attrs[k])]));
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
const source = png(200, 100);
const node = {x: 30, y: 50, w: 80, h: 40, big: 'map', offset_x_var: 'ox', offset_y_var: 'oy', color: 0};
const surface = {big_images: [{id: 'map', width: 200, height: 100}]};
const images = [{id: 'big:map', dataUrl: source, frameWidth: 200, frameHeight: 100}];

test('big image offsets are integer-clamped, with original dimensions preserved instead of viewport scaling', () => {
  for (const [vars, x, y] of [[{ox: 0, oy: 0}, 30, 50], [{ox: 12, oy: 7}, 18, 43],
    [{ox: 999, oy: 999}, -90, -10], [{ox: -5, oy: -8}, 30, 50],
    [{ox: 1.9, oy: 2.9}, 30, 50], [{ox: '12', oy: null}, 30, 50],
    [{ox: NaN, oy: Infinity}, 30, 50], [{}, 30, 50]]) {
    const rendered = elements(render({node, surface, images, vars}), 'image');
    assert.equal(rendered.length, 1);
    assert.deepEqual(geometry(rendered[0]), {x, y, width: 200, height: 100});
    assert.equal(rendered[0].href, source); assert.equal(rendered[0].preserveAspectRatio, 'none');
  }
});

test('big image resolves its own ID, accepts snake-case frame metadata, and does not mutate inputs', () => {
  const input = {node: {...node}, surface: structuredClone(surface), vars: {ox: 9, oy: 11},
    images: [{id: 'map', dataUrl: 'data:image/png;base64,AAAA'},
      {id: 'big:map', dataUrl: source, frame_width: 200, frame_height: 100}]};
  const before = structuredClone(input), image = elements(render(input), 'image')[0];
  assert.equal(image.href, source); assert.deepEqual(geometry(image), {x: 21, y: 39, width: 200, height: 100});
  assert.deepEqual(input, before);
});

function unavailable(props) {
  const html = render(props);
  assert.equal(elements(html, 'image').length, 0);
  assert.equal(elements(html, 'video').length, 0);
  assert.match(html, /预览不可用/);
  assert.deepEqual(geometry(elements(html, 'rect')[0]), {x: props.node.x, y: props.node.y, width: props.node.w, height: props.node.h});
}
test('big image rejects external/non-PNG sources, missing IDs, invalid sizes and frame-size mismatches', () => {
  for (const badSource of ['https://example.invalid/image.png', 'file:///private/example.png',
    'data:image/jpeg;base64,AAAA', 'data:image/png;base64,not base64!', '', undefined]) {
    unavailable({node, surface, images: [{...images[0], dataUrl: badSource}]});
  }
  unavailable({node, surface, images: [{...images[0], id: 'map'}]});
  unavailable({node, surface: {}, images});
  for (const [width, height] of [[79, 100], [200, 39], [1281, 100], [200, 961], [200.5, 100], [200, 100.5]]) {
    unavailable({node, surface: {big_images: [{id: 'map', width, height}]}, images});
  }
  for (const patch of [{frameWidth: 199}, {frameHeight: 99}, {frameWidth: undefined}]) {
    unavailable({node, surface, images: [{...images[0], ...patch}]});
  }
});

const clip = {type: 'clip', asset: 'intro', x: 4, y: 6, w: 120, h: 80, color: 0};
// Shape-only MP4 fixture: SSR does not decode bytes, run effects or play media.
const mp4 = 'data:video/mp4;base64,AAAAHGZ0eXBpc29t';
test('inactive clip never mounts video/foreignObject or exposes media src even with an available asset', () => {
  for (const active of [false, undefined]) {
    const html = render({node: clip, surface: {}, images: [{id: 'clip:intro', dataUrl: mp4}], ...(active === undefined ? {} : {active})});
    assert.equal(elements(html, 'video').length, 0);
    assert.equal(elements(html, 'foreignObject').length, 0);
    assert.equal(elements(html, 'text').length, 0);
    assert.ok(!html.includes(mp4)); assert.doesNotMatch(html, /\bsrc=/);
  }
});

test('active clip rejects external, blob, file, wrong-MIME and malformed data URLs without a video/src', () => {
  for (const dataUrl of ['https://example.invalid/intro.mp4', '//example.invalid/intro.mp4',
    'blob:https://example.invalid/object', 'file:///private/intro.mp4', 'javascript:alert(1)',
    'data:video/webm;base64,AAAA', 'data:video/mp4;base64,not base64!', 'data:video/mp4;base64,', '', undefined]) {
    const html = render({node: clip, surface: {}, active: true, images: [{id: 'clip:intro', dataUrl}]});
    assert.match(html, /预览不可用/);
    assert.equal(elements(html, 'video').length, 0);
    assert.equal(elements(html, 'foreignObject').length, 0);
    assert.doesNotMatch(html, /\bsrc=/);
  }
});

test('valid active clip renders only a muted inline preload-none video; source assignment remains effect-owned', () => {
  const html = render({node: clip, surface: {}, active: true, images: [{id: 'clip:intro', dataUrl: mp4}]});
  assert.deepEqual(geometry(elements(html, 'foreignObject')[0]), {x: 4, y: 6, width: 120, height: 80});
  const videos = elements(html, 'video'); assert.equal(videos.length, 1);
  assert.equal(videos[0].preload, 'none'); assert.equal(videos[0]['aria-hidden'], 'true');
  assert.equal(videos[0].tabindex, '-1'); assert.ok('muted' in videos[0]); assert.ok('playsinline' in videos[0]);
  assert.ok(!('controls' in videos[0])); assert.ok(!('src' in videos[0])); assert.ok(!html.includes(mp4));
});

test('clip cannot consume a differently namespaced or unrelated asset', () => {
  for (const id of ['intro', 'big:intro', 'clip:other']) {
    const html = render({node: clip, surface: {}, active: true, images: [{id, dataUrl: mp4}]});
    assert.match(html, /预览不可用/); assert.equal(elements(html, 'video').length, 0);
  }
});

test('unrelated media node types do not mount content', () => {
  assert.equal(render({node: {type: 'rect', x: 0, y: 0, w: 20, h: 20}, surface: {}, images: []}), '');
});
