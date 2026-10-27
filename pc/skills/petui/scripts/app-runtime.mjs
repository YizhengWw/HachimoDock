/** Bounded v4 tool simulator. Preview never calls host services or writes device saves. */
import {actionEnabled} from "./input-policy.mjs";
import {localInitial,localAction,localAdvance,localView} from './local-tools.mjs';
const bounded = n => Math.min(1e9, Math.max(-1e9, n));
export function initialApp(runtime) {
  const s={state:runtime.initial_state,page:runtime.initial_page||runtime.pages?.[0]?.id||"",vars:Object.fromEntries(Object.entries(runtime.vars||{}).map(([k,d])=>[k,d.init??(d.type==="int"?0:"")])),clock:0,ticks:(runtime.tick||[]).map(()=>0)};
  return runtime.local?localAdvance(runtime,localInitial(s)):s;
}
function effects(runtime,s,r) {
  const original={...s.vars};
  const clamp=(k,v)=>{if(typeof v!=="number")return v;const [lo,hi]=runtime.limits?.[k]||[-1e9,1e9];return Math.max(lo,Math.min(hi,v));};
  for(const [k,v] of Object.entries(r.set||{}))s.vars[k]=clamp(k,v&&typeof v==="object"?original[v.var]:v);
  for(const [k,v] of Object.entries(r.inc||{}))s.vars[k]=clamp(k,bounded(Number(s.vars[k])+v));
  for(const [k,[lo,hi]] of Object.entries(runtime.limits||{}))s.vars[k]=Math.min(hi,Math.max(lo,s.vars[k]));
}
export function appAction(runtime,state,action) {
  if(!actionEnabled(runtime,state,action))return state;
  if(runtime.local)return localAction(runtime,state,action);
  const r=(runtime.transitions||[]).find(r=>r.on===action&&(r.from==="*"||r.from===state.state));
  if(!r)return state;
  const s=structuredClone(state);if(r.to&&r.to!==s.state){s.state=r.to;s.ticks.fill(s.clock);}
  const pages=(runtime.pages||[]).map(p=>p.id),at=pages.indexOf(s.page);
  if(r.page&&pages.length)s.page=r.page==="next"?pages[(at+1)%pages.length]:r.page==="prev"?pages[(at+pages.length-1)%pages.length]:r.page;
  effects(runtime,s,r);
  if(runtime.input?.cooldown?.[action])s.actionTimes={...s.actionTimes,[action]:s.clock};
  return s;
}
export function appAdvance(runtime,state,ms) {
  const s=structuredClone(state);s.clock+=Math.max(0,ms);
  if(runtime.local)return localAdvance(runtime,s);
  (runtime.tick||[]).forEach((t,i)=>{
    if(t.while_state!=="*"&&t.while_state!==s.state){s.ticks[i]=s.clock;return;}
    const steps=Math.min(10,Math.floor((s.clock-s.ticks[i])/t.every_ms));
    for(let n=0;n<steps;n++){
      effects(runtime,s,t);s.ticks[i]+=t.every_ms;
      if(t.when&&s.vars[t.when.var]<=t.when.lte){
        if(t.then?.to)s.state=t.then.to;
        effects(runtime,s,t.then||{});s.ticks.fill(s.clock);break;
      }
    }
  });return s;
}
function value(rule,s) {
  if(typeof rule==="string")return rule;
  if(!rule)return "";
  if(rule.switch_state)return value(rule.switch_state[s.state],s);
  if(rule.switch_page)return value(rule.switch_page[s.page],s);
  if(rule.var)return String(s.vars[rule.var]??"");
  const name=rule.fmt_mmss||rule.fmt_hms;
  if(name){const n=Math.max(0,Number(s.vars[name])||0),ss=String(n%60).padStart(2,"0"),mm=String(Math.floor(n/60)%(rule.fmt_hms?60:1e9)).padStart(2,"0");return rule.fmt_hms?`${String(Math.floor(n/3600)).padStart(2,"0")}:${mm}:${ss}`:`${mm}:${ss}`;}
  return "";
}
export function appDashboard(runtime,state) {
  if(runtime.local)return {...runtime.dashboard,...localView(runtime,state)};
  return Object.fromEntries(Object.entries(runtime.dashboard||{}).map(([k,r])=>[k,k==="progress"?Math.max(0,Math.min(100,Number(state.vars[r?.pct_of])*100/(Number(state.vars[r?.of_max])||1))) :value(r,state)]));
}
export function appPage(runtime,state){return Math.max(0,(runtime.pages||[]).findIndex(p=>p.id===state.page));}
export function appVisible(n,page,vars={},stateIndex=0,hostMode=0){return (!n.host_mask||!!(n.host_mask&(1<<hostMode)))&&(!n.state_mask||!!(n.state_mask&(1<<stateIndex)))&&(n.page==null||n.page===4||n.page===page)&&(!n.show||(Number.isFinite(vars[n.show.var])&&vars[n.show.var]>=n.show.min&&vars[n.show.var]<=n.show.max));}
export function appTile(n,vars={}){const q=n.tile;if(!q)return null;const v=Number(vars[q.var])||0,c=q.columns,max=Number(vars[q.max_var]);return {columns:c,rows:q.rows,row:q.row,index:q.max_var?Math.min(c-1,Math.max(0,max>0?Math.floor(v*c/max):0)):((v%c)+c)%c};}
export function appHit(surface,page,x,y,vars={},stateIndex=0,hostMode=0){return [...(surface?.elements||[])].reverse().find(n=>n.action&&appVisible(n,page,vars,stateIndex,hostMode)&&x>=n.x&&x<n.x+n.w&&y>=n.y&&y<n.y+n.h)?.action||null;}
