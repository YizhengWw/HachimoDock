import {readFileSync} from 'node:fs';
import test from 'node:test';import assert from 'node:assert/strict';import {createHash} from 'node:crypto';
import {firmwareArtifact,buildEvidence,TARGETS} from './build-framework-evidence.mjs';
import {rendererStackArtifact} from './framework-stack-evidence.mjs';
const source={valid:true,commit:'a'.repeat(40),digest:'b'.repeat(64),scope:{entireRepository:false}};
function sample(){const elf=Buffer.alloc(1536);elf.write('ELF',1,'ascii');elf[0]=0x7f;elf[4]=elf[5]=1;elf.writeUInt16LE(243,18);
 elf.writeUInt32LE(128,32);elf.writeUInt16LE(40,46);elf.writeUInt16LE(9,48);elf.writeUInt16LE(8,50);
 const names='\0.iram0.text\0.dram0.data\0.dram0.bss\0.dram1.bss\0.tcm.text\0.rtc.text\0.ext_ram.bss\0.shstrtab\0';elf.write(names,512,'ascii');
 for(const [i,name,size,type,offset,address,flags] of [[1,'.iram0.text',64,1,1024,0x4ff00000,6],[2,'.dram0.data',96,1,1088,0x4ff01000,3],[3,'.dram0.bss',128,8,0,0x4ff02000,3],[4,'.dram1.bss',256,8,0,0x4ff40000,3],[5,'.tcm.text',32,1,1184,0x30100000,6],[6,'.rtc.text',16,1,1216,0x50108000,6],[7,'.ext_ram.bss',1024,8,0,0x48240000,3],[8,'.shstrtab',names.length,3,512,0,0]]){const at=128+i*40;elf.writeUInt32LE(names.indexOf(name),at);elf.writeUInt32LE(type,at+4);elf.writeUInt32LE(flags,at+8);elf.writeUInt32LE(address,at+12);elf.writeUInt32LE(offset,at+16);elf.writeUInt32LE(size,at+20);}
 const image=Buffer.alloc(1024);image[0]=0xe9;image.writeUInt32LE(0xabcd5432,32);image.write('0.7.103-p4-r27',48,'ascii');Buffer.from(createHash('sha256').update(elf).digest()).copy(image,176);
 const budget={appBytes:1024,otaBytes:2621440,otaHeadroomBytes:2620416,internalStaticBytes:592,externalStaticBytes:1024,minimumOtaHeadroomBytes:131072,maximumInternalStaticBytes:393216,
  memoryAccounting:'p4-allocated-ram-v2',internalSections:{'.iram0.text':64,'.dram0.data':96,'.dram0.bss':128,'.dram1.bss':256,'.tcm.text':32,'.rtc.text':16}};return {elf,image,budget};}
test('build evidence binds the actual image descriptor to its exact RISC-V ELF',()=>{const {elf,image,budget}=sample(),a=firmwareArtifact(elf,image,budget);assert.equal(a.version,'0.7.103-p4-r27');assert.equal(a.imageBytes,1024);const bad=Buffer.from(image);bad[176]^=1;assert.throws(()=>firmwareArtifact(elf,bad,budget),/elf_image_mismatch/);});
test('wrong image architecture, invalid descriptor, forged budget and truncation fail closed',()=>{const s=sample();for(const offset of [0,4,5,18]){const bad=Buffer.from(s.elf);bad[offset]^=1;assert.throws(()=>firmwareArtifact(bad,s.image,s.budget));}
 for(const offset of [0,32,48]){const bad=Buffer.from(s.image);bad[offset]=255;assert.throws(()=>firmwareArtifact(s.elf,bad,s.budget));}
 for(const key of ['appBytes','otaHeadroomBytes','minimumOtaHeadroomBytes','maximumInternalStaticBytes','internalStaticBytes'])assert.throws(()=>firmwareArtifact(s.elf,s.image,{...s.budget,[key]:0}));
 assert.throws(()=>firmwareArtifact(s.elf,s.image.subarray(0,200),s.budget));});
