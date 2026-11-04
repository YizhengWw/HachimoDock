import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {deviceJsonStringsNoNul,deviceJsonStringsValidText,deviceJsonWithinDepth,DEVICE_JSON_MAX_DEPTH} from './device-json.js';
import {deviceJsonStringsNoNul as mirror} from '../../skills/petui/scripts/device-json.mjs';
import {deviceJsonStringsValidText as textMirror} from '../../skills/petui/scripts/device-json.mjs';
import {deviceJsonWithinDepth as depthMirror} from '../../skills/petui/scripts/device-json.mjs';
import {initialApp} from './app-runtime.js';
import {initialApp as initialMirror} from '../../skills/petui/scripts/app-runtime.mjs';
const suite=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/device-json-strings-v1.json',import.meta.url)));
const unicode=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/device-json-unicode-v1.json',import.meta.url)));
const depth=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/device-json-depth-v1.json',import.meta.url)));
test('container depth agrees across actual model mirrors without alias/cycle shortcuts',()=>{
  assert.equal(DEVICE_JSON_MAX_DEPTH,depth.maximumContainerDepth);
  for(const c of depth.cases){const value=JSON.parse(c.source),before=JSON.stringify(value);
    assert.equal(deviceJsonWithinDepth(value),c.allowed,c.name);assert.equal(depthMirror(value),c.allowed,c.name);
    assert.equal(JSON.stringify(value),before);
  }
  const shared={v:0};let deep=shared;for(let i=0;i<31;i++)deep=[deep];
  assert(deviceJsonWithinDepth(deep));assert(!deviceJsonWithinDepth({first:shared,deeper:deep}));
  const cycle=[];cycle.push(cycle);assert(!deviceJsonWithinDepth(cycle));
  let graph={};for(let i=0;i<30;i++)graph={left:graph,right:graph};assert(deviceJsonWithinDepth(graph));
});
test('preview refuses over-depth data before initializing or changing existing inputs',()=>{
  let deep=0;for(let i=0;i<32;i++)deep=[deep];
  const runtime={initial_state:'idle',states:['idle'],vars:{},transitions:[],tick:[],extra:deep};
  const before=JSON.stringify(runtime);
  for(const initial of [initialApp,initialMirror])assert.throws(()=>initial(runtime),/depth budget/);
  assert.equal(JSON.stringify(runtime),before);
});
test('decoded Unicode scalars and raw text agree with the shared native contract',()=>{
  for(const c of unicode.cases){
    let valid=false;try{valid=deviceJsonStringsValidText(JSON.parse(c.source));}catch{}
    assert.equal(valid,c.allowed,c.name);
    let mirrorValid=false;try{mirrorValid=textMirror(JSON.parse(c.source));}catch{}
    assert.equal(mirrorValid,c.allowed,c.name);
  }
  const decoder=new TextDecoder('utf-8',{fatal:true});
  for(const c of unicode.byteCases){
    let valid=false;try{valid=deviceJsonStringsValidText(JSON.parse(decoder.decode(Buffer.from(c.hex,'hex'))));}catch{}
    assert.equal(valid,c.allowed,c.name);
  }
});
test('unpaired UTF-16 is refused before preview state exists, paired text is preserved',()=>{
  const base={initial_state:'idle',states:['idle'],vars:{label:{type:'string',init:''}},transitions:[],tick:[]};
  for(const initial of [initialApp,initialMirror]){
    for(const value of ['\ud800','\udfff','okay\ud800']){
      const runtime=structuredClone(base);runtime.vars.label.init=value;const before=JSON.stringify(runtime);
      assert.throws(()=>initial(runtime),/Unicode/);assert.equal(JSON.stringify(runtime),before);
    }
    const runtime=structuredClone(base);runtime.vars.label.init='🐾';assert.equal(initial(runtime).vars.label,'🐾');
  }
  let seed=12345;
  for(let i=0;i<10000;i++){
    seed=(Math.imul(seed,1664525)+1013904223)>>>0;const scalar=1+seed%0x10ffff;
    if(scalar<0xd800||scalar>0xdfff)assert(deviceJsonStringsValidText({v:String.fromCodePoint(scalar)}));
    assert(!deviceJsonStringsValidText({[String.fromCharCode(0xd800+i%2048)]:'value'}));
  }
});
test('device strings agree with decoded shared vectors without coercion or mutation',()=>{
  for(const c of suite.cases){
    const before=JSON.stringify(c.value);
    assert.equal(deviceJsonStringsNoNul(c.value),c.allowed,c.name);
    assert.equal(mirror(c.value),c.allowed,c.name);
    assert.equal(JSON.stringify(c.value),before);
  }
  for(const c of suite.rawCases)assert.equal(deviceJsonStringsNoNul(JSON.parse(c.json)),c.allowed);
});
test('preview refuses ambiguous strings before publishing any initialized model',()=>{
  const base={initial_state:'idle',states:['idle'],vars:{label:{type:'string',init:''}},transitions:[],tick:[]};
  for(const initial of [initialApp,initialMirror]){
    for(const value of ['A\0B','\0','end\0']){
      const runtime=structuredClone(base);runtime.vars.label.init=value;
      const before=JSON.stringify(runtime);
      assert.throws(()=>initial(runtime),/cannot contain NUL/);
      assert.equal(JSON.stringify(runtime),before);
    }
    const runtime=structuredClone(base);runtime.vars.label.init='A\\u0000B';
    assert.equal(initial(runtime).vars.label,'A\\u0000B');
  }
});
test('repeat references do not become a recursion failure or hide a NUL leaf',()=>{
  const shared={text:'okay'},value={a:shared,b:shared};
  assert.equal(deviceJsonStringsNoNul(value),true);
  shared.text='not\0okay';assert.equal(deviceJsonStringsNoNul(value),false);
});
test('product and skill representation utility are exact mechanical mirrors',()=>{
  assert.equal(fs.readFileSync(new URL('./device-json.js',import.meta.url),'utf8'),
    fs.readFileSync(new URL('../../skills/petui/scripts/device-json.mjs',import.meta.url),'utf8'));
});
