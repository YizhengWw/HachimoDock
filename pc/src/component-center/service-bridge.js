/** Shared bounded presentation semantics. This module never calls a provider,
 * chooses a file, executes an action, or treats display interpolation as ACK. */
export const SERVICE_BRIDGE='p4-service-bridge-v1';
const name=(s,n)=>typeof s==='string'&&new RegExp(`^[A-Za-z0-9_.-]{1,${n}}$`).test(s);
const integer=(n,min,max)=>Number.isInteger(n)&&n>=min&&n<=max;
export function validTimeline(t){return t&&Object.keys(t).length===5&&integer(t.positionMs,0,86400000)&&integer(t.durationMs,1,86400000)&&t.positionMs<=t.durationMs&&typeof t.running==='boolean'&&integer(t.horizonMs,100,10000)&&['percent','position','remaining'].includes(t.projection);}
export function timelineValue(t,age){
 if(!validTimeline(t)||!Number.isFinite(age)||age<0||age>=10000)return null;
 const p=Math.min(t.durationMs,t.positionMs+(t.running?Math.min(t.horizonMs,age):0));
 return Math.floor(t.projection==='position'?p:t.projection==='remaining'?t.durationMs-p:p*100/t.durationMs);
}
export function validImageRef(r){return r&&Object.keys(r).length===4&&name(r.key,47)&&name(r.revision,64)&&integer(r.width,8,192)&&integer(r.height,8,192);}
export function bridgeReply(reply,now=performance.now()){
 return reply?.service==='bridge'?{...reply,_bridgeReceivedAt:now}:reply;
}
export function bridgeSnapshot(runtime,snapshot,now){
 if(runtime?.surface?.services!==SERVICE_BRIDGE||!snapshot?.presentation)return snapshot;
 const age=now-snapshot._bridgeReceivedAt;
 if(!Number.isFinite(age)||age<0||age>=10000)return {...snapshot,vars:Object.fromEntries(Object.entries(runtime.vars||{}).map(([k,v])=>[k,v.type==='int'?0:'—'])),serviceImages:{},ui:{mode:7,actions:[]}};
 const vars={...snapshot.vars};
 const entries=Object.entries(snapshot.presentation.timelines||{});
 if(entries.length>2)return {...snapshot,serviceImages:{},ui:{mode:7,actions:[]}};
 for(const [key,t] of entries){const n=timelineValue(t,age);if(runtime.vars?.[key]?.type==='int'&&n!==null)vars[key]=n;}
 return {...snapshot,vars};
}
export function serviceImage(node,snapshot){
 if(!node?.image_bind)return null;
 const ref=snapshot?.presentation?.images?.[node.image_bind],image=snapshot?.serviceImages?.[node.image_bind];
 return validImageRef(ref)&&image?.key===ref.key&&image?.revision===ref.revision&&typeof image.dataUrl==='string'&&image.dataUrl.startsWith('data:image/png;base64,')?image.dataUrl:null;
}
export function sliderData(node,x){
 if(!name(node?.value_param,31)||node.type!=='meter'||!Number.isFinite(x)||!integer(node.w,1,640))return null;
 const lo=node.value_min??0,hi=node.value_max??100;
 if(!integer(lo,-1e9,1e9)||!integer(hi,lo+1,1e9))return null;
 return {[node.value_param]:lo+Math.trunc((hi-lo)*Math.max(0,Math.min(node.w,x-node.x))/node.w)};
}
export function validateBridgeSurface(s,runtime){
 if(!s||typeof s!=='object')return [];
 const declared=Object.hasOwn(s,'services'),errors=[];
 if(runtime&&(declared?!/^[0-9a-f]{64}$/.test(runtime.host_contract||''):Object.hasOwn(runtime,'host_contract')))errors.push('服务桥缺少绑定签名');
 if(declared&&(s.services!==SERVICE_BRIDGE||s.engine!=='p4-app-surface-v1'||s.graphics!=='p4-app-graphics-v1'||s.bindings!=='p4-variable-bindings-v1'||runtime?.host!==true||runtime?.host_context!=='p4-host-context-v1'||runtime?.input?.engine!=='p4-input-v1'))errors.push('服务桥需要显式能力及带上下文的宿主应用');
 for(const n of s.elements||[]){
  if(Object.hasOwn(n,'image_bind')&&(!declared||n.type!=='image'||!name(n.image_bind,15)||['tile','motion','rotate_ms'].some(k=>Object.hasOwn(n,k))||(s.images||[]).some(im=>im.id===n.image&&im.encoding)))errors.push('动态图片绑定无效');
  if(Object.hasOwn(n,'value_param')){
   if(!declared||!n.action?.startsWith('host.')||!sliderData(n,n.x)||runtime?.input?.confirm?.[n.action])errors.push('参数滑杆需要真实宿主动作、整数范围及即时操作');
  }else if(Object.hasOwn(n,'value_min')||Object.hasOwn(n,'value_max'))errors.push('数值范围缺少参数名');
 }return errors;
}
