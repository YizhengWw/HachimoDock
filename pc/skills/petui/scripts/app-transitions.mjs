// Pure branch selection shared by input admission and execution. No effects or IO.
import {navigationAllowed} from './app-navigation.mjs';
import {dataProjection} from './d-surface.mjs';
export function cellGet(rows,x,y){return Number.isInteger(x)&&Number.isInteger(y)&&y>=0&&y<(rows?.length||0)&&x>=0&&x<rows[y].length?parseInt(rows[y][x],16):-1;}
const cellArg=(v,vars)=>typeof v==='object'?vars[v.var]:v;
// p4-logic-v1 table, also used by timer completions and the C game core.
const compare={lte:(a,b)=>a<=b,lt:(a,b)=>a<b,gte:(a,b)=>a>=b,gt:(a,b)=>a>b,eq:(a,b)=>a===b,ne:(a,b)=>a!==b};
export function conditionHolds(when,vars,cells=[]){
 if(!when)return true;
 if(Array.isArray(when.all))return when.all.every(c=>conditionHolds(c,vars,cells));
 const left=when.cell?cellGet(cells,cellArg(when.cell.x,vars),cellArg(when.cell.y,vars)):Number(vars[when.var]);
 if(when.lte!==undefined)return left<=Number(when.lte);
 const right=when.value&&typeof when.value==='object'?Number(vars[when.value.var]):Number(when.value);
 return (compare[when.op||'lte']||(()=>false))(left,right);
}
export function selectTransition(runtime,snapshot,action){
 for(const transition of runtime.transitions||[]){
  if(transition.on!==action||transition.from!=='*'&&transition.from!==snapshot.state
    ||transition.from_page!=null&&transition.from_page!==snapshot.page)continue;
  const taken=conditionHolds(transition.when,snapshot.vars,snapshot.cells);
  if(taken?!navigationAllowed(runtime,snapshot,transition):!transition.else)continue;
  return taken?transition:{...transition.else,on:transition.on,from:transition.from,from_page:transition.from_page};
 }
 return null;
}
// Predict the same single paging effect (including var operands and clamping)
// without mutating the snapshot. A refused first branch cannot run a later one.
export function pagingChanges(runtime,snapshot,rule){
 const key=runtime.data?.page_var;
 if(!key||rule.to||rule.page||Object.keys(rule.inc||{}).length+Object.keys(rule.set||{}).length!==1
   ||!Object.hasOwn(rule.inc||{},key)&&!Object.hasOwn(rule.set||{},key))return true;
 const current=Number(snapshot.vars?.[key]||0),increment=Object.hasOwn(rule.inc||{},key);
 const operand=increment?rule.inc[key]:rule.set[key],value=operand&&typeof operand==='object'?Number(snapshot.vars[operand.var]):operand;
 let next=increment?Math.min(1e9,Math.max(-1e9,current+value)):value;
 const [lo,hi]=runtime.limits?.[key]||[-1e9,1e9];next=Math.max(lo,Math.min(hi,next));
 if(!Number.isFinite(next))return false;
 const {pages,page}=dataProjection(runtime,snapshot);
 return page!==((next%pages)+pages)%pages;
}
