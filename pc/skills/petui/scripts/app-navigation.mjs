// Bounded local navigation. No service calls, URLs or component-specific routes.
import {encodedNavigationCore} from './navigation-bytes.mjs';
export const NAVIGATION_ENGINE = "p4-app-navigation-v1";
export const NAVIGATION_MAX_DEPTH = 16;
let core;
function nativeNavigation(runtime,state,rule,apply){
  if(runtime.navigation!==NAVIGATION_ENGINE)return null;
  const pages=runtime.pages,history=state.history??[];
  // Page ceiling follows the negotiated quota: 4 (v1), 6 (p4-quota-v2), 12 (p4-quota-v3 / app shell).
  const maxPages=runtime.quota==='p4-quota-v3'?12:runtime.quota==='p4-quota-v2'?6:4;
  if(!Array.isArray(pages)||pages.length<1||pages.length>maxPages||!Array.isArray(history)||history.length>NAVIGATION_MAX_DEPTH)return null;
  const names=pages.map(p=>p?.id);
  if(names.some(n=>typeof n!=='string'||!n)||new Set(names).size!==names.length)return null;
  const page=names.indexOf(state.page),from=rule.from_page==null?-1:names.indexOf(rule.from_page),p=rule.page;
  if(page<0||(rule.from_page!=null&&from<0)||!p||typeof p!=='object'||Array.isArray(p))return null;
  const entries=Object.entries(p);if(entries.length!==1)return null;
  const [key,value]=entries[0],operation={push:1,replace:2,reset:3,back:4}[key];
  if(!operation||(key==='back'&&value!==true))return null;
  const target=key==='back'?-1:names.indexOf(value);if(key!=='back'&&target<0)return null;
  const prior=[];for(let i=0;i<history.length;i++){
    if(!Object.hasOwn(history,i))return null;
    const at=names.indexOf(history[i]);if(at<0)return null;prior.push(at);
  }
  if(!core){
    const module=new WebAssembly.Module(Uint8Array.from(atob(encodedNavigationCore),c=>c.charCodeAt(0)));
    if(WebAssembly.Module.imports(module).length)throw Error('Navigation imports are forbidden');
    core=new WebAssembly.Instance(module,{}).exports;
    if(core.memory.buffer.byteLength!==65536)throw Error('Navigation memory budget changed');
  }
  core.nv_begin(names.length,prior.length,page);
  for(let i=0;i<prior.length;i++)if(!core.nv_history(i,prior[i]))return null;
  if(!core.nv_rule(operation,target,from)||!core.nv_allowed())return null;
  if(!apply)return {};
  if(!core.nv_apply())return null;
  return {page:names[core.nv_page()],history:Array.from({length:core.nv_depth()},(_,i)=>names[core.nv_at(i)])};
}
export function navigationAllowed(runtime, state, rule) {
  if(rule.from_page!=null && rule.from_page!==state.page)return false;
  const p=rule.page;
  if(!p||typeof p!=="object")return true;
  return nativeNavigation(runtime,state,rule,false)!==null;
}
export function navigate(runtime,state,rule) {
  const p=rule.page;
  if(p&&typeof p==='object'){
    const next=nativeNavigation(runtime,state,rule,true);if(!next)return false;
    state.page=next.page;state.history=next.history;return true;
  }
  if(!navigationAllowed(runtime,state,rule))return false;
  const pages=(runtime.pages||[]).map(p=>p.id),at=pages.indexOf(state.page);
  if(!p||!pages.length)return true;
  if(typeof p==="string") {
    state.page=p==="next"?pages[(at+1)%pages.length]:p==="prev"?pages[(at+pages.length-1)%pages.length]:p;
    if(runtime.navigation)state.history=[];
  }
  return true;
}
