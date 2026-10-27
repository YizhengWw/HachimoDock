import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {validateClawpkgManifest} from '../lib/clawpkg-contract.js';

const directory = new URL('../../builtin-clawpkgs/fresh-fruit-slice/', import.meta.url);
const manifest = Object.fromEntries(['component.json','buttons.json','negative-screen.json','share.json','runtime/widget.json'].map(name=>[name,JSON.parse(readFileSync(new URL(name,directory)))]));
manifest['assets/'] = {}; manifest['runtime/'] = {};

test('formal touch package satisfies the frontend contract',()=>{
  assert.deepEqual(validateClawpkgManifest(manifest).errors,[]);
});
test('touch scene rejects invalid bounds, executable/extra fields and mismatched kind',()=>{
  for (const mutate of [
    m=>m['component.json'].kind='tool',
    m=>m['runtime/widget.json'].scene.duration_sec=121,
    m=>m['runtime/widget.json'].scene.revision=3,
    m=>m['runtime/widget.json'].scene.duration_sec=NaN,
    m=>m['runtime/widget.json'].scene.lives=0,
    m=>m['runtime/widget.json'].scene.sound_volume=101,
    m=>m['runtime/widget.json'].scene.script='evil()',
    m=>m['runtime/widget.json'].scene.sprites.pop(),
    m=>m['runtime/widget.json'].transitions[0].effects=[],
    m=>m['runtime/widget.json'].vars={score:0},
  ]) {
    const copy=structuredClone(manifest);mutate(copy);
    assert.equal(validateClawpkgManifest(copy).valid,false);
  }
});
test('trusted preview module is bounded and import-free',async()=>{
  const bytes=readFileSync(new URL('./fruit-core.wasm',import.meta.url));
  const module=await WebAssembly.compile(bytes);
  assert.deepEqual(WebAssembly.Module.imports(module),[]);
  const instance=await WebAssembly.instantiate(module);
  assert.equal(instance.exports.memory.buffer.byteLength,131072);
  assert.throws(()=>instance.exports.memory.grow(1),RangeError);
});
