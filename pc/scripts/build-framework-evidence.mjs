// Explicit development compilation only: two allowlisted v1 targets, no serial,
// flashing, installation, audio, credentials, publishing or release packaging.
import {readFileSync,statSync,mkdirSync,mkdtempSync,writeFileSync} from 'node:fs';
import {resolve,dirname,join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {sourceVersion} from './test-framework-offline.mjs';
import {rendererStackArtifact} from './framework-stack-evidence.mjs';
import {partitionArtifact} from './framework-partition-evidence.mjs';
const repo=resolve(dirname(fileURLToPath(import.meta.url)),'../..');
export const TARGETS=Object.freeze({normal:'esp32_p4_evboard',ui:'esp32_p4_canvas_candidate',program:'esp32_p4_program_public_candidate'});
const sha=bytes=>createHash('sha256').update(bytes).digest('hex');
const fail=code=>{throw Error(code);};
function elfMemory(elf){
 const start=elf.readUInt32LE(32),size=elf.readUInt16LE(46),count=elf.readUInt16LE(48),strings=elf.readUInt16LE(50);
 if(size!==40||count<1||count>4096||strings>=count||start+count*size>elf.length)fail('invalid_elf_sections');
 const table=start+strings*size,offset=elf.readUInt32LE(table+16),length=elf.readUInt32LE(table+20);
 if(offset+length>elf.length||length<1)fail('invalid_elf_names');
 const regions=[[0x30100000,0x30102000],[0x4ff00000,0x4ffc0000],[0x50108000,0x50110000]];
 const required=['.iram0.text','.dram0.data','.dram0.bss'],found=new Set(),accounted=new Map(),spans=[];let external=0;
 for(let i=0;i<count;i++){
  const at=start+i*size,index=elf.readUInt32LE(at),type=elf.readUInt32LE(at+4),flags=elf.readUInt32LE(at+8),address=elf.readUInt32LE(at+12),data=elf.readUInt32LE(at+16),bytes=elf.readUInt32LE(at+20);
  if(index>=length||(type!==8&&data+bytes>elf.length))fail('invalid_elf_section_bounds');
  const end=elf.indexOf(0,offset+index);if(end<0||end>=offset+length||end-offset-index>255)fail('invalid_elf_section_name');
  const rawName=elf.subarray(offset+index,end);if(rawName.some(c=>c>127))fail('invalid_elf_section_name');
  const name=rawName.toString('ascii');if(!(flags&2))continue;
  if(!/^\.[A-Za-z0-9_.-]{1,63}$/.test(name)||found.has(name))fail('invalid_or_duplicate_allocated_section');
  found.add(name);if(bytes>32*1024*1024||address+bytes>4294967296)fail('invalid_allocated_section_bounds');
  const fits=regions.some(([lo,hi])=>address>=lo&&address<hi&&address+bytes<=hi),intersects=regions.some(([lo,hi])=>address<hi&&address+bytes>lo);
  if((intersects||required.includes(name))&&!fits)fail('internal_section_bounds');
  if(fits&&bytes){accounted.set(name,bytes);spans.push([address,address+bytes]);}
  if(name.startsWith('.ext_ram.')&&name!=='.ext_ram.dummy'&&bytes){if(address<0x48000000||address+bytes>0x4c000000)fail('external_section_bounds');external+=bytes;}
 }
 if(!required.every(n=>found.has(n)))fail('missing_elf_memory_sections');
 spans.sort((a,b)=>a[0]-b[0]);if(spans.some((s,i)=>i>0&&s[0]<spans[i-1][1]))fail('overlapping_internal_sections');
 const sections=Object.fromEntries([...accounted].sort((a,b)=>a[0]<b[0]?-1:1));
 return {internal:Object.values(sections).reduce((a,b)=>a+b,0),external,sections};
}
export function firmwareArtifact(elf,image,budget,partitionTable){
 const partition=partitionTable===undefined?null:partitionArtifact(partitionTable);
 const otaBytes=partition?.otaBytes??2621440;
 if(!Buffer.isBuffer(elf)||elf.length<64||elf.length>64*1024*1024||elf.subarray(0,4).toString('hex')!=='7f454c46'||elf[4]!==1||elf[5]!==1||elf.readUInt16LE(18)!==243)fail('invalid_p4_elf');
 if(!Buffer.isBuffer(image)||image.length<288||image.length>otaBytes||image[0]!==0xe9||image.readUInt32LE(32)!==0xabcd5432)fail('invalid_app_image');
 const versionBytes=image.subarray(48,80),end=versionBytes.indexOf(0);if(end<1)fail('invalid_app_version');
 const encodedVersion=versionBytes.subarray(0,end);if(encodedVersion.some(b=>b>127))fail('invalid_app_version');
 const version=encodedVersion.toString('ascii');if(!/^[A-Za-z0-9_.+-]{1,31}$/.test(version))fail('invalid_app_version');
 const elfSha256=sha(elf);if(image.subarray(176,208).toString('hex')!==elfSha256)fail('elf_image_mismatch');
 const memory=elfMemory(elf);
 const keys=['appBytes','otaBytes','otaHeadroomBytes','internalStaticBytes','externalStaticBytes','minimumOtaHeadroomBytes','maximumInternalStaticBytes'];
 if(!budget||Object.keys(budget).length!==keys.length+2||!keys.every(k=>Object.hasOwn(budget,k)&&Number.isSafeInteger(budget[k])&&budget[k]>=0)
   ||budget.memoryAccounting!=='p4-allocated-ram-v2'||!budget.internalSections||typeof budget.internalSections!=='object'||Array.isArray(budget.internalSections))fail('invalid_budget_record');
 if(budget.appBytes!==image.length||budget.otaBytes!==otaBytes||budget.otaHeadroomBytes!==budget.otaBytes-image.length
  ||budget.minimumOtaHeadroomBytes!==131072||budget.maximumInternalStaticBytes!==393216||budget.otaHeadroomBytes<131072||budget.internalStaticBytes>393216
  ||budget.internalStaticBytes!==memory.internal||budget.externalStaticBytes!==memory.external
  ||JSON.stringify(Object.entries(budget.internalSections).sort())!==JSON.stringify(Object.entries(memory.sections).sort()))fail('firmware_budget_failed');
 return {...(partition?{partitionLayout:partition}:{}),version,elfSha256,imageSha256:sha(image),elfBytes:elf.length,imageBytes:image.length,budget:{...budget,internalSections:{...budget.internalSections}}};
}
export async function buildEvidence({snapshot,run,artifact,save}){
 const before=await snapshot(),report={schema:1,engine:'p4-build-evidence-v1',sourceBefore:before,sourceAfter:null,targets:[],
  status:'running',sourceConsistent:false,hardwareVerified:false,acousticVerified:false,releaseReady:false};save(report);
 if(!before?.valid||!/^[a-f0-9]{64}$/.test(before.digest||'')||!/^[a-f0-9]{40}$/.test(before.commit||'')){report.status='blocked';report.reason='source_snapshot_unavailable';save(report);return report;}
 for(const [id,target] of Object.entries(TARGETS)){
  const result=await run(target,id),item={id,target,status:'failed',exitCode:result.code??null};report.targets.push(item);
  if(result.code!==0||result.signal||result.error){item.reason='build_not_completed';report.status='failed';save(report);break;}
  try{Object.assign(item,await artifact(target),{status:'built'});}catch{item.reason='artifact_verification_failed';report.status='failed';save(report);break;}
  save(report);
 }
 report.sourceAfter=await snapshot();report.sourceConsistent=report.sourceAfter?.valid===true&&report.sourceAfter.digest===before.digest&&report.sourceAfter.commit===before.commit;
 if(!report.sourceConsistent){report.status='blocked';report.reason='source_changed_during_build';}
 else if(report.targets.length===Object.keys(TARGETS).length&&report.targets.every(t=>t.status==='built'))report.status='built_source_consistent';
 save(report);return report;
}
function boundedFile(file,max){const size=statSync(file).size;if(size<1||size>max)fail('artifact_size_invalid');const bytes=readFileSync(file);if(bytes.length!==size||statSync(file).size!==size)fail('artifact_changed');return bytes;}
export async function main(args){
 let output,python;for(let i=0;i<args.length;i++){if(args[i]==='--output'&&args[i+1])output=resolve(args[++i]);else if(args[i]==='--pio-python'&&args[i+1])python=resolve(args[++i]);else fail('explicit_output_and_pio_python_required');}
 if(!output||!python)fail('explicit_output_and_pio_python_required');
 mkdirSync(output,{recursive:true});const directory=mkdtempSync(join(output,'firmware-evidence-'));
 const snapshot=()=>sourceVersion({git:'git'});
 const report=await buildEvidence({snapshot,save:r=>writeFileSync(join(directory,'report.json'),JSON.stringify(r,null,2)+'\n'),
  run:(target,id)=>{
   const r=spawnSync(python,['-m','platformio','run','-e',target],{cwd:join(repo,'firmware'),encoding:'utf8',maxBuffer:16*1024*1024,timeout:600000,
    env:{...process.env,PLATFORMIO_SETTING_ENABLE_TELEMETRY:'No',PLATFORMIO_SETTING_CHECK_PLATFORMIO_INTERVAL:'0',PLATFORMIO_SETTING_CHECK_PLATFORMS_INTERVAL:'0'}});
   writeFileSync(join(directory,`${id}.log`),(r.stdout||'')+'\n'+(r.stderr||''));return {code:r.status,signal:r.signal,error:!!r.error};
  },artifact:target=>{const dir=join(repo,'firmware/.pio/build',target),artifact=firmwareArtifact(boundedFile(join(dir,'firmware.elf'),64*1024*1024),boundedFile(join(dir,'firmware.bin'),3145728),JSON.parse(boundedFile(join(dir,'firmware-budget.json'),2048)),boundedFile(join(dir,'partitions.bin'),4096));
   artifact.rendererStack=rendererStackArtifact(JSON.parse(boundedFile(join(dir,'firmware.stack-budget.json'),32768)),artifact.elfSha256);return artifact;}});
 console.log(JSON.stringify({report:join(directory,'report.json'),status:report.status,hardwareVerified:false,releaseReady:false}));
 return report.status==='built_source_consistent'?0:2;
}
if(process.argv[1]&&resolve(process.argv[1])===fileURLToPath(import.meta.url))main(process.argv.slice(2)).then(code=>{process.exitCode=code;}).catch(()=>{console.error('firmware_evidence_failed');process.exitCode=2;});
