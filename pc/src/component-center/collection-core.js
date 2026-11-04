/** Offline authoring model, not an installed device capability or record store.
 * Source rows are projected text only; full records remain with the parent.
 * Parent input routing still needs current dataset/UI identity and grants. */
import {encodedCollectionCore} from './collection-bytes.mjs';
const OPERATIONS=Object.freeze(['status','page_next','page_previous','up','down','clear_selection',...Array.from({length:8},(_,i)=>`row_${i}`)]);
const dense=(a,max)=>Array.isArray(a)&&a.length<=max&&Object.keys(a).length===a.length&&Array.from({length:a.length},(_,i)=>Object.hasOwn(a,i)).every(Boolean);
const exact=(o,keys)=>o&&typeof o==='object'&&!Array.isArray(o)&&Object.keys(o).length===keys.length&&keys.every(k=>Object.hasOwn(o,k));
const fail=()=>{throw Error('Invalid bounded collection model');};
const encoder=new TextEncoder(),decoder=new TextDecoder('utf-8',{fatal:true});
const EXPORTS=['memory','cv_buffer','cv_seed_key','cv_stride','cv_load','cv_apply','cv_state_offset','cv_state_key',
 'cv_text','cv_metric','cv_selected_index','cv_row_text','cv_row_metric'].sort();
function encoded(s,max,nonempty=false){
 if(typeof s!=='string'||s.length>max||(nonempty&&!s)||/[\u0000-\u001f\u007f-\u009f]/u.test(s)||Array.from(s).some(c=>{const n=c.codePointAt(0);return n>=0xd800&&n<=0xdfff;}))fail();
 const bytes=encoder.encode(s);if(bytes.length>max)fail();return bytes;
}
let module;
export function createCollectionPreview(rows,{pageSize=4,state={offset:0,selected:null}}={}){
 if(!dense(rows,128)||!Number.isInteger(pageSize)||pageSize<1||pageSize>8||!exact(state,['offset','selected'])||!Number.isInteger(state.offset)||state.offset<0||state.offset>127)fail();
 const selected=state.selected===null?new Uint8Array():encoded(state.selected,63,true);
 const prepared=rows.map(row=>{
  if(!exact(row,['key','label'])&&!exact(row,['key','label','detail']))fail();
  return [encoded(row.key,63,true),encoded(row.label,512),encoded(Object.hasOwn(row,'detail')?row.detail:'',512)];
 });
 if(!module){module=new WebAssembly.Module(Uint8Array.from(atob(encodedCollectionCore),c=>c.charCodeAt(0)));if(WebAssembly.Module.imports(module).length||JSON.stringify(WebAssembly.Module.exports(module).map(item=>item.name).sort())!==JSON.stringify(EXPORTS))fail();}
 const e=new WebAssembly.Instance(module,{}).exports;if(e.memory.buffer.byteLength!==262144||e.cv_stride()!==1090)fail();
 const memory=new Uint8Array(e.memory.buffer),start=e.cv_buffer();
 prepared.forEach((row,i)=>row.forEach((bytes,field)=>memory.set(bytes,start+i*1090+[0,64,577][field])));
 memory.set(selected,e.cv_seed_key());if(e.cv_load(prepared.length,pageSize,state.offset)!==0)fail();
 const text=pointer=>{let length=0;while(length<64&&memory[pointer+length])length++;if(length===64)fail();return decoder.decode(memory.subarray(pointer,pointer+length));};
 const snapshot=()=>({offset:e.cv_state_offset(),selected:text(e.cv_state_key())||null});
 const status=()=>{
  // cv_load and each explicit action already published a complete view. Source
  // bytes are privately copied/immutable in this instance; a read must not
  // rescan all records. Dataset replacement validates a NEW instance instead.
  return {rows:Array.from({length:pageSize},(_,i)=>({present:!!e.cv_row_metric(i,0),index:e.cv_row_metric(i,1),selected:!!e.cv_row_metric(i,2),truncated:!!e.cv_row_metric(i,3),key:text(e.cv_row_text(i,0)),label:text(e.cv_row_text(i,1)),detail:text(e.cv_row_text(i,2))})),
   count:e.cv_metric(0),offset:e.cv_metric(1),page:e.cv_metric(2),pages:e.cv_metric(3),selectionKey:text(e.cv_text(0))||null,
   selectedLabel:text(e.cv_text(1)),selectedIndex:e.cv_selected_index(),canPrevious:!!e.cv_metric(5),canNext:!!e.cv_metric(6),canUp:!!e.cv_metric(7),canDown:!!e.cv_metric(8)};
 };
 return Object.freeze({snapshot,status,
  action(operation){const index=OPERATIONS.indexOf(operation);if(index<1)fail();const result=e.cv_apply(index);if(result===4)return false;if(result!==0)fail();return true;},
  withRows(next){return createCollectionPreview(next,{pageSize,state:snapshot()});},
 });
}
