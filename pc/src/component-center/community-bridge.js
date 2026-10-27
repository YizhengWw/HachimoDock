// Fixed-origin, versioned public UI protocol. Never accept paths, URLs or native command names.
export const COMMUNITY_ORIGIN='https://115.191.67.48';
const ID=/^[a-z][a-z0-9_-]{0,43}$/;
export function parseCommunityMessage(event,source,origin,channel){
  if(event.source!==source||event.origin!==origin)return null;
  const d=event.data;if(!d||d.v!==1||d.type!=='pet-community-request'||d.channel!==channel||!/^web-[0-9]{1,9}$/.test(d.requestId||''))return null;
  const f=d.fields;if(!f||typeof f!=='object'||Array.isArray(f))return null;
  const rules={browse:['query','category','order','offset','view','metric','window'],rankings:['metric','window','view','category','query','order','offset'],detail:['id'],comments:['id','offset'],like:['id','active'],comment:['id','text','requestId'],profile:['name'],favorite:['id'],status:['id'],download:['id'],configure:['id']};
  if(!rules[d.action]||Object.keys(f).some(k=>!rules[d.action].includes(k)))return null;
  if(['detail','comments','like','comment','favorite','status','download','configure'].includes(d.action)&&!ID.test(f.id||''))return null;
  if('offset'in f&&(!Number.isInteger(f.offset)||f.offset<0||f.offset>10000))return null;
  if('query'in f&&(typeof f.query!=='string'||f.query.length>80))return null;
  if('category'in f&&!['','hachimi','game','productivity','tool','relax','life'].includes(f.category))return null;
  if('order'in f&&!['featured','latest'].includes(f.order))return null;
  if('metric'in f&&!['downloads','likes'].includes(f.metric))return null;
  if('window'in f&&!['24h','7d','all'].includes(f.window))return null;
  if(d.action==='like'&&typeof f.active!=='boolean')return null;
  if(d.action==='comment'&&(typeof f.text!=='string'||!f.text.trim()||[...f.text].length>500||!/^[A-Za-z0-9_-]{16,64}$/.test(f.requestId||'')))return null;
  if(d.action==='profile'&&(typeof f.name!=='string'||[...f.name].length>40))return null;
  // Reconstruct inputs: fields unrelated to each native operation never pass through.
  const fields={};for(const key of ({browse:['query','category','order','offset'],rankings:['metric','window']}[d.action]||rules[d.action]))if(key in f)fields[key]=f[key];
  return {action:d.action,fields,requestId:d.requestId};
}
