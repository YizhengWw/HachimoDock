import test from 'node:test';import assert from 'node:assert/strict';import{readFileSync}from'node:fs';
import{projectNode,hitNode,viewportOffset,viewportContext}from'./app-viewport.js';
import{appHit,initialApp,appAction,appAdvance}from'./app-runtime.js';
import * as mirror from '../../skills/petui/scripts/app-runtime.mjs';
const suite=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/app-viewport-v1.json',import.meta.url)));
test('packaged viewport model exactly matches the PC model',()=>{
 assert.equal(readFileSync(new URL('../../skills/petui/scripts/app-viewport.mjs',import.meta.url),'utf8'),readFileSync(new URL('./app-viewport.js',import.meta.url),'utf8'));
});
const multi=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/app-viewports-v1.json',import.meta.url)));
for(const [index,c]of multi.cases.entries())test(`independent viewports share C/Python vectors: ${index}`,()=>{
 assert.deepEqual(multi.surface.elements.map(n=>{const p=projectNode(multi.surface,n,c.vars);return p?[p.x,p.y]:null;}),c.positions);
 for(const [x,y,expected]of c.hits)assert.equal(appHit(multi.surface,0,x,y,c.vars),expected);
});
test('viewport tuple invalidates presses for either offset; unchanged value is stable',()=>{
 const before=viewportContext(multi.surface,{left_offset:20,right_offset:40});
 assert.equal(before,viewportContext(multi.surface,{right_offset:40,left_offset:20,unrelated:99}));
 assert.notEqual(before,viewportContext(multi.surface,{left_offset:21,right_offset:40}));
 assert.notEqual(before,viewportContext(multi.surface,{left_offset:20,right_offset:41}));
 assert.equal(viewportContext(multi.surface,{left_offset:999}),viewportContext(multi.surface,{left_offset:1000}));
});
test('batched scroll away/back and tick changes keep a transient pointer revision',()=>{
 const runtime={states:['ready'],initial_state:'ready',vars:{left_offset:{type:'int',init:0},right_offset:{type:'int',init:0},unrelated:{type:'int',init:0}},surface:multi.surface,input:{engine:'p4-input-v1'},transitions:[{from:'*',on:'down',inc:{left_offset:120}},{from:'*',on:'up',inc:{left_offset:-120}},{from:'*',on:'unrelated',inc:{unrelated:1}}],tick:[]};
 let state=initialApp(runtime),other=mirror.initialApp(runtime);
 for(const action of ['down','up']){state=appAction(runtime,state,action);other=mirror.appAction(runtime,other,action);assert.deepEqual(state,other);}
 assert.equal(state.vars.left_offset,0);assert.equal(state.pointerEpoch,2);
 state=appAction(runtime,state,'unrelated');assert.equal(state.pointerEpoch,2,'unrelated values do not spuriously cancel clicks');
 const ticks={...runtime,transitions:[],tick:[{every_ms:100,inc:{left_offset:120}},{every_ms:100,inc:{left_offset:-120}}]};
 const after=appAdvance(ticks,initialApp(ticks),100);assert.equal(after.vars.left_offset,0);assert.equal(after.pointerEpoch,2);assert.deepEqual(after,mirror.appAdvance(ticks,mirror.initialApp(ticks),100));
 const clamped={...runtime,vars:{...runtime.vars,left_offset:{type:'int',init:400}}};
 assert.equal(appAction(clamped,initialApp(clamped),'down').pointerEpoch,undefined,'unchanged projected clamp is not a geometry change');
});
test('wrong-page, hidden and out-of-clip content cannot receive a hit',()=>{
 const surface=structuredClone(multi.surface),vars={left_offset:120,right_offset:200};
 surface.elements[0].page=1;assert.equal(appHit(surface,0,40,250,vars),null);
 assert.equal(appHit(surface,1,40,250,vars),'left.row');
 surface.elements[0].show={var:'visible',min:1,max:1};assert.equal(appHit(surface,1,40,250,vars),null);
 assert.equal(appHit(surface,1,40,250,{...vars,visible:1}),'left.row');
 surface.elements[0].view='absent';assert.equal(projectNode(surface,surface.elements[0],vars),null);
});
for(const c of suite.cases)test(`viewport projection and clipped hit: ${c.scroll}`,()=>{
 const vars={scroll:c.scroll},n=projectNode(suite.surface,suite.node,vars);
 assert.deepEqual(n?[n.x,n.y]:null,c.position);
 for(const [x,y,expected]of[c.hit,c.outside])assert.equal(hitNode(suite.surface,suite.node,vars,x,y),expected);
});
test('fixed header is unaffected; scroll can reveal a previously unreachable action',()=>{
 const fixed={...suite.node,in_view:false,action:'fixed'},row={...suite.node,y:500,action:'row'};
 const surface={...suite.surface,elements:[fixed,row]};assert.equal(appHit(surface,0,60,190,{scroll:380}),'row');
 assert.equal(appHit(surface,0,60,190,{scroll:0}),null);assert.deepEqual(projectNode(surface,fixed,{scroll:380}),fixed);
 assert.equal(viewportOffset(surface,{scroll:true}),0);
});