test('all allocated P4 RAM banks, TCM and RTC contribute to the unchanged budget',()=>{
 const s=sample();assert.equal(firmwareArtifact(s.elf,s.image,s.budget).budget.internalStaticBytes,592);
 const old={...s.budget,internalStaticBytes:288};delete old.memoryAccounting;delete old.internalSections;assert.throws(()=>firmwareArtifact(s.elf,s.image,old),/invalid_budget_record/);
 assert.throws(()=>firmwareArtifact(s.elf,s.image,{...s.budget,internalSections:{'.iram0.text':592}}),/firmware_budget_failed/);
 for(const mutate of [elf=>elf.writeUInt32LE(0x4ffbfff0,128+4*40+12),elf=>elf.writeUInt32LE(0x4ff00000,128+4*40+12),elf=>elf.writeUInt32LE(400000,128+4*40+20)]){
  const elf=Buffer.from(s.elf),image=Buffer.from(s.image);mutate(elf);createHash('sha256').update(elf).digest().copy(image,176);assert.throws(()=>firmwareArtifact(elf,image,s.budget));
 }
});
test('only the fixed compilation targets are allowed (normal, ui, public program candidate); successful builds never imply hardware or release',async()=>{const called=[],saved=[];const r=await buildEvidence({snapshot:()=>source,run:target=>{called.push(target);return {code:0};},artifact:()=>firmwareArtifact(...Object.values(sample())),save:r=>saved.push(structuredClone(r))});assert.deepEqual(called,Object.values(TARGETS));assert.equal(r.status,'built_source_consistent');assert.equal(r.releaseReady,false);assert.equal(r.hardwareVerified,false);assert.equal(saved.at(-1).sourceConsistent,true);});
test('failure or signal cannot fall back to a stale existing binary',async()=>{for(const result of [{code:1},{code:0,signal:'SIGTERM'},{code:null,error:true}]){let read=0;const r=await buildEvidence({snapshot:()=>source,run:()=>result,artifact:()=>{read++;return {};},save:()=>{}});assert.equal(read,0);assert.equal(r.status,'failed');assert.equal(r.targets.length,1);}});
test('changed source invalidates otherwise successful builds',async()=>{let n=0;const r=await buildEvidence({snapshot:()=>({...source,digest:(n++?'c':'b').repeat(64)}),run:()=>({code:0}),artifact:()=>({}),save:()=>{}});assert.equal(r.status,'blocked');assert.equal(r.sourceConsistent,false);assert.equal(r.reason,'source_changed_during_build');});
function stackSample(){return {schema:1,engine:'p4-render-stack-v1',elfSha256:'a'.repeat(64),budgetBytes:4096,allCallEdges:false,maxReviewedBytes:512,
 paths:['media_text','app_label_width','app_label_icon','data_label_width','data_label_icon','snapshot_host','snapshot_form_text','snapshot_form_values','snapshot_form_availability','render_metrics'].map(id=>({id,bytes:512,frames:[{module:'pet_p4_renderer',function:'pet_p4_renderer_render',bytes:256,emitted:true},{module:'pet_p4_renderer',function:'render_miniapp_page',bytes:256,emitted:true}]}))};}
