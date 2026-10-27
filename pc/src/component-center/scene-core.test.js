import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {fileURLToPath} from 'node:url';
import {createSceneCore} from './scene-core.js';
import {initialApp,appAction,appHit,appPage} from './app-runtime.js';
const bytes=fs.readFileSync(new URL('./classic-core.wasm',import.meta.url));
const root=process.env.PET_APP_PACKAGE_DIR||fileURLToPath(new URL('../../builtin-clawpkgs',import.meta.url));
const load=id=>JSON.parse(fs.readFileSync(`${root}/${id}/runtime/widget.json`,'utf8'));
test('frog real shared core moves, shows reactions and clears them on tick',{skip:!root},async()=>{
  const r=load('bloomfrog_companion'),c=await createSceneCore(bytes,r.scene);
  const before=c.snapshot();assert.equal(before.running,true);
  c.action('move_left',0);assert.equal(c.snapshot().entities[0].x,before.entities[0].x-1);
  c.action('move_right',0);assert.equal(c.snapshot().entities[0].x,before.entities[0].x);
  c.action('pet_pet',0);assert.equal(c.snapshot().entities.find(e=>e.id==='joy').active,true);
  c.tick(1);c.tick(901);assert.equal(c.snapshot().entities.some(e=>e.id==='joy'&&e.active),false);
  c.action('feed_pet',902);assert.equal(c.snapshot().entities.find(e=>e.id==='snack').active,true);
  let state=initialApp(r);state=appAction(r,state,'pet_pet');assert.equal(state.vars.bond,31);
  assert.equal(appHit(r.surface,appPage(r,state),365,400,state.vars),'pet_pet');
});
test('pong shared core starts, moves paddle, scores, ends and restarts',{skip:!root},async()=>{
  const r=load('two-key-pong'),c=await createSceneCore(bytes,r.scene);
  c.action('start',0);assert.equal(c.snapshot().running,true);
  const before=c.snapshot().entities[0].x;c.action('shift_left',0);assert.equal(c.snapshot().entities[0].x,before-2);
  c.action('shift_right',0);assert.equal(c.snapshot().entities[0].x,before);
  // Follow the real ball until a contact scores; then intentionally move away.
  let now=0;
  while(c.snapshot().score===0&&now<10000){
    const [bat,ball]=c.snapshot().entities;
    if(ball.x<bat.x)c.action('shift_left',now);
    else if(ball.x>bat.x+bat.width-2)c.action('shift_right',now);
    now+=171;c.tick(now);
  }
  assert.ok(c.snapshot().score>0);
  while(!c.snapshot().over&&now<30000){
    const [bat,ball]=c.snapshot().entities;
    c.action(ball.x<8?'shift_right':'shift_left',now);now+=171;c.tick(now);
  }
  assert.equal(c.snapshot().over,true);c.action('start',now);
  assert.equal(c.snapshot().running,true);assert.equal(c.snapshot().score,0);
});
test('pointer moves only its declared axis and never mints scores',{skip:!root},async()=>{
  const r=load('two-key-pong'),c=await createSceneCore(bytes,r.scene);
  assert.equal(c.pointer('bat',15,15,1),0);
  c.action('start',0);const before=c.snapshot();
  c.pointer('bat',15,1,1);
  assert.equal(c.snapshot().entities[0].x,12);
  assert.equal(c.snapshot().entities[0].y,before.entities[0].y);
  for(let i=0;i<30;i++)c.pointer('bat',i%16,0,1);
  assert.equal(c.snapshot().score,0);
  c.pointer('bat',-100,0,1);assert.equal(c.snapshot().entities[0].x,0);
  assert.equal(c.pointer('missing',5,5,1),0);
  assert.equal(c.pointer('bat',2147483647,0,1),0);
});
