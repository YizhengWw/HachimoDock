/** Opt-in bounded host collection. Data only; no fetch, services or records. */
export const HOST_COLLECTION_ENGINE='p4-host-collection-v1';
const need=q=>{if(!q)throw new TypeError('Invalid bounded host collection');};
const object=(v,keys)=>{need(v&&typeof v==='object'&&!Array.isArray(v));need(Object.keys(v).length===keys.length&&Object.keys(v).every(k=>keys.includes(k)));};
const integer=(v,lo,hi)=>Number.isInteger(v)&&v>=lo&&v<=hi;
const name=(v,max)=>typeof v==='string'&&v.length>=1&&v.length<=max&&[...v].every(c=>{const n=c.charCodeAt(0);return n>=65&&n<=90||n>=97&&n<=122||n>=48&&n<=57||'._-'.includes(c);});
const text=(v,nonempty=false)=>typeof v==='string'&&(!nonempty||v.length>0)&&![...v].some(c=>{const n=c.codePointAt(0);return n<32||n>=127&&n<160||n>=0xd800&&n<=0xdfff;})&&new TextEncoder().encode(v).length<=63;
export function hostCollectionConfig(v,surface,runtime){
  object(v,['engine','source','x','y','w','row_height','gap','visible','page','actions','colors']);
  need(v.engine===HOST_COLLECTION_ENGINE&&name(v.source,16)&&integer(v.x,0,639)&&integer(v.y,0,479)&&integer(v.w,128,640-v.x));
  need(integer(v.visible,1,8)&&integer(v.row_height,48,96)&&integer(v.gap,0,16)&&integer(v.page,0,4));
  const height=v.visible*v.row_height+(v.visible-1)*v.gap;need(v.y+height<=480);
  need(Array.isArray(v.actions)&&v.actions.length===v.visible&&new Set(v.actions).size===v.actions.length&&v.actions.every(a=>typeof a==='string'&&a.startsWith('host.')&&name(a.slice(5),42)));
  need(Array.isArray(v.colors)&&v.colors.length===6&&v.colors.every(c=>integer(c,0,65535)));
  if(surface){
    need(surface.engine==='p4-app-surface-v1'&&surface.graphics==='p4-app-graphics-v1'&&Array.isArray(surface.elements)&&surface.elements.length>=1&&surface.elements.length+v.visible*3<=32);
    need(!['game_view','scene_view','data_view','input'].some(k=>Object.hasOwn(surface,k)));
    const overlap=n=>v.x<n.x+n.w&&v.x+v.w>n.x&&v.y<n.y+n.h&&v.y+height>n.y;
    const views=surface.viewports?.items||(surface.viewport?[surface.viewport]:[]);
    need(!views.some(overlap)&&!surface.elements.some(n=>Object.hasOwn(n,'action')&&!Object.hasOwn(n,'view')&&!Object.hasOwn(n,'in_view')&&overlap(n)));
  }
  if(runtime)need(runtime.host===true&&runtime.host_context==='p4-host-context-v1'&&runtime.input?.engine==='p4-input-v1'
    &&(v.page===4||v.page<Math.max(1,runtime.pages?.length||0))&&v.actions.every(a=>runtime.transitions?.some(t=>t.on===a)));
  return {...v,actions:[...v.actions],colors:[...v.colors]};
}
export function hostCollectionSnapshot(v){
  object(v,['engine','source','visible','count','offset','selectionKey','rows']);
  need(v.engine===HOST_COLLECTION_ENGINE&&name(v.source,16)&&integer(v.visible,1,8)&&integer(v.count,0,128)&&integer(v.offset,0,127));
  const last=Math.floor(Math.max(0,v.count-1)/v.visible)*v.visible;
  need(v.offset<=last&&v.offset%v.visible===0&&(v.selectionKey===null||text(v.selectionKey,true))&&Array.isArray(v.rows)&&v.rows.length===v.visible);
  const keys=new Set();
  const rows=v.rows.map((r,i)=>{
    object(r,['present','key','label','detail','index','selected','truncated']);
    need(typeof r.present==='boolean'&&typeof r.selected==='boolean'&&typeof r.truncated==='boolean'&&r.present===(v.offset+i<v.count));
    need(text(r.key,r.present)&&text(r.label)&&text(r.detail));
    if(r.present){need(!keys.has(r.key)&&r.index===v.offset+i&&r.selected===(v.selectionKey===r.key));keys.add(r.key);}
    else need(r.key===''&&r.label===''&&r.detail===''&&r.index===-1&&!r.selected&&!r.truncated);
    return {present:r.present,key:r.key,label:r.label,detail:r.detail,index:r.index,selected:r.selected,truncated:r.truncated};
  });
  return {engine:v.engine,source:v.source,visible:v.visible,count:v.count,offset:v.offset,selectionKey:v.selectionKey,rows};
}
export function hostCollectionContext(v){try{return JSON.stringify(hostCollectionSnapshot(v));}catch{return 'unavailable';}}
export function updateCollectionReply(previous,reply,merge=false){
  const old=Number.isSafeInteger(previous?._collectionEpoch)&&previous._collectionEpoch>=0?previous._collectionEpoch:0;
  const epoch=old+(hostCollectionContext(previous?.collectionView)!==hostCollectionContext(reply?.collectionView)?1:0);
  const data=merge?{...previous,...reply}:{...reply};
  if(!Object.hasOwn(reply||{},'collectionView'))delete data.collectionView;
  if(!Number.isSafeInteger(epoch))return {...data,collectionView:null,_collectionEpoch:Number.MAX_SAFE_INTEGER,ui:{mode:7,actions:[]}};
  return {...data,_collectionEpoch:epoch};
}
export function hostCollectionSurface(surface,snapshot){
  if(!surface?.collection_view)return surface;
  const c=hostCollectionConfig(surface.collection_view,surface);
  let model;try{model=hostCollectionSnapshot(snapshot);}catch{}
  const ready=model?.source===c.source&&model.visible===c.visible;
  const extra=[];
  for(let slot=0;slot<c.visible;slot++)for(let field=0;field<3;field++){
    const row=ready?model.rows[slot]:null,present=!!row?.present;
    const placeholder=slot===0&&field===1&&(!ready||model.count===0);
    const n={type:field?'text':'button',x:c.x,y:c.y+slot*(c.row_height+c.gap),w:c.w,h:c.row_height,
      color:c.colors[field===1?2:field===2?3:row?.selected?1:0],page:c.page,scale:1,opacity:255,
      font_size:field===2?12:16,line_height:field===2?16:20,align:'left',valign:'top',padding:0,
      text:'',hidden:!present&&!placeholder,disabled:!field&&!present,_collectionSlot:slot,_collectionField:field};
    if(!field)Object.assign(n,{radius:12,foreground:c.colors[2],stroke:c.colors[row?.selected?5:4],stroke_width:1,action:present?c.actions[slot]:undefined,_collectionLabel:present?(row.label||row.key):''});
    else {n.x+=12;n.y+=field===1?6:28;n.w-=24;n.h=field===1?20:16;n.weight=field===1?600:400;
      n.text=placeholder?(ready?'暂无条目':'等待电脑数据'):present?(field===1?row.label:row.detail):'';}
    extra.push(n);
  }
  return {...surface,elements:[...surface.elements,...extra]};
}
