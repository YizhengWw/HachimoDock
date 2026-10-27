import fs from 'node:fs';
import assert from 'node:assert/strict';
import {localInitial,localAction,localAdvance,localView,localTarget,civilDay} from './local-tools.mjs';
const r=JSON.parse(fs.readFileSync(process.argv[2],'utf8')),buttons=JSON.parse(fs.readFileSync(process.argv[3],'utf8'));
let checks=0;const ok=v=>{assert.ok(v);checks++};
const initial=()=>localInitial({vars:Object.fromEntries(Object.entries(r.vars).map(([k,v])=>[k,v.init])),clock:0},civilDay(2026,10,23)*86400+17*3600);
let s=initial(),c=r.local;
const action=(op,random=123456789)=>{const key=Object.entries(c.actions).find(([,v])=>v===op)?.[0];assert.ok(key);s=localAction(r,s,key,random);return s;};
const advance=()=>{s.clock+=c.reveal_ms;s=localAdvance(r,s);};
ok(buttons.every(b=>b.event!=='button.sw3.short_press'&&c.actions[b.action]));
ok(r.surface.elements.filter(e=>e.action).every(e=>c.actions[e.action]&&e.w>=44&&e.h>=44));
if(c.kind==='deck'){
  for(let group=0;group<c.groups.length;group++){
    s.vars.local0=group;let last=-1;const reached=new Set();
    let seed=0x12345678;
    for(let i=0;i<512;i++){
      seed^=seed<<13;seed^=seed>>>17;seed^=seed<<5;
      action('draw',seed>>>0);ok(s.local.phase===1);const blocked=JSON.stringify(s);action('draw');ok(JSON.stringify(s)===blocked);advance();
      const selected=s.vars[`local${2+2*group}`];ok(s.local.phase===2&&selected!==last);last=selected;reached.add(selected);ok(localView(r,s).note===c.groups[group].items[selected].note);
    }ok(reached.size===c.groups[group].items.length);
    if(Object.values(c.actions).includes('exclude')){
      for(let i=0;i<c.groups[group].items.length;i++){action('exclude');if(i<c.groups[group].items.length-1)advance();}ok(s.local.phase===4);
      action('draw');ok(s.local.phase===4);action('reset');ok(s.vars[`local${1+2*group}`]===0);action('draw');advance();ok(s.local.phase===2);
    }
  }
  if(Object.values(c.actions).includes('close')){action('draw');action('close');advance();ok(s.local.phase===0);}
}else{
  s.wall=-1;s=localAdvance(r,s);ok(localView(r,s).phase===5);action('edit');ok(s.local.phase===5);
  s.wall=civilDay(2026,10,23)*86400+17*3600;s=localAdvance(r,s);ok(s.local.phase===0);
  ok(localView(r,s).metricValue==='01:00:00');s.clock=3600000;s=localAdvance(r,s);ok(localView(r,s).metricValue==='已到站');
  s=initial();const saved=JSON.stringify(s.vars);action('edit');action('up');action('cancel');ok(JSON.stringify(s.vars)===saved);
  action('edit');action('up');action('edit');ok(s.vars.local2===1140);
  action('next');action('next');ok(s.vars.local0===2);action('edit');action('next');action('next');action('up');action('edit');ok(s.vars.local5===civilDay(2026,10,25));
  const restored=localInitial({vars:structuredClone(s.vars),clock:0},s.wall);ok(localView(r,restored).note===localView(r,s).note);
  ok(localTarget('weekdays',0,1080,civilDay(2026,10,24)*86400)===civilDay(2026,10,26)*86400+64800);
  ok(localTarget('weekend',0,0,civilDay(2026,10,25)*86400)===civilDay(2026,10,24)*86400);
  ok(localTarget('date',civilDay(2024,2,29),0,civilDay(2024,2,28)*86400)===civilDay(2024,2,29)*86400);
}
console.log(JSON.stringify({ok:true,mode:'local-tools',checks,errors:[],deviceVerified:false}));
