import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {validateClawpkgManifest} from '../lib/clawpkg-contract.js';
import {initialApp} from './app-runtime.js';
import {dSnapshot,dSurface,flowLines} from './d-surface.js';
import {initialApp as mirroredInitial} from '../../skills/petui/scripts/app-runtime.mjs';
import {flowLines as mirroredFlow,dSnapshot as mirroredSnapshot} from '../../skills/petui/scripts/d-surface.mjs';
const read=n=>JSON.parse(readFileSync(new URL(`../../skills/petui/contracts/${n}.json`,import.meta.url)));
const suite=read('phase2-pc-contracts-v1');
function runtime(c){const r=structuredClone(suite.bases[c.base]);for(const p of c.patches){let at=r;for(const k of p.path.slice(0,-1))at=at[k];at[p.path.at(-1)]=p.value;}return r;}
function manifest(r,base){return {...structuredClone(suite.packages[base]),'runtime/widget.json':r,'assets/':[],'runtime/':[]};}
for(const c of suite.cases)test(`complete manifest ${c.name}`,()=>{
 const r=runtime(c),report=validateClawpkgManifest(manifest(r,c.base));assert.equal(report.valid,c.ok,report.errors.join('\n'));
 if(c.base.startsWith('app-form')||c.name.includes('derived')||c.name==='derived-program-output'){
  for(const init of [initialApp,mirroredInitial])if(c.ok)assert.doesNotThrow(()=>init(r));else assert.throws(()=>init(r));
 }
});
for(const c of read('longtext-v1').flow)test(`byte-bounded flow ${c.name||c.text.slice(0,15)}`,()=>{
 for(const flow of [flowLines,mirroredFlow]){const rows=flow(c.text,c.width,c.font_size);assert.deepEqual(rows,c.lines);assert.ok(rows.every(s=>new TextEncoder().encode(s).length<=120));}
});
test('first projection and each page preserve all UTF8 characters without editing input',()=>{
 const r=structuredClone(suite.bases.longtext);r.surface.texts[0].text='á'.repeat(120);r.surface.elements[0].h=20;
 const s=initialApp(r),before=structuredClone(s);for(const snap of [dSnapshot,mirroredSnapshot])assert.equal(snap(r,s).vars.pages,2);
 const collected=[];for(let page=0;page<2;page++){const state={...s,vars:{...s.vars,page}};collected.push(...dSurface(r,state).elements.map(n=>n.text));}assert.equal(collected.join(''),'á'.repeat(120));assert.deepEqual(s,before);
});
test('render projection rejects a conflicting derived writer even without initialApp',()=>{
 const r=runtime(suite.cases.find(c=>c.name==='derived-program-output'));for(const snap of [dSnapshot,mirroredSnapshot])assert.throws(()=>snap(r,{vars:{count:0,readPage:0}}));
});
