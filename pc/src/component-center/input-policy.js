// Versioned, bounded input policy. No package code or host side effects.
import {localEnabled} from './local-tools.js';
import {selectTransition,pagingChanges} from './app-transitions.js';
import {formOperation,formAllowed} from './app-form.js';
import {hostAction,programInputValues} from './variable-owners.js';
export const INPUT_ENGINE = "p4-input-v1";
export function hasInputPolicy(runtime) { return runtime?.input?.engine === INPUT_ENGINE; }
export function actionAccepted(runtime,snapshot,action,accepted=true){
  if(accepted!==true||!hasInputPolicy(runtime)||!runtime.input.cooldown?.[action])return snapshot;
  return {...snapshot,actionTimes:{...snapshot.actionTimes,[action]:snapshot.clock}};
}
export function actionEnabled(runtime, snapshot, action) {
  if (!hasInputPolicy(runtime) || !action) return true;
  if(runtime.program&&Object.hasOwn(runtime.program.actions||{},action)&&(snapshot.program?.live===false||snapshot.program?.canEnqueue===false))return false;
  const op=formOperation(runtime,action);if(op)return formAllowed(runtime,snapshot,op);
  if(runtime.local&&!localEnabled(runtime,snapshot,action))return false;
  if(runtime.ownership&&hostAction(runtime,action)&&(snapshot.hostReply?.fresh!==true||!Array.isArray(snapshot.ui?.actions)))return false;
  if(hostAction(runtime,action) && snapshot.ui?.actions && !snapshot.ui.actions.includes(action))return false;
  if(runtime.ownership&&programInputValues(runtime,snapshot.vars,action,snapshot.hostReply)===null)return false;
  const wait=runtime.input.cooldown?.[action],last=snapshot.actionTimes?.[action];
  if(wait&&last!=null&&(snapshot.clock<last||snapshot.clock-last<wait))return false;
  const rule=selectTransition(runtime,snapshot,action);
  return rule!==null&&pagingChanges(runtime,snapshot,rule);
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
