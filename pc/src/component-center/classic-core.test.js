import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import {createClassicCore} from './classic-core.js';
const bytes=await fs.readFile(new URL('./classic-core.wasm',import.meta.url));
for(const type of ['snake','blocks','flappy'])test(`${type}: real C core start, input, progression, result and restart`,async()=>{
  const actions=type==='flappy'?{flap:'flap'}:type==='snake'?{start:'start',left:'left',right:'right'}:{start:'start',left:'left',right:'right',rotate:'rotate',drop:'drop'};
  const game=await createClassicCore(bytes,{type,tick_ms:100,actions}),start=type==='flappy'?'flap':'start';
  assert.equal(game.snapshot().running,false);game.action(start,0);assert.equal(game.snapshot().running,true);
  const before=game.snapshot();game.action(type==='flappy'?'flap':'left',1);game.tick(120);game.tick(240);
  assert.notDeepEqual(game.snapshot().cells,before.cells);
  for(let t=360;t<200000&&!game.snapshot().over;t+=120){if(type==='blocks')game.action('drop',t);game.tick(t);}
  assert.equal(game.snapshot().over,true);game.action(start,200001);
  assert.equal(game.snapshot().over,false);assert.equal(game.snapshot().running,true);
});
test('package transitions prevent start from resetting a running game',async()=>{
  const runtime=JSON.parse(await fs.readFile(new URL('../../builtin-clawpkgs/snake-turn/runtime/widget.json',import.meta.url)));
  const game=await createClassicCore(bytes,runtime);
  game.action(runtime.game.actions.start,0);game.tick(1000);
  const before=game.snapshot();
  game.action(runtime.game.actions.start,1001);
  assert.deepEqual(game.snapshot(),before);
});
test('flappy speed is package-owned; defaults and invalid parameters are explicit',async()=>{
  const base={type:'flappy',tick_ms:100,actions:{flap:'flap'}};
  const fast=await createClassicCore(bytes,base);
  const slow=await createClassicCore(bytes,{...base,vertical_speed_percent:50});
  fast.action('flap',0);slow.action('flap',0);
  for(let t=100;t<=300;t+=100){fast.tick(t);slow.tick(t);}
  const bird=s=>s.cells.findIndex(t=>t===3);
  assert.ok(bird(slow.snapshot())>bird(fast.snapshot()),'half-speed bird rises less in the same elapsed time');
  const pipes=s=>s.cells.map((t,i)=>t===1||t===2?i:-1).filter(i=>i>=0);
  assert.deepEqual(pipes(slow.snapshot()),pipes(fast.snapshot()),'pipes keep their cadence');
  for(const n of [0,24,101,50.5,'50'])await assert.rejects(createClassicCore(bytes,{...base,vertical_speed_percent:n}));
  await assert.rejects(createClassicCore(bytes,{type:'blocks',tick_ms:100,actions:{},vertical_speed_percent:50}));
});
