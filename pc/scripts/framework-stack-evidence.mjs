// Pure validation/projection only. No files, build commands or device access.
const IDS=['media_text','app_label_width','app_label_icon','data_label_width','data_label_icon','snapshot_host','snapshot_form_text','snapshot_form_values','snapshot_form_availability','render_metrics'];
const V2_IDS=[...IDS,'snapshot_host_collection'];
const V3_IDS=[...V2_IDS,'async_asset_pack','async_asset_packed_fallback','async_asset_i420_fallback'];
const V4_IDS=[...V3_IDS,'app_panel_gradient','app_cache_visibility','clip_frame_visibility'];
const integer=n=>Number.isSafeInteger(n)&&n>=0&&n<=4096;
const name=s=>typeof s==='string'&&/^[A-Za-z_][A-Za-z0-9_]{0,63}$/.test(s);
const exact=(v,k)=>v&&typeof v==='object'&&!Array.isArray(v)&&Object.keys(v).length===k.length&&k.every(key=>Object.hasOwn(v,key));
const fail=()=>{throw Error('invalid_renderer_stack_evidence');};
export function rendererStackArtifact(s,elfSha256){
 const v2=s?.schema===2&&s?.engine==='p4-render-stack-v2',v3=s?.schema===3&&s?.engine==='p4-render-stack-v3',v4=s?.schema===4&&s?.engine==='p4-render-stack-v4',ids=v4?V4_IDS:v3?V3_IDS:v2?V2_IDS:IDS;
 if(!exact(s,['schema','engine','elfSha256','budgetBytes','allCallEdges','paths','maxReviewedBytes'])||(!v2&&!v3&&!v4&&(s.schema!==1||s.engine!=='p4-render-stack-v1'))||s.elfSha256!==elfSha256||!/^[a-f0-9]{64}$/.test(elfSha256)||s.budgetBytes!==4096||s.allCallEdges!==false||!Array.isArray(s.paths)||s.paths.length!==ids.length||!integer(s.maxReviewedBytes))fail();
 const used=new Set(),paths=s.paths.map(p=>{
  if(!exact(p,['id','bytes','frames'])||!ids.includes(p.id)||used.has(p.id)||!integer(p.bytes)||!Array.isArray(p.frames)||p.frames.length<2||p.frames.length>10)fail();used.add(p.id);
  const functions=new Set(),frames=p.frames.map(f=>{
   const modules=['pet_p4_renderer','pet_p4_miniapp','pet_p4_app_host','pet_p4_form_v2',...(v2||v3||v4?['pet_p4_surface','pet_p4_host_collection']:[])];
   if(!exact(f,['module','function','bytes','emitted'])||!modules.includes(f.module)||!name(f.function)||!integer(f.bytes)||typeof f.emitted!=='boolean'||(!f.emitted&&f.bytes!==0))fail();
   const key=f.module+':'+f.function;if(functions.has(key))fail();functions.add(key);
   return {module:f.module,function:f.function,bytes:f.bytes,emitted:f.emitted};
  });
  if(frames[0].module!=='pet_p4_renderer'||frames[0].function!=='pet_p4_renderer_render'||!frames[0].emitted||frames.reduce((n,f)=>n+f.bytes,0)!==p.bytes)fail();
  return {id:p.id,bytes:p.bytes,frames};
 });
 if(Math.max(...paths.map(p=>p.bytes))!==s.maxReviewedBytes)fail();
 return {schema:s.schema,engine:s.engine,elfSha256,budgetBytes:4096,allCallEdges:false,paths,maxReviewedBytes:s.maxReviewedBytes};
}
