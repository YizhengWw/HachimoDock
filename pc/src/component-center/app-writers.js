// Ownership checks shared by authoring admission and preview entry points.
const fields=r=>r.form?.fields||[];
function roles(r){const f=r.form||{};return new Set([f.focus_var,f.active_var,...fields(r).flatMap(q=>[q.var,q.draft_var]),...Object.entries(f.keyboard||{}).filter(([k])=>k.endsWith('_var')).map(([,v])=>v)].filter(v=>typeof v==='string'));}
export function validateFormWriters(r){
 if(!r.form)return;
 const all=roles(r),transition=r.form.engine==='p4-app-form-v1'?new Set([...all].filter(k=>!fields(r).some(q=>q.var===k))):all;
 const safe=(rule,owned)=>{for(const op of ['set','inc'])if(Object.keys(rule?.[op]||{}).some(k=>owned.has(k)))throw new TypeError('Form variables have an exclusive writer');};
 for(const t of r.transitions||[])for(const branch of [t,t.else])safe(branch,transition);
 for(const t of r.tick||[])for(const branch of [t,t.then])safe(branch,all);
}
export function validateDerivedWriters(r){
 const form=roles(r),seen=new Set(),program=new Set(r.program?.outputs||[]);
 for(const n of r.surface?.elements||[])if(n.type==='longtext'&&Object.hasOwn(n,'count_var')){
  const k=n.count_var,host=r.host===true&&(!r.ownership||r.ownership.host?.includes(k));
  if(typeof k!=='string'||r.vars?.[k]?.type!=='int'||k===n.page_var||seen.has(k)||form.has(k)||program.has(k)||host)throw new TypeError('Longtext count_var requires an exclusive derived integer writer');
  seen.add(k);
 }
 if(seen.size){
  const rules=[...(r.transitions||[]).flatMap(t=>[t,t.else]),...(r.tick||[]).flatMap(t=>[t,t.then])];
  for(const rule of rules)for(const op of ['set','inc'])if(Object.keys(rule?.[op]||{}).some(k=>seen.has(k)))throw new TypeError('Longtext count_var is a read-only derived writer, not a guest set/inc target');
 }
}
