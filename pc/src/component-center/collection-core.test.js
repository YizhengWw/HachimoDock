import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createCollectionPreview} from './collection-core.js';
const rows=[{key:'a',label:'A'},{key:'b',label:'B'},{key:'c',label:'C'}];
test('collection product executes the same shared keyed-model contract as the PC service and native C',()=>{
 const suite=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/collection-model-v1.json',import.meta.url)));
 let p=createCollectionPreview(suite.rows,{pageSize:suite.pageSize,state:suite.initialState});
 for(const step of suite.steps){
  const saved=p.snapshot(),old=p.status();let next=p;
  if(step.error==='bad_data'){assert.throws(()=>p.withRows(step.rows));assert.deepEqual(p.snapshot(),saved);assert.deepEqual(p.status(),old);continue;}
  if(step.rows)next=p.withRows(step.rows);
  if(step.on!=='status'){
   const okay=next.action(step.on);if(step.error==='no_row'){assert.equal(okay,false);assert.deepEqual(next.snapshot(),saved);assert.deepEqual(next.status(),old);continue;}
   assert.equal(okay,true);
  }
  p=next;const view=p.status();assert.deepEqual({state:p.snapshot(),offset:view.offset,selectedIndex:view.selectedIndex,labels:view.rows.map(r=>r.label)},step.expected);
 }
});
test('collection fixed native core retains stable selection on reordered and filtered rows',()=>{
 const p=createCollectionPreview(rows,{pageSize:2});
 assert.equal(p.action('row_1'),true);assert.deepEqual(p.snapshot(),{offset:0,selected:'b'});
 const next=p.withRows([rows[2],rows[0],rows[1]]);assert.equal(next.status().selectedIndex,2);
 assert.equal(next.status().selectedLabel,'B');assert.equal(p.status().selectedIndex,1);
 const hidden=next.withRows([rows[0]]);assert.equal(hidden.status().selectedIndex,-1);
 assert.equal(hidden.status().selectionKey,'b');assert.deepEqual(hidden.snapshot(),next.snapshot());
 assert.equal(hidden.withRows(rows).status().selectedLabel,'B');
});
test('collection pages/empty slots are bounded and missing rows never select another record',()=>{
 const p=createCollectionPreview(rows,{pageSize:2});assert.equal(p.action('page_next'),true);
 const saved=p.snapshot(),visible=p.status();assert.equal(visible.offset,2);assert.equal(visible.rows[1].present,false);
 assert.equal(visible.rows[1].key,'');assert.equal(visible.rows[1].index,-1);
 assert.equal(p.action('row_1'),false);assert.deepEqual(p.snapshot(),saved);assert.deepEqual(p.status(),visible);
 const empty=p.withRows([]);assert.equal(empty.status().pages,0);assert.equal(empty.action('down'),false);
 assert.equal(empty.action('row_0'),false);assert.equal(empty.status().canUp,false);assert.deepEqual(empty.snapshot(),saved);
});
test('status clamps rendered offset without silently changing saved state',()=>{
 const p=createCollectionPreview(rows,{pageSize:2,state:{offset:127,selected:'missing'}});
 assert.equal(p.status().offset,2);assert.deepEqual(p.snapshot(),{offset:127,selected:'missing'});
 assert.equal(p.action('page_previous'),true);assert.deepEqual(p.snapshot(),{offset:0,selected:'missing'});
});
test('collection UTF-8 truncates display on scalar boundaries but preserves IDs and source text',()=>{
 const source=[{key:'猫咪',label:'🐱'.repeat(128),detail:'字'.repeat(170)}];
 const p=createCollectionPreview(source,{pageSize:1});assert.equal(p.action('row_0'),true);
 assert.equal(p.status().rows[0].label,'🐱'.repeat(15));assert.equal(p.status().rows[0].detail,'字'.repeat(21));
 assert.equal(p.status().rows[0].truncated,true);assert.equal(source[0].label.length,256);
 assert.equal(p.snapshot().selected,'猫咪');
});
test('bad collection replacement preserves the prior instance; native validation rejects duplicate stable IDs',()=>{
 const p=createCollectionPreview(rows,{pageSize:2});p.action('row_1');const state=p.snapshot(),view=p.status();
 for(const replacement of [[rows[0],rows[0]],[{key:'',label:'x'}],[{key:'a',label:'x',detail:null}],
  [{key:'a',label:'x\n'}],[{key:'a',label:'x\u0085'}],[{key:'a',label:'\ud800'}],Array(129).fill(rows[0]),new Array(1)]){
  assert.throws(()=>p.withRows(replacement),/Invalid bounded collection/);assert.deepEqual(p.snapshot(),state);assert.deepEqual(p.status(),view);
 }
 for(const bad of [{offset:128,selected:null},{offset:0,selected:''},{offset:0,selected:'x'.repeat(64)},{offset:0,selected:null,extra:0}])assert.throws(()=>createCollectionPreview(rows,{state:bad}));
 assert.throws(()=>p.action('remove'));assert.throws(()=>p.action('status'));
});
test('collection product artifact has no imports/grow and raw ABI cannot retry a poisoned load',()=>{
 const binary=fs.readFileSync(new URL('./collection-core.wasm',import.meta.url)),m=new WebAssembly.Module(binary);
 assert.deepEqual(WebAssembly.Module.imports(m),[]);const e=new WebAssembly.Instance(m,{}).exports;
 assert.deepEqual(WebAssembly.Module.exports(m).map(item=>item.name).sort(),[
  'memory','cv_buffer','cv_seed_key','cv_stride','cv_load','cv_apply','cv_state_offset','cv_state_key',
  'cv_text','cv_metric','cv_selected_index','cv_row_text','cv_row_metric'].sort());
 assert.equal(e.pet_collection_project_validated,undefined);assert.equal(e.pet_collection_host_publish,undefined);
 assert.equal(e.memory.buffer.byteLength,262144);assert.throws(()=>e.memory.grow(1),RangeError);assert.equal(e.cv_stride(),1090);
 assert.equal(e.cv_load(129,4,0),1);assert.equal(e.cv_load(0,4,0),1);assert.equal(e.cv_apply(0),1);
 const other=new WebAssembly.Instance(m,{}).exports;assert.equal(other.cv_load(0,4,0),0);
 assert.equal(other.cv_apply(256),1);assert.equal(other.cv_state_offset(),0);
 const row=new WebAssembly.Instance(m,{}).exports,bytes=new Uint8Array(row.memory.buffer);
 bytes.fill(65,row.cv_buffer(),row.cv_buffer()+64);assert.equal(row.cv_load(1,4,0),2);
});
test('collection same native model matches an independent 1000-action stable-key reference',()=>{
 const source=Array.from({length:128},(_,i)=>({key:`k${i}`,label:`L${i}`}));let p=createCollectionPreview(source,{pageSize:5});
 let offset=0,selected=null,seed=7,accepted=0,rejected=0;
 for(let i=0;i<1000;i++){
  seed=(Math.imul(seed,1664525)+1013904223)>>>0;const count=(seed>>>16)%129;
  seed=(Math.imul(seed,1664525)+1013904223)>>>0;const rotation=(seed>>>16)%128;
  const data=source.map((_,j)=>source[(j+rotation)%128]).slice(0,count);p=p.withRows(data);
  const last=Math.floor(Math.max(0,count-1)/5)*5;let shown=Math.floor(Math.min(offset,last)/5)*5;
  seed=(Math.imul(seed,1664525)+1013904223)>>>0;const op=(seed>>>16)%13;
  const operation=['page_next','page_previous','up','down','clear_selection',...Array.from({length:8},(_,i)=>`row_${i}`)][op];
  let next=selected,okay=true;const index=data.findIndex(r=>r.key===selected);
  if(op===0)shown=Math.min(shown+5,last);else if(op===1)shown=Math.max(0,shown-5);
  else if(op===4)next=null;
  else if(op===2||op===3){if(!count)okay=false;else{const at=index<0?0:op===2?Math.max(0,index-1):Math.min(count-1,index+1);next=data[at].key;shown=Math.floor(at/5)*5;}}
  else{const row=op-5;if(row>=5||shown+row>=count)okay=false;else next=data[shown+row].key;}
  assert.equal(p.action(operation),okay);if(okay){offset=shown;selected=next;accepted++;}else rejected++;
  assert.deepEqual(p.snapshot(),{offset,selected});const view=p.status();
  assert.equal(view.selectedIndex,data.findIndex(r=>r.key===selected));assert.equal(view.offset,Math.floor(Math.min(offset,last)/5)*5);
 }
 assert.ok(accepted>300&&rejected>100);
});
test('read-only collection views do not rerun immutable source validation or expose writable native state',()=>{
 const original=WebAssembly.Instance,calls=[];
 try{
  WebAssembly.Instance=function(...args){
   const instance=Reflect.construct(original,args),exports={...instance.exports},apply=exports.cv_apply;
   exports.cv_apply=operation=>{calls.push(operation);return apply(operation);};
   return {exports};
  };
  const source=rows.map(row=>({...row})),p=createCollectionPreview(source,{pageSize:2});
  const shown=p.status();shown.rows[0].label='caller mutation';source[0].label='source mutation';
  const saved=p.snapshot();saved.offset=127;
  for(let i=0;i<50;i++){
   assert.equal(p.status().rows[0].label,'A');assert.deepEqual(p.snapshot(),{offset:0,selected:null});
  }
  assert.deepEqual(calls,[]);
  assert.equal(p.action('row_1'),true);assert.deepEqual(calls,[7]);
  for(let i=0;i<50;i++)assert.equal(p.status().selectedLabel,'B');
  const next=p.withRows([rows[1],rows[0]]);assert.equal(next.status().selectedIndex,0);
  assert.equal(next.status().selectedLabel,'B');assert.equal(p.status().selectedIndex,1);assert.deepEqual(calls,[7]);
 }finally{WebAssembly.Instance=original;}
});
