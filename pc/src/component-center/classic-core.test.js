import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import {createClassicCore} from './classic-core.js';
const bytes=await fs.readFile(new URL('./classic-core.wasm',import.meta.url));
const GOMOKU_ACTIONS={place:'place',left:'left',right:'right',up:'up',down:'down',undo:'undo'};
// Walk the cursor to the lowest free cell and place; white answers on the next tick.
function gomokuPlay(game,t){
  const s=game.snapshot(),w=s.width,cursor=s.cells.findIndex(c=>c&4),target=s.cells.findIndex(c=>!(c&3));
  let cx=cursor%w,cy=Math.floor(cursor/w);const tx=target%w,ty=Math.floor(target/w);
  while(cx>tx){game.action('left',t);cx--;}while(cx<tx){game.action('right',t);cx++;}
  while(cy>ty){game.action('up',t);cy--;}while(cy<ty){game.action('down',t);cy++;}
  game.action('place',t);game.tick(t+120);return t+120;
}
for(const type of ['snake','blocks','flappy','gomoku'])test(`${type}: real C core start, input, progression, result and restart`,async()=>{
  const actions=type==='flappy'?{flap:'flap'}:type==='snake'?{start:'start',left:'left',right:'right'}:type==='gomoku'?GOMOKU_ACTIONS:{start:'start',left:'left',right:'right',rotate:'rotate',drop:'drop'};
  const game=await createClassicCore(bytes,{type,tick_ms:100,actions}),start=type==='flappy'?'flap':type==='gomoku'?'place':'start';
  assert.equal(game.snapshot().running,false);game.action(start,0);assert.equal(game.snapshot().running,true);
  const before=game.snapshot();game.action(type==='flappy'?'flap':'left',1);game.tick(120);game.tick(240);
  assert.notDeepEqual(game.snapshot().cells,before.cells);
  if(type==='gomoku'){let t=360;for(let moves=0;moves<120&&!game.snapshot().over;moves++)t=gomokuPlay(game,t+1);}
  else for(let t=360;t<200000&&!game.snapshot().over;t+=120){if(type==='blocks')game.action('drop',t);game.tick(t);}
  assert.equal(game.snapshot().over,true);game.action(start,200001);
  assert.equal(game.snapshot().over,false);assert.equal(game.snapshot().running,true);
});
test('gomoku: 15x15 board, packed cells, bounded AI reply, score contract and undo',async()=>{
  const game=await createClassicCore(bytes,{type:'gomoku',tick_ms:300,actions:GOMOKU_ACTIONS});
  let s=game.snapshot();assert.equal(s.kind,5);assert.equal(s.width,15);assert.equal(s.height,15);
  assert.equal(s.cells.filter(c=>c&3).length,0);
  game.action('place',10);s=game.snapshot();assert.equal(s.running,true);assert.equal(s.score,0);
  assert.equal(s.cells.findIndex(c=>c&4),7*15+7,'cursor starts at tengen');
  assert.equal(game.snapshot().aiReads,0);
  game.action('place',11);s=game.snapshot();assert.equal(s.score,1);
  assert.equal(s.cells[7*15+7]&3,1,'black stone at the cursor');assert.ok(s.cells[7*15+7]&8,'last-move marker');
  game.action('place',12);assert.equal(game.snapshot().score,1,'no second stone while white is thinking');
  game.tick(309);assert.equal(game.snapshot().cells.filter(c=>(c&3)===2).length,0,'white waits for the tick');
  game.tick(310);s=game.snapshot();
  const white=s.cells.findIndex(c=>(c&3)===2);assert.ok(white>=0,'white replied on the tick');
  assert.equal(s.score,2,'score counts every stone: the reply is the odd->even step a package can cue on');
  assert.ok(s.cells[white]&8,'marker moves to the reply');
  assert.ok(s.aiReads>0&&s.aiReads<=15*15*4*9,`AI work ${s.aiReads} stays within 8100 line reads`);
  assert.equal(s.aiReads,(15*15-1)*4*9,'first reply scans every empty cell exactly once');
  const wx=white%15,wy=Math.floor(white/15);assert.ok(Math.abs(wx-7)<=1&&Math.abs(wy-7)<=1,'white answers next to the opening stone');
  game.action('undo',311);s=game.snapshot();
  assert.equal(s.cells.filter(c=>c&3).length,0,'undo removes the pair');assert.equal(s.score,0);
  assert.equal(s.cells.findIndex(c=>c&4),7*15+7,'cursor returns to the undone stone');
  game.action('undo',312);assert.deepEqual(game.snapshot().cells,s.cells,'single-level undo');
  let t=400;for(let moves=0;moves<120&&!game.snapshot().over;moves++)t=gomokuPlay(game,t+1);
  s=game.snapshot();assert.equal(s.over,true);assert.ok(s.score>=2000&&s.score<3000,'white win encodes as 2000+stones');
  assert.equal(s.score-2000,s.cells.filter(c=>c&3).length,'low digits are the stone count');
  assert.equal(s.cells.filter(c=>c&16).length,5,'winning five highlighted');
  assert.equal(s.cells.findIndex(c=>c&4),-1,'cursor hidden after the result');
  const before=s.cells;game.action('left',t+1);game.tick(t+600);assert.deepEqual(game.snapshot().cells,before,'finished board is frozen');
  game.action('place',t+601);s=game.snapshot();assert.equal(s.running,true);assert.equal(s.over,false);assert.equal(s.score,0);
});
test('gomoku through a package with transitions: place starts, moves, and restarts from result',async()=>{
  const runtime={initial_state:'ready',states:['ready','playing','result'],transitions:[
    {from:'ready',on:'gomoku.place',to:'playing'},{from:'result',on:'gomoku.place',to:'playing'},
    ...['place','left','right','up','down','undo'].map(k=>({from:'playing',on:`gomoku.${k}`}))],
    game:{type:'gomoku',tick_ms:300,playing_state:'playing',result_state:'result',score_var:'score',
      actions:Object.fromEntries(Object.keys(GOMOKU_ACTIONS).map(k=>[k,`gomoku.${k}`]))}};
  const game=await createClassicCore(bytes,runtime);
  assert.equal(game.snapshot().state,'ready');game.action('gomoku.place',0);
  assert.equal(game.snapshot().state,'playing');assert.equal(game.snapshot().running,true);
  game.action('gomoku.undo',1);assert.equal(game.snapshot().score,0);
  game.action('gomoku.place',2);assert.equal(game.snapshot().score,1);
  let t=3;for(let moves=0;moves<120&&game.snapshot().state!=='result';moves++){
    const s=game.snapshot(),w=s.width,cursor=s.cells.findIndex(c=>c&4),target=s.cells.findIndex(c=>!(c&3));
    if(cursor<0||target<0)break;let cx=cursor%w,cy=Math.floor(cursor/w);const tx=target%w,ty=Math.floor(target/w);
    while(cx>tx){game.action('gomoku.left',t);cx--;}while(cx<tx){game.action('gomoku.right',t);cx++;}
    while(cy>ty){game.action('gomoku.up',t);cy--;}while(cy<ty){game.action('gomoku.down',t);cy++;}
    game.action('gomoku.place',t);t+=300;game.tick(t);
  }
  assert.equal(game.snapshot().state,'result');
  game.action('gomoku.place',t+1);assert.equal(game.snapshot().state,'playing');assert.equal(game.snapshot().score,0);
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
