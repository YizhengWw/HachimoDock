import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createParticleActionQueue,previewOwnedValue,clearPreviewOwner,previewFormCapabilityAccepts,previewCanvasCapabilityAccepts} from './preview-lifecycle.js';
import {canvasCapabilityAccepts} from './canvas-core.js';
import {stepParticlePreview} from './particle-preview.js';
const src=name=>readFileSync(new URL(name,import.meta.url),'utf8');
const emitters=[{trigger:{action:'one'}},{trigger:{action:'two'}}];
function ready(queue,decl=emitters,page=0){queue.configure(decl,true,page);const generation=queue.generation();queue.setReady(decl,true,generation);return generation;}
test('particle producer bounds FIFO at four and drops new input',()=>{
 const q=createParticleActionQueue(),g=ready(q);
 for(const a of ['one','two','one','two'])assert.equal(q.enqueue(a),true);
 for(let n=0;n<10000;n++)assert.equal(q.enqueue('one'),false);
 assert.deepEqual(q.take(emitters,g),['one','two','one','two']);assert.deepEqual(q.take(emitters,g),[]);
});
test('only CURRENT declared actions are admitted; no emitters never accumulates',()=>{
 const q=createParticleActionQueue();const g=ready(q);
 for(const a of ['other',null,3,{},undefined])assert.equal(q.enqueue(a),false);
 const next=[{trigger:{action:'next'}}];const ng=ready(q,next);
 assert.equal(q.enqueue('one'),false);assert.equal(q.enqueue('next'),true);
 assert.deepEqual(q.take(emitters,g),[]);assert.deepEqual(q.take(next,ng),['next']);
 const eg=ready(q,[]);assert.equal(q.enqueue('next'),false);assert.deepEqual(q.take([],eg),[]);
});
test('pre-ready and failed core actions are dropped, not replayed after readiness',()=>{
 const q=createParticleActionQueue();q.configure(emitters,true,0);const g=q.generation();
 assert.equal(q.enqueue('one'),false);q.setReady(emitters,true,g);assert.deepEqual(q.take(emitters,g),[]);
 q.enqueue('one');q.setReady(emitters,false,g);q.setReady(emitters,true,g);assert.deepEqual(q.take(emitters,g),[]);
});
test('hide/show invalidates old async core leases and pending actions before effects',()=>{
 const q=createParticleActionQueue(),old=ready(q);q.enqueue('one');
 q.configure(emitters,false,0);assert.equal(q.enqueue('one'),false);
 q.configure(emitters,true,0);const current=q.generation();assert.notEqual(current,old);
 q.setReady(emitters,true,old);assert.equal(q.isReady(emitters,current),false);assert.equal(q.enqueue('one'),false);
 q.setReady(emitters,true,current);q.setReady(emitters,false,old);assert.equal(q.isReady(emitters,current),true);
 assert.deepEqual(q.take(emitters,current),[]);
});
test('page and owner changes cannot replay queued actions',()=>{
 const q=createParticleActionQueue(),g=ready(q);q.enqueue('one');q.configure(emitters,true,1);
 assert.deepEqual(q.take(emitters,g),[]);q.enqueue('two');
 const other=createParticleActionQueue(),ng=ready(other);assert.deepEqual(other.take(emitters,ng),[]);
 assert.deepEqual(q.take(emitters,g),['two']);
});
test('bounded producer drives actual preview consumer once with page/guard admission retained',()=>{
 const q=createParticleActionQueue(),g=ready(q),calls=[];
 const core={burst:i=>calls.push(i),step(){},dots:()=>[]};
 for(const a of ['two','one','two','one','two'])q.enqueue(a);
 stepParticlePreview(core,emitters,{clock:20,page:0,actions:q.take(emitters,g)});
 assert.deepEqual(calls,[0,0,1,1]);calls.length=0;
 stepParticlePreview(core,emitters,{clock:40,page:0,actions:q.take(emitters,g)});assert.deepEqual(calls,[]);
});
test('form versions require their exact native capability, including independent v3',()=>{
 const names=['widgetAppForm','widgetAppFormV2','widgetAppFormV3'];
 for(let i=0;i<3;i++)for(let j=0;j<3;j++)assert.equal(previewFormCapabilityAccepts(`p4-app-form-v${i+1}`,{[names[j]]:`p4-app-form-v${j+1}`}),i===j);
 for(const engine of ['future','p4-app-form-v4','toString','__proto__',undefined,null,{},1])assert.equal(previewFormCapabilityAccepts(engine,{widgetAppForm:'p4-app-form-v1'}),false);
 assert.equal(previewFormCapabilityAccepts('p4-app-form-v3',null),false);
});
test('canvas existing compatibility includes v1 on v2, rejects unknown engine/capability',()=>{
 const engines=['p4-canvas-v1','p4-canvas-v2','future','',undefined,null];
 const caps=['p4-canvas-v1','p4-canvas-v2','future','',undefined,null];
 for(const e of engines)for(const c of caps){const expected=e==='p4-canvas-v1'&&(c==='p4-canvas-v1'||c==='p4-canvas-v2')||e==='p4-canvas-v2'&&c==='p4-canvas-v2';assert.equal(previewCanvasCapabilityAccepts(e,c),expected);assert.equal(canvasCapabilityAccepts(e,c),expected);}
});
test('render ownership hides an old sprite map before effects and ignores out-of-order completion',async()=>{
 const a={},b={},empty=new Map(),oldMap=new Map([['hero','OLD']]),nextMap=new Map([['hero','NEW']]);
 let record={owner:a,value:oldMap};assert.equal(previewOwnedValue(record,b,empty),empty);
 let completeOld,completeNew;
 const old=new Promise(resolve=>{completeOld=resolve;}).then(value=>{record={owner:a,value};});
 const next=new Promise(resolve=>{completeNew=resolve;}).then(value=>{record={owner:b,value};});
 completeNew(nextMap);await next;assert.equal(previewOwnedValue(record,b,empty).get('hero'),'NEW');
 completeOld(oldMap);await old;assert.equal(previewOwnedValue(record,b,empty),empty); // safe even if a stale publisher slipped cleanup
});
test('toast from previous owner is absent during new render; old expiry cannot clear new toast',()=>{
 const a={},b={},old={owner:a,value:{bubble:'A'}},next={owner:b,value:{bubble:'B'}};
 assert.equal(previewOwnedValue(old,b),null);assert.equal(previewOwnedValue(next,b).bubble,'B');
 assert.equal(clearPreviewOwner(next,a),next);assert.equal(clearPreviewOwner(next,b),null);
});
// These inspect connection points only; they do not claim mounted React/DOM coverage.
test('production dispatch and confirmation feed bounded queue after original host authorization',()=>{
 const s=src('./AppSurface.jsx');assert.doesNotMatch(s,/particleActions\.current\.push/);
 assert.match(s,/if\(!active\|\|!actionEnabled\(runtime,snapshot,action\)\|\|!hostGeometryReady/);
 assert.ok(s.indexOf('particleActions.enqueue(action)')>s.indexOf('hostGeometryReady(runtime,snapshot,action,node)'));
 assert.match(s,/if\(c.hostGuard&&\(snapshot.hostReply\?\.fresh!==true\|\|c.hostEpoch!==snapshot.hostEpoch\)\)return;/);
 assert.match(s,/particleActions\.enqueue\(c\.action\);dispatchAction\(c\.action,\{confirm:true,context:c\.context\}\)/);
});
test('particle producer and real core readiness share generation; visibility clears synchronously',()=>{
 const s=src('./AppSurface.jsx'),layer=src('./ParticleLayer.jsx');
 assert.match(s,/useMemo\(\(\)=>createParticleActionQueue\(\),\[previewOwner\]\)/);
 assert.match(s,/const visibility=\(\)=>\{particleActions\.configure\(surface.particles,active&&!document.hidden,particlePage\)/);
 assert.match(s,/actions=\{particleActions\} active=\{particleActive\} generation=\{particleGeneration\}/);
 assert.match(layer,/!actions\?\.isCurrent\(emitters,generation\)/);assert.match(layer,/actions\?\.take\(emitters,generation\)/);
});
test('preview capabilities stay strict without turning optional device support into an install gate',()=>{
 const s=src('../ComponentCenter.jsx');
 const start=s.indexOf('function gameInstallBlockedReason('),end=s.indexOf('\nfunction localInstallBlockedReason(',start);
 const check=new Function(`${s.slice(start,end)};return gameInstallBlockedReason;`)();
 assert.equal(check({appRuntime:{form:{engine:'p4-app-form-v3'}}},{capabilities:{}}),'');
 assert.equal(previewFormCapabilityAccepts('p4-app-form-v3',{widgetAppForm:'p4-app-form-v1'}),false);
 assert.equal(previewCanvasCapabilityAccepts('p4-canvas-v2','p4-canvas-v1'),false);
});
test('page visibility reaches mixed/nonmixed previews; readonly journey/default and service lifecycle remain',()=>{
 const center=src('../ComponentCenter.jsx'),modal=src('./ComponentPreviewModal.jsx'),panel=src('./ComponentAppPanel.jsx'),surface=src('./AppSurface.jsx');
 assert.match(center,/<ComponentPreviewModal\s+active=\{active\}/);
 assert.match(modal,/<ComponentAppPanel[^\n]+active=\{active\}/);
 assert.match(modal,/<DeviceScreenPreview[^\n]+active=\{active\}/);
 assert.match(panel,/<ComponentAppPreview component=\{component\} active=\{active\}/);
 assert.match(panel,/<AppSurface active=\{active\}/);assert.doesNotMatch(panel,/\sactive=\{true\}/);
 assert.match(panel,/enabled&&!busy&&service!=="journey"/);assert.match(surface,/active=false,onAction:dispatchAction/);
 assert.match(src('./DeviceScreenPreview.jsx'),/Boolean\(component\) && !cover && active/);
});
test('sprites are owned at render and drawn before paint; disposed loads remain ignored',()=>{
 const s=src('./DeviceScreenPreview.jsx');assert.match(s,/spriteOwner = useMemo\(\(\) => \(\{\}\), \[scene, sprites, type\]\)/);
 assert.match(s,/spriteImages = previewOwnedValue\(loadedSprites, spriteOwner, EMPTY_SPRITE_IMAGES\)/);
 assert.match(s,/if \(!disposed\) setLoadedSprites\(\{ owner: spriteOwner, value: next \}\)/);
 assert.match(s,/useLayoutEffect\(\(\) => \{\s+const canvas = canvasRef.current/);
});
test('toast render checks owner, listener checks stopped, and expiry clears only same owner',()=>{
 const s=src('./AppSurface.jsx');assert.match(s,/visibleToast=previewOwnedValue\(toast,previewOwner\)/);
 assert.match(s,/if\(stopped\|\|event.payload.componentId!==componentId\)return/);
 assert.match(s,/setToast\(current=>clearPreviewOwner\(current,previewOwner\)\)/);
 assert.match(s,/\{visibleToast&&<div role="status"/);assert.doesNotMatch(s,/\{toast&&<div role="status"/);
});