test('v2 stack evidence includes the actual host collection resolver while retaining read-only v1 compatibility',()=>{
 const old=stackSample();assert.equal(rendererStackArtifact(old,old.elfSha256).schema,1);
 const s=stackSample();s.schema=2;s.engine='p4-render-stack-v2';s.paths.push({id:'snapshot_host_collection',bytes:512,frames:[
  {module:'pet_p4_renderer',function:'pet_p4_renderer_render',bytes:256,emitted:true},
  {module:'pet_p4_surface',function:'pet_surface_resolve_collection',bytes:224,emitted:true},
  {module:'pet_p4_host_collection',function:'pet_host_collection_matches',bytes:32,emitted:true}]});
 assert.equal(rendererStackArtifact(s,s.elfSha256).schema,2);
 const incomplete=structuredClone(s);incomplete.paths.pop();assert.throws(()=>rendererStackArtifact(incomplete,incomplete.elfSha256));
 s.schema=1;s.engine='p4-render-stack-v1';assert.throws(()=>rendererStackArtifact(s,s.elfSha256));
});
test('static-path reports bind the exact ELF and never claim complete runtime stack coverage',()=>{const s=stackSample(),out=rendererStackArtifact(s,'a'.repeat(64));assert.deepEqual(out,s);assert.equal(out.allCallEdges,false);});
test('v3 requires the three native async render paths in addition to all original paths',()=>{
 const s=stackSample();s.schema=3;s.engine='p4-render-stack-v3';
 for(const id of ['snapshot_host_collection','async_asset_pack','async_asset_packed_fallback','async_asset_i420_fallback'])s.paths.push({...structuredClone(s.paths[0]),id});
 assert.equal(rendererStackArtifact(s,s.elfSha256).paths.length,14);
 for(const id of ['media_text','snapshot_host_collection','async_asset_pack','async_asset_packed_fallback','async_asset_i420_fallback']){
  const missing=structuredClone(s);missing.paths=missing.paths.filter(p=>p.id!==id);assert.throws(()=>rendererStackArtifact(missing,missing.elfSha256));
 }
});
test('v4 requires panel and visibility paths and keeps historical schemas valid',()=>{
 const s=stackSample();s.schema=4;s.engine='p4-render-stack-v4';
 const added=['snapshot_host_collection','async_asset_pack','async_asset_packed_fallback','async_asset_i420_fallback','app_panel_gradient','app_cache_visibility','clip_frame_visibility'];
 for(const id of added)s.paths.push({...structuredClone(s.paths[0]),id});
 assert.equal(rendererStackArtifact(s,s.elfSha256).paths.length,17);
 for(const id of added){const missing=structuredClone(s);missing.paths=missing.paths.filter(p=>p.id!==id);assert.throws(()=>rendererStackArtifact(missing,missing.elfSha256));}
 const oldSchema=structuredClone(s);oldSchema.schema=3;oldSchema.engine='p4-render-stack-v3';assert.throws(()=>rendererStackArtifact(oldSchema,oldSchema.elfSha256));
 const oversized=structuredClone(s);oversized.budgetBytes=4097;assert.throws(()=>rendererStackArtifact(oversized,oversized.elfSha256));
});
test('forged, oversized, duplicate or path-bearing static stack records fail closed',()=>{for(const mutate of [s=>s.elfSha256='b'.repeat(64),s=>s.maxReviewedBytes=4097,s=>s.allCallEdges=true,s=>s.paths[0].bytes=1,s=>s.paths[0].frames[0].emitted=false,s=>s.paths[1].id=s.paths[0].id,s=>s.paths[0].frames[0].module='SECRET/private/path',s=>s.extra='SECRET',s=>s.paths[0].frames[0].extra='SECRET']){const s=stackSample();mutate(s);assert.throws(()=>rendererStackArtifact(s,'a'.repeat(64)));}});

test('larger OTA budget requires the actual checksum-valid generated partition table',()=>{
 const lines=readFileSync(new URL('../../firmware/partitions.csv',import.meta.url),'utf8').split('\n').filter(s=>s.trim()&&!s.trim().startsWith('#'));
 const table=Buffer.alloc(3072,255),subtypes={nvs:2,phy:1,ota:0,ota_0:16,ota_1:17,spiffs:130};
 lines.forEach((line,i)=>{const [name,type,sub,address,size]=line.split(',').map(x=>x.trim());const p=i*32;table.writeUInt16LE(0x50aa,p);table[p+2]=type==='app'?0:1;table[p+3]=subtypes[sub]??Number(sub);table.writeUInt32LE(Number(address),p+4);table.writeUInt32LE(Number(size),p+8);table.fill(0,p+12,p+28);table.write(name,p+12);table.writeUInt32LE(0,p+28);});
 const end=lines.length*32;table.writeUInt16LE(0xebeb,end);createHash('md5').update(table.subarray(0,end)).digest().copy(table,end+16);
 const {elf,image,budget}=sample(),expanded={...budget,otaBytes:3145728,otaHeadroomBytes:3145728-image.length};
 assert.throws(()=>firmwareArtifact(elf,image,expanded),/firmware_budget_failed/);
 assert.throws(()=>firmwareArtifact(elf,image,budget,table),/firmware_budget_failed/);
 const artifact=firmwareArtifact(elf,image,expanded,table);assert.equal(artifact.partitionLayout.otaBytes,3145728);assert.equal(artifact.partitionLayout.md5Verified,true);
 const corrupt=Buffer.from(table);corrupt[32]^=1;assert.throws(()=>firmwareArtifact(elf,image,expanded,corrupt));
 assert.throws(()=>firmwareArtifact(elf,image,{...expanded,minimumOtaHeadroomBytes:131071},table),/firmware_budget_failed/);
});
