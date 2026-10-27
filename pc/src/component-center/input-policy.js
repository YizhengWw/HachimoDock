// Versioned, bounded input policy. No package code or host side effects.
import {localEnabled} from './local-tools.js';
export const INPUT_ENGINE = "p4-input-v1";
export function hasInputPolicy(runtime) { return runtime?.input?.engine === INPUT_ENGINE; }
export function actionEnabled(runtime, snapshot, action) {
  if (!hasInputPolicy(runtime) || !action) return true;
  if(runtime.local&&!localEnabled(runtime,snapshot,action))return false;
  if(runtime.host && snapshot.ui?.actions && !snapshot.ui.actions.includes(action))return false;
  const wait=runtime.input.cooldown?.[action],last=snapshot.actionTimes?.[action];
  if(wait&&last!=null&&(snapshot.clock<last||snapshot.clock-last<wait))return false;
  return (runtime.transitions || []).some(t => {
    if(t.on!==action || t.from!=="*"&&t.from!==snapshot.state)return false;
    const key=runtime.data?.page_var;
    if(key && !t.to && !t.page && Object.keys(t.inc||{}).length+Object.keys(t.set||{}).length===1 &&
       (Object.hasOwn(t.inc||{},key)||Object.hasOwn(t.set||{},key))){
      const pages=Math.max(1,Math.ceil(Math.min(20,snapshot.data?.rows?.length||0)/5));
      const current=Number(snapshot.vars?.[key]||0);
      const next=Object.hasOwn(t.inc||{},key)?current+t.inc[key]:t.set[key];
      if(!Number.isFinite(next))return false;
      return ((current%pages)+pages)%pages!==((next%pages)+pages)%pages;
    }
    return true;
  });
}
export function pressAction(runtime, action) {
  return hasInputPolicy(runtime) && runtime.input.press?.includes(action);
}
export function swipeAction(runtime, dx, dy, elapsed) {
  if (!hasInputPolicy(runtime) || elapsed > 700 || elapsed < 0) return null;
  const ax = Math.abs(dx), ay = Math.abs(dy);
  if (Math.max(ax, ay) < 40 || Math.max(ax, ay) < Math.min(ax, ay) * 1.5) return null;
  const direction = ax > ay ? (dx < 0 ? "left" : "right") : (dy < 0 ? "up" : "down");
  return runtime.input.swipe?.[direction] || null;
}
