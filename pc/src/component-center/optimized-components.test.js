import {test} from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import {fileURLToPath} from "node:url";
import {initialApp,appAction,appAdvance,appHit,appVisible} from "./app-runtime.js";
import {createClassicCore} from "./classic-core.js";
const root=process.env.PET_OPTIMIZED_PACKAGE_DIR||fileURLToPath(new URL('../../builtin-clawpkgs',import.meta.url));
const load=id=>JSON.parse(fs.readFileSync(`${root}/${id}/runtime/widget.json`));
const fixture={skip:!root};
test("frog caps bond, retains durable counters, every interaction still responds",fixture,()=>{
  const r=load("bloomfrog_companion");let s=initialApp(r);
  // Accepted care actions respect the same cooldown as physical/touch input.
  for(let i=0;i<50;i++){s=appAction(r,s,"feed_pet");s=appAdvance(r,s,30000);}
  assert.equal(s.vars.bond,100);assert.equal(s.vars.meals,50);
  assert.deepEqual(r.persist.vars,["bond","pets","meals"]);
  s=appAction(r,s,"pet_pet");assert.equal(s.vars.pets,1);assert.equal(s.vars.bond,100);
});
test("water due pause/resume stays due until acknowledged; no phantom drink",fixture,()=>{
  const r=load("drink-reminder");let s=initialApp(r);
  for(let i=0;i<60;i++)s=appAdvance(r,s,60000);
  assert.equal(s.state,"due");assert.equal(s.vars.drink_count_today,0);
  s=appAction(r,s,"reminder.pause_resume");assert.equal(s.state,"paused_due");
  s=appAdvance(r,s,600000);s=appAction(r,s,"reminder.pause_resume");
  assert.equal(s.state,"due");assert.equal(s.vars.remaining_min,0);
  s=appAction(r,s,"reminder.acknowledge");assert.equal(s.vars.drink_count_today,1);assert.equal(s.vars.remaining_min,60);
});
test("tomato paused reset and skip do not restart, full completion counts exactly once",fixture,()=>{
  const r=load("tomato-clock");let s=initialApp(r);
  assert.equal(s.state,"ready");
  s=appAdvance(r,s,10000);assert.equal(s.vars.completed_sessions,0);
  s=appAction(r,s,"tomato.start_pause");assert.equal(s.state,"focus");
  s=appAction(r,s,"tomato.start_pause");assert.equal(s.state,"focus_paused");
  s=appAction(r,s,"tomato.reset_phase");assert.equal(s.state,"focus_paused");
  s=appAction(r,s,"tomato.skip_phase");assert.equal(s.state,"rest_paused");
  const left=s.vars.remaining_s;s=appAdvance(r,s,50000);assert.equal(s.vars.remaining_s,left);
  s=appAction(r,initialApp(r),"tomato.start_pause");for(let i=0;i<1500;i++)s=appAdvance(r,s,1000);
  assert.equal(s.state,"rest");assert.equal(s.vars.completed_sessions,1);
  s=appAction(r,s,"tomato.start_pause");for(let i=0;i<400;i++)s=appAdvance(r,s,1000);
  assert.equal(s.vars.completed_sessions,1);
});
test("touch targets preserve actions and reach all tool settings",fixture,()=>{
  for(const id of ["computer-status","upcoming-todos","stock-watchlist"]){
    const r=load(id);
    for(const n of r.surface.elements.filter(n=>n.action)){
      assert.ok(n.w>=44&&n.h>=44);
      // Host detail controls deliberately do not exist in the list mode. Check
      // every declared visible mode rather than assuming all actions are ready.
      const modes=r.host?Array.from({length:8},(_,i)=>i):[0];
      assert.ok(modes.some(mode=>appVisible(n,0,{},0,mode)));
      for(const mode of modes.filter(mode=>appVisible(n,0,{},0,mode))){
        assert.equal(appHit(r.surface,0,n.x+n.w/2,n.y+n.h/2,{},0,mode),n.action,`${id}/${n.action}/mode-${mode}`);
      }
    }
  }
  const g=load("hachimi-garden");assert.equal(g.surface.elements.filter(n=>["less","more"].includes(n.action)).length,2);
  const h=load("hachimi-home");let s=appAction(h,initialApp(h),"select.plant");
  const before=s.vars.plant;s=appAction(h,s,"next");assert.equal(s.vars.plant,before+1);
  s=appAction(h,s,"previous");assert.equal(s.vars.plant,before);assert.equal(s.state,"plant");
  s=appAction(h,s,"finish");assert.equal(s.state,"idle");assert.equal(s.vars.plant,before);
});
test("break snooze, countdown and current activity survive component restore declaration",fixture,()=>{
  const r=load("hachimi-break");let s=appAction(r,initialApp(r),"secondary");
  for(let i=0;i<10;i++)s=appAdvance(r,s,1000);
  assert.equal(s.vars.left,290);assert.equal(r.persist.states.later,"later");assert.ok(r.persist.vars.includes("left"));
  assert.equal(r.persist.states.run,"paused");
});
for(const id of ["snake-turn","block-combo","flappy-bird"])test(`${id} actual C game is frozen while paused and resumes without catch-up`,fixture,async()=>{
  const r=load(id),bytes=fs.readFileSync(new URL("./classic-core.wasm",import.meta.url)),c=await createClassicCore(bytes,r);
  const start=r.game.actions.start||r.game.actions.flap;c.action(start,1000);
  c.action(r.lifecycle.pause_action,1001);assert.equal(c.snapshot().state,"paused");
  const before=c.snapshot();c.tick(60000);c.tick(120000);
  assert.deepEqual(c.snapshot().cells,before.cells);assert.equal(c.snapshot().score,before.score);
  c.action(start,120000);
  if(id==='flappy-bird'){
    // Flap is disabled while paused; only the visible pause/resume control resumes.
    assert.equal(c.snapshot().state,"paused");
    c.action(r.lifecycle.pause_action,120001);
  }
  assert.equal(c.snapshot().state,"playing");
  c.tick(120002);assert.deepEqual(c.snapshot().cells,before.cells);
  assert.ok(r.persist.vars.includes("best"));
});
