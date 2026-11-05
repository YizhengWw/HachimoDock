import test from 'node:test';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import {readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';

const require = createRequire(import.meta.url);
const React = require('react');
const {renderToStaticMarkup} = require('react-dom/server');
const {buildSync} = require('esbuild');
const module = {exports: {}};
const bundle = buildSync({entryPoints: [fileURLToPath(new URL('./GlobalAudioCuesPanel.jsx', import.meta.url))],
  bundle: true, write: false, platform: 'node', format: 'cjs', external: ['react'], logLevel: 'silent'});
runInNewContext(bundle.outputFiles[0].text, {module, exports: module.exports, URL, TextEncoder,
  require: name => {assert.equal(name, 'react'); return React;}}, {filename: 'GlobalAudioCuesPanel.actual.cjs'});
const Panel = module.exports.default;
const selected = props => {
  const html = renderToStaticMarkup(React.createElement(Panel, props));
  return [...html.matchAll(/<option\b([^>]*)>/g)]
    .filter(([, attrs]) => /\bselected=""/.test(attrs))
    .map(([, attrs]) => attrs.match(/\bvalue="([^"]+)"/)[1]);
};

test('appearance sound controls initially select the actual action, including dotted idle and touch states', () => {
  for (const currentFamily of ['welcome', 'working', 'done', 'waiting_user', 'error',
    'idle', 'idle.default', 'idle.playing', 'touch.lick', 'permission_request']) {
    assert.deepEqual(selected({currentFamily}), [currentFamily]);
  }
});

test('standalone global controls retain their default without an appearance context', () => {
  for (const currentFamily of [undefined, '', 'unknown']) assert.deepEqual(selected({currentFamily}), ['done']);
});

test('appearance detail supplies the selected action to the global controls', () => {
  const source = readFileSync(new URL('./AppearanceDetail.jsx', import.meta.url), 'utf8');
  assert.match(source, /<GlobalAudioCuesPanel\b[^>]*currentFamily=\{activeFamily\}/);
});
