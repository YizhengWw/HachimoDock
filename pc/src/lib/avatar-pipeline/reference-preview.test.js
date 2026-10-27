import test from 'node:test';
import assert from 'node:assert/strict';
import {createReferencePreviewController,normalizeStyleDescription} from './reference-preview.js';
import {buildReferenceEditPrompt,BLACK_BACKGROUND_EDIT_PROMPT,editImageBackgroundWithArk} from './providers/volcano-image.js';
import {reviewVisual} from './quality.js';

test('optional style uses default when blank; custom style reaches image request and quality check',async()=>{
 assert.equal(buildReferenceEditPrompt('  '),BLACK_BACKGROUND_EDIT_PROMPT);
 const style='深肤色、正常头身比例、水彩风格';
 const prompt=buildReferenceEditPrompt(style);
 assert.match(prompt,/深肤色/);assert.doesNotMatch(prompt,/白皙、明亮自然的白皮肤|约 2.5 到 3 头身/);
 assert.equal(normalizeStyleDescription('x'.repeat(1200)).length,1000);
 const old=globalThis.fetch;const bodies=[];
 globalThis.fetch=async(_url,init)=>{bodies.push(JSON.parse(init.body));return Response.json(bodies.length===1?{data:[{b64_json:'AQID'}]}:{output_text:'{"pass":true,"issues":[]}'});};
 try{
  await editImageBackgroundWithArk({apiKey:'fixture',imageDataUrl:'data:image/png;base64,AA==',styleDescription:style});
  assert.equal(bodies[0].prompt,prompt);
  await reviewVisual({thinking:{apiKey:'fixture'},images:['data:image/png;base64,AA=='],kind:'reference',styleDescription:style});
  const text=bodies[1].input[0].content.map(c=>c.text||'').join('\n');
  assert.match(text,/深肤色、正常头身比例、水彩风格/);assert.match(text,/自定义风格优先/);
 }finally{globalThis.fetch=old;}
});

test('double click sends once; editing/cancelling rejects a stale preview and permits a fresh request',async()=>{
 let resolve,calls=0;const states=[];const file=new File(['image'],'fixture.png');
 const controller=createReferencePreviewController(s=>states.push(s),async()=>{calls++;return new Promise(r=>{resolve=r;});});
 const first=controller.run({imageFile:file,styleDescription:'Q版'});
 await controller.run({imageFile:file,styleDescription:'Q版'});assert.equal(calls,1);
 controller.invalidate();resolve({bytes:new Uint8Array([1]),dataUrl:'old'});await first;
 assert.equal(states.at(-1).result,null);assert.equal(states.at(-1).busy,false);
 const next=controller.run({imageFile:file,styleDescription:' 水彩 '});resolve({bytes:new Uint8Array([2]),dataUrl:'new'});await next;
 assert.equal(calls,2);assert.equal(states.at(-1).result.style,'水彩');assert.equal(states.at(-1).result.source,file);
});

test('failed preview clears busy and waits for a new explicit click',async()=>{
 let calls=0;const states=[];const controller=createReferencePreviewController(s=>states.push(s),async()=>{calls++;throw Error('fixture failure');});
 await controller.run({});assert.equal(calls,1);assert.equal(states.at(-1).error,'fixture failure');assert.equal(states.at(-1).busy,false);
});
