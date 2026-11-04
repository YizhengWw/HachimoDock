const bytes=s=>new TextEncoder().encode(s).length;
const need=q=>{if(!q)throw new TypeError('text_at requires a bounded p4-texts-v1 table and an int index');};
export function validateRuntimeTexts(r){
 if(Object.hasOwn(r,'texts')){
  need(r.engine==='p4-bounded-runtime-v4'&&Array.isArray(r.texts)&&r.texts.length>=1&&r.texts.length<=64);
  let size=r.texts.length-1;
  for(const s of r.texts){need(typeof s==='string'&&bytes(s)<=120&&![...s].some(c=>{const n=c.codePointAt(0);return n<32||n===127||n>=0xd800&&n<=0xdfff;}));size+=bytes(s);}need(size<=4096);
 }
 const ref=k=>need(Array.isArray(r.texts)&&r.texts.length>0&&typeof k==='string'&&/^[A-Za-z0-9_.-]{1,31}$/.test(k)&&r.vars?.[k]?.type==='int');
 const visit=v=>{if(Array.isArray(v)){v.forEach(visit);return;}if(!v||typeof v!=='object')return;if(Object.hasOwn(v,'text_at')){need(Object.keys(v).length===1);ref(v.text_at);}else Object.values(v).forEach(visit);};visit(r.dashboard);
 for(const n of r.surface?.elements||[])if(Object.hasOwn(n,'text_at')){
  need(Object.keys(n).every(k=>['type','x','y','w','h','color','page','text_at','action','radius','font_size','line_height','align','valign','padding','gap','icon_size','weight','foreground','stroke','stroke_width','icon','state_mask','host_mask','show','motion','in_view','view','scale','tile','show','columns','cursor','image_bind','value_param','value_min','value_max','opacity'].includes(k)));
  need(r.surface.bindings==='p4-variable-bindings-v1'&&r.surface.engine==='p4-app-surface-v1'&&r.surface.graphics==='p4-app-graphics-v1'&&['text','button'].includes(n.type)&&!['text','bind','var_bind','field_bind','key_for','row_bind'].some(k=>Object.hasOwn(n,k)));ref(n.text_at);
 }
}
