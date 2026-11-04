// Read only the operator's explicit local evidence files. Never connect, flash,
// deploy, generate images, read credentials or publish a report. This checks
// coherence, NOT authentication or completion of all architecture capabilities.
import {readFileSync,statSync,writeFileSync,mkdirSync} from 'node:fs';
import {resolve,dirname} from 'node:path';import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {rendererStackArtifact} from './framework-stack-evidence.mjs';
export const REQUIRED_STAGES=['runner_selftest','frontend','bridge','firmware_host','workflows','graphics','form_schema','evidence_tools','browser_ui','desktop_typecheck','source_consistency'];
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const hex=(s,n)=>typeof s==='string'&&s.length===n&&!/[^0-9a-f]/.test(s);
const integer=(v,lo=0,hi=Number.MAX_SAFE_INTEGER)=>Number.isSafeInteger(v)&&v>=lo&&v<=hi;
const token=(v,n)=>typeof v==='string'&&v.length>=1&&v.length<=n&&!/[^A-Za-z0-9_.+-]/.test(v);
export function fnv(bytes,pixels=false){let sum=2166136261;for(let i=0;i<bytes.length;i+=pixels?2:1)sum=Math.imul(sum^(pixels?bytes[i]|bytes[i+1]<<8:bytes[i]),16777619)>>>0;return sum.toString(16).padStart(8,'0');}
function requireValue(v,code){if(!v)throw Error(code);}
function sameSource(a,b){return a?.valid===true&&b?.valid===true&&hex(a.digest,64)&&hex(a.commit,40)&&a.digest===b.digest&&a.commit===b.commit;}
export function verifyScreenshot(c,raw,target){
 requireValue(Buffer.isBuffer(raw)&&raw.length===614400,'raw_frame_size');
 requireValue(c?.passed===true&&c.receivedBytes===raw.length&&c.expectedBytes===raw.length&&Array.isArray(c.errors)&&c.errors.length===0&&c.eventQueueDrops===0&&c.audioUsed===false&&c.cloudCalls===0,'capture_not_verified');
 const b=c.begin,e=c.end,i=c.identity;
 requireValue(b?.width===640&&b.height===480&&b.format==='rgb565le'&&b.integrity==='chunk-fnv1a32-v1'&&b.totalBytes===raw.length&&token(b.id,31),'invalid_frame_metadata');
 requireValue(integer(b.chunkBytes,1,3072)&&Math.ceil(raw.length/b.chunkBytes)<=200,'invalid_chunk_budget');
 const count=Math.ceil(raw.length/b.chunkBytes);requireValue(e?.id===b.id&&e.totalBytes===raw.length&&e.chunks===count&&c.nextIndex===count&&c.endReceived===true&&c.metadataMatches===true&&c.checksumMatches===true&&Array.isArray(c.chunks)&&c.chunks.length===count,'incomplete_frame');
 requireValue(hex(e.checksum,8)&&e.checksum===fnv(raw,true)&&c.computedChecksum===e.checksum,'frame_checksum');
 c.chunks.forEach((chunk,index)=>{const bytes=raw.subarray(index*b.chunkBytes,Math.min(raw.length,(index+1)*b.chunkBytes)),checksum=fnv(bytes);
  requireValue(chunk.index===index&&chunk.expectedBytes===bytes.length&&chunk.receivedBytes===bytes.length&&chunk.rawSize===bytes.length&&chunk.rawSizeMatches===true&&chunk.checksumMatches===true&&chunk.checksum===checksum&&chunk.computedChecksum===checksum,'chunk_integrity');});
 requireValue(i?.engine==='p4-evidence-binding-v1'&&i.boardMatched===true&&i.bootBound===true&&i.sourceBound===true&&hex(i.bootId,16)&&i.bootId===b.bootId&&i.appElfSha256===target.elfSha256&&i.authentication===false,'identity_not_bound');
 requireValue(integer(i.fromUptimeUs)&&integer(i.toUptimeUs,1)&&i.toUptimeUs>i.fromUptimeUs&&token(i.buildId,95)&&i.buildId.startsWith(target.version+'+')&&typeof i.buildDirty==='boolean'&&token(i.gitSha,40)&&integer(i.protocolSchema,1,4294967295),'invalid_identity_window');
 return {frameBytes:raw.length,chunks:count,rawSha256:hash(raw),bootId:i.bootId,appElfSha256:i.appElfSha256,sourceBound:true};
}
export function verifyEvidence({offline,builds,capture=null,raw=null,profile='ui'}){
 const out={schema:1,engine:'p4-framework-evidence-v1',gates:[],evidenceChainReady:false,frameworkComplete:false,releaseReady:false,
  deviceContacted:false,audioUsed:false,cloudCalls:0,authentication:false};
 const gate=(id,fn)=>{try{const result=fn();out.gates.push({id,status:result?.pending?'pending':'verified',...(result?.pending?{reason:result.pending}:{evidence:result})});}catch(error){out.gates.push({id,status:'failed',reason:/^[a-z_]+$/.test(error.message)?error.message:'invalid_evidence'});}};
 gate('source',()=>{requireValue(sameSource(offline?.source,offline?.sourceAfter),'offline_source_changed');requireValue(sameSource(builds?.sourceBefore,builds?.sourceAfter)&&sameSource(offline.source,builds.sourceBefore),'different_source_revisions');return {digest:offline.source.digest,commit:offline.source.commit,scope:offline.source.scope?.entireRepository===true?'repository':'declared_framework_inputs'};});
 gate('offline',()=>{
  requireValue(Array.isArray(offline?.stages),'invalid_offline_report');const ids=offline.stages.map(s=>s.id);requireValue(new Set(ids).size===ids.length,'duplicate_stage');
  for(const id of REQUIRED_STAGES){const s=offline.stages.find(s=>s.id===id);if(!s)return {pending:'missing_offline_stage'};
   if(s.tests){requireValue(['passed','failed','skipped'].every(k=>integer(s.tests[k])),'invalid_test_counts');requireValue(s.tests.failed===0,'offline_stage_failed');}
   if(s.status==='failed')throw Error('offline_stage_failed');if(s.status!=='passed'||s.selected!==true)return {pending:'offline_stage_not_verified'};
   if(id!=='source_consistency')requireValue(integer(s.tests?.passed,1)&&s.tests.failed===0&&s.tests.skipped===0,'false_success_summary');
  }return {stages:REQUIRED_STAGES.length};
 });
 gate('firmware',()=>{
  requireValue(builds?.engine==='p4-build-evidence-v1'&&builds.status==='built_source_consistent'&&builds.sourceConsistent===true&&builds.hardwareVerified===false&&builds.releaseReady===false,'unverified_build');
  requireValue(Array.isArray(builds.targets)&&builds.targets.length===2&&new Set(builds.targets.map(t=>t.id)).size===2,'invalid_build_targets');
  const records=builds.targets.map(t=>{requireValue(['normal','ui'].includes(t.id)&&t.target===(t.id==='normal'?'esp32_p4_evboard':'esp32_p4_canvas_candidate')&&t.status==='built'&&t.exitCode===0&&token(t.version,31)&&hex(t.elfSha256,64)&&hex(t.imageSha256,64),'invalid_build_artifact');
   const b=t.budget,sections=b?.internalSections;
   requireValue(integer(t.imageBytes,288,2621440)&&integer(t.elfBytes,64,67108864)&&b?.appBytes===t.imageBytes&&b.otaBytes===2621440&&b.otaHeadroomBytes===b.otaBytes-t.imageBytes&&b.otaHeadroomBytes>=131072&&b.minimumOtaHeadroomBytes===131072&&b.maximumInternalStaticBytes===393216&&integer(b.internalStaticBytes,0,393216)&&integer(b.externalStaticBytes)
     &&b.memoryAccounting==='p4-allocated-ram-v2'&&sections&&typeof sections==='object'&&!Array.isArray(sections)
     &&['.iram0.text','.dram0.data','.dram0.bss'].every(name=>Object.hasOwn(sections,name))
     &&Object.entries(sections).length>0&&Object.entries(sections).every(([name,bytes])=>/^\.[A-Za-z0-9_.-]{1,63}$/.test(name)&&integer(bytes,1,33554432))
     &&Object.values(sections).reduce((a,b)=>a+b,0)===b.internalStaticBytes,'invalid_build_budget');
   const stack=t.rendererStack?rendererStackArtifact(t.rendererStack,t.elfSha256):null;
   return {profile:t.id,version:t.version,elfSha256:t.elfSha256,imageSha256:t.imageSha256,otaHeadroomBytes:b.otaHeadroomBytes,internalStaticBytes:b.internalStaticBytes,
    ...(stack?{reviewedStaticStackBytes:stack.maxReviewedBytes,stackBudgetBytes:stack.budgetBytes,allCallEdges:false}:{})};});return {targets:records};
 });
 gate('physical_capture',()=>{
  if(!capture&&!raw)return {pending:'device_evidence_not_collected'};requireValue(['normal','ui'].includes(profile),'invalid_profile');
  const target=builds?.targets?.find(t=>t.id===profile);requireValue(target,'missing_build_profile');
  requireValue(typeof capture.identity?.gitSha==='string'&&capture.identity.gitSha.length>=7&&builds.sourceBefore?.commit?.startsWith(capture.identity.gitSha)&&capture.identity.buildDirty===builds.sourceBefore?.dirty,'firmware_source_metadata_mismatch');
  return verifyScreenshot(capture,raw,target);
 });
 out.evidenceChainReady=out.gates.every(g=>g.status==='verified');
 // A coherent screenshot is not a timing, acoustic, deployment, driver or
 // general architecture acceptance. These are deliberately not inferred.
 out.remainingGates=['rendered_component_package_identity','correlated_frame_and_operation_timing','voice_and_aec_acceptance','remaining_capability_acceptance'];
 out.status=out.gates.some(g=>g.status==='failed')?'failed':out.evidenceChainReady?'evidence_chain_verified':'pending';return out;
}
function readBounded(file,max){const before=statSync(file);requireValue(before.isFile()&&before.size>0&&before.size<=max,'input_size_invalid');const data=readFileSync(file),after=statSync(file);requireValue(data.length===before.size&&after.size===before.size&&after.mtimeMs===before.mtimeMs&&after.ino===before.ino,'input_changed');return data;}
export function main(args){
 const paths={};let output,profile='ui';const allowed={'--offline':'offline','--builds':'builds','--capture':'capture','--raw':'raw'};
 for(let n=0;n<args.length;n++){const flag=args[n];if(Object.hasOwn(allowed,flag)&&args[n+1]&&!paths[allowed[flag]])paths[allowed[flag]]=resolve(args[++n]);else if(flag==='--output'&&args[n+1]&&!output)output=resolve(args[++n]);else if(flag==='--profile'&&args[n+1])profile=args[++n];else throw Error('invalid_arguments');}
 requireValue(paths.offline&&paths.builds&&output&&(!paths.capture===!paths.raw),'explicit_input_paths_required');
 const bytes=Object.fromEntries(Object.entries(paths).map(([id,file])=>[id,readBounded(file,id==='raw'?614400:4*1024*1024)]));
 const report=verifyEvidence({offline:JSON.parse(bytes.offline),builds:JSON.parse(bytes.builds),capture:bytes.capture?JSON.parse(bytes.capture):null,raw:bytes.raw||null,profile});
 report.inputSha256=Object.fromEntries(Object.entries(bytes).map(([id,bytes])=>[id,hash(bytes)]));mkdirSync(dirname(output),{recursive:true});writeFileSync(output,JSON.stringify(report,null,2)+'\n',{flag:'wx'});
 console.log(JSON.stringify({status:report.status,evidenceChainReady:report.evidenceChainReady,frameworkComplete:false,releaseReady:false}));return report.status==='failed'?1:report.evidenceChainReady?0:2;
}
if(process.argv[1]&&resolve(process.argv[1])===fileURLToPath(import.meta.url)){try{process.exitCode=main(process.argv.slice(2));}catch{console.error('evidence_input_invalid');process.exitCode=2;}}
