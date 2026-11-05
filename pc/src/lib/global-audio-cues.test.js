import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const original=readFileSync(new URL('./global-audio-cues.js',import.meta.url),'utf8');
async function fixture(t){
 const files=new Map(),calls=[];const storage=new Map();
 const old=Object.getOwnPropertyDescriptor(globalThis,'localStorage');
 Object.defineProperty(globalThis,'localStorage',{configurable:true,value:{getItem:k=>storage.get(k),setItem:(k,v)=>storage.set(k,v),removeItem:k=>storage.delete(k)}});
 t.after(()=>{if(old)Object.defineProperty(globalThis,'localStorage',old);else delete globalThis.localStorage;delete globalThis.__audioFsFixture;});
 const fs={BaseDirectory:{AppLocalData:1},exists:async p=>files.has(p),mkdir:async p=>calls.push(['mkdir',p]),readFile:async p=>new Uint8Array(files.get(p)),readTextFile:async p=>files.get(p),writeFile:async(p,b)=>{calls.push(['writeFile',p]);files.set(p,new Uint8Array(b));},writeTextFile:async(p,b)=>{calls.push(['writeTextFile',p]);files.set(p,b);},rename:async(a,b)=>{calls.push(['rename',a,b]);assert(files.has(a));files.set(b,files.get(a));files.delete(a);},remove:async p=>{calls.push(['remove',p]);files.delete(p);}};
 globalThis.__audioFsFixture=fs;
 const code=original.replace(/import \{([^}]+)\} from '@tauri-apps\/plugin-fs';/, 'const {$1}=globalThis.__audioFsFixture;').replace("'./avatar-pipeline/families.js'",JSON.stringify(new URL('./avatar-pipeline/families.js',import.meta.url).href));
 const module=await import(`data:text/javascript;base64,${Buffer.from(code).toString('base64')}#${Math.random()}`);
 return {files,calls,module};
}
const mapPath='global-audio-cues/audio-overrides.json';
const wav=new Uint8Array(46).fill(7);
test('upload/removal persist only global WAV and map, never video or appearance files',async t=>{
 const f=await fixture(t);f.files.set('custom-appearances/example/manifest.json','KEEP');
 await f.module.saveGlobalAudioCue('done',wav);
 assert.deepEqual(await f.module.readGlobalAudioCue('done'),wav);assert(f.module.globalAudioDirty());
 await f.module.saveGlobalAudioCue('welcome',wav);
 assert.deepEqual(Object.keys(JSON.parse(f.files.get(mapPath))),['done','welcome']);
 await f.module.removeGlobalAudioCue('done');assert.equal(await f.module.readGlobalAudioCue('done'),null);
 assert.equal(f.files.get('custom-appearances/example/manifest.json'),'KEEP');
 assert(f.calls.every(c=>c.slice(1).every(p=>p.startsWith('global-audio-cues'))));
 assert.doesNotMatch(original,/prepareP4Appearance|prepare_p4_appearance|invoke\(/);
});
test('migrates existing built-in cues byte exactly once and preserves originals',async t=>{
 const f=await fixture(t);const legacy='custom-appearances/builtin-terrier/audio-overrides';
 f.files.set(`${legacy}.json`,JSON.stringify({welcome:`${legacy}/welcome.wav`,done:`${legacy}/done.wav`}));
 f.files.set(`${legacy}/welcome.wav`,wav);f.files.set(`${legacy}/done.wav`,wav);
 await f.module.migrateBuiltinGlobalAudioCues();assert.deepEqual(await f.module.readGlobalAudioCue('welcome'),wav);
 const calls=f.calls.length;await f.module.migrateBuiltinGlobalAudioCues();assert.equal(f.calls.length,calls);
 assert.deepEqual(f.files.get(`${legacy}/welcome.wav`),wav);
});
test('invalid maps and state names fail before writing',async t=>{
 const f=await fixture(t);await assert.rejects(f.module.saveGlobalAudioCue('../secret',wav));assert.equal(f.calls.length,0);
 f.files.set(mapPath,JSON.stringify({done:'/outside.wav'}));await assert.rejects(f.module.saveGlobalAudioCue('done',wav));assert.equal(f.calls.length,0);
});

test('an older send acknowledgement cannot clear a newer sound edit',async t=>{
 const f=await fixture(t);await f.module.saveGlobalAudioCue('done',wav);
 const revision=f.module.globalAudioRevision();await f.module.saveGlobalAudioCue('error',wav);
 f.module.markGlobalAudioDirty(false,revision);assert(f.module.globalAudioDirty());
 f.module.markGlobalAudioDirty(false,f.module.globalAudioRevision());assert(!f.module.globalAudioDirty());
});

test('sound edits also work when a webview lacks randomUUID',async t=>{
 const previous=Object.getOwnPropertyDescriptor(globalThis,'crypto');
 Object.defineProperty(globalThis,'crypto',{configurable:true,value:{}});
 t.after(()=>{if(previous)Object.defineProperty(globalThis,'crypto',previous);else delete globalThis.crypto;});
 const f=await fixture(t);await f.module.saveGlobalAudioCue('done',wav);assert(f.module.globalAudioDirty());
});
