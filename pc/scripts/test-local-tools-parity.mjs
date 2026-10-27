import fs from 'node:fs';import os from 'node:os';import path from 'node:path';import {spawnSync} from 'node:child_process';import assert from 'node:assert/strict';
import {localInitial,localAdvance,localAction,localView,civilDay} from '../src/component-center/local-tools.js';
const root=path.resolve(import.meta.dirname,'../..'),tmp=fs.mkdtempSync(path.join(os.tmpdir(),'pet-local-parity-')),exe=path.join(tmp,'trace');
let cc=spawnSync('clang',['-std=c11','-fsanitize=address,undefined','-Wno-deprecated-declarations','-I'+root+'/firmware/main','-I'+root+'/firmware/tests/vendor/cjson',root+'/firmware/tests/local_tools_trace.c',root+'/firmware/main/pet_p4_local_tools.c',root+'/firmware/tests/vendor/cjson/cJSON.c','-lm','-o',exe],{encoding:'utf8'});assert.equal(cc.status,0,cc.stderr);
for(const id of ['decision-capsule','answer-book','hope-train']){
 const file=path.join(root,'pc/community-components/local-delights',id,'runtime/widget.json'),r=JSON.parse(fs.readFileSync(file));
 const run=spawnSync(exe,[file],{encoding:'utf8',maxBuffer:8*1024*1024});assert.equal(run.status,0,run.stderr);const rows=run.stdout.trim().split('\n').map(s=>JSON.parse(s));
 let s=localInitial({vars:Object.fromEntries(Object.entries(r.vars).map(([k,v])=>[k,v.init])),clock:0},-1),seed=0x12983746;
 rows.forEach((expected,i)=>{seed^=seed<<13;seed^=seed>>>17;seed^=seed<<5;seed>>>=0;s.clock+=seed%1200;s.wall=i<8?-1:civilDay(2026,10,23)*86400+17*3600;s=localAdvance(r,s);s=localAction(r,s,Object.keys(r.local.actions)[seed%Object.keys(r.local.actions).length],seed);const v=localView(r,s);
  const actual={phase:v.phase,field:s.local.field,draftDay:s.local.draftDay,draftMinute:s.local.draftMinute,pending:s.local.pending,headline:v.headline,note:v.note,metricValue:v.metricValue,progress:v.progress,vars:Array.from({length:7},(_,i)=>s.vars[`local${i}`])};assert.deepEqual(actual,expected,`${id} step ${i}`);
 });console.log(`${id}: ${rows.length} C/JS action+clock snapshots agree`);
}
