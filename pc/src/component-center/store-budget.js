/** Worst-case ordinary app-save admission; same constants as pet_p4_store_budget.h. */
export function persistenceBudget(r){
 const p=r.persist,v3=r.quota==='p4-quota-v3',limit=v3?2303:1023;
 if(p==null)return {bytes:0,limit};
 const count=v3?32:r.quota==='p4-quota-v2'?16:8,selected=p.vars;
 const fail=()=>{throw new TypeError('Invalid bounded persist subset');};
 if(!Array.isArray(selected)||selected.length<1||selected.length>count||new Set(selected).size!==selected.length)fail();
 let bytes=128+(p.page===true?64:0)+(Array.isArray(r.alarms)&&r.alarms.length?268:0);
 const fields=['p4-app-form-v2','p4-app-form-v3'].includes(r.form?.engine)?r.form.fields||[]:[];
 for(const name of selected){
  if(typeof name!=='string'||! /^[A-Za-z0-9_.-]{1,31}$/.test(name))fail();
  const kind=r.vars?.[name]?.type;let size;
  if(kind==='int')size=11;
  else if(kind==='string'){
   const field=fields.find(f=>f.kind==='text'&&f.var===name);if(!v3&&!field)fail();
   const cap=field?.max_bytes??127;if(!Number.isInteger(cap)||cap<1||cap>(field?63:v3?127:63))fail();size=2+6*cap;
  }else fail();
  bytes+=name.length+4+size;
 }
 return {bytes,limit};
}
export function persistenceFits(r){try{const {bytes,limit}=persistenceBudget(r);return bytes<=limit;}catch{return false;}}
