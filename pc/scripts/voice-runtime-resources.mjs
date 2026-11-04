/** Offline, explicit voice-service resource staging/integrity; never installs or starts services. */
import {createHash} from 'node:crypto';
import {existsSync,lstatSync,mkdirSync,readdirSync,readFileSync,renameSync,writeFileSync} from 'node:fs';
import {dirname,join,relative,resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
const sha=b=>createHash('sha256').update(b).digest('hex');
const fail=message=>{throw Error(`voice-service runtime: ${message}`);};
const manifestName='runtime-manifest.json';
function regular(root,name){
  if(typeof name!=='string'||!name||name.includes('\\')||name.startsWith('/')||name.split('/').some(p=>!p||p==='.'||p==='..'))fail('unsafe resource path');
  let p=root;for(const part of name.split('/')){p=join(p,part);if(lstatSync(p).isSymbolicLink())fail('symlink resources are forbidden');}
  if(!lstatSync(p).isFile())fail('resource is not a file');return p;
}
function files(root,prefix=''){
 const result=[];for(const e of readdirSync(join(root,prefix),{withFileTypes:true})){
  const name=prefix?`${prefix}/${e.name}`:e.name;
  if(e.isSymbolicLink())fail('symlink resources are forbidden');
  if(e.isDirectory())result.push(...files(root,name));else if(e.isFile())result.push(name);else fail('nonregular resource');
 }return result.sort();
}
export function verifyVoiceRuntime(root,{target,arch,approvedManifestSha256}={}){
 root=resolve(root);if(!existsSync(root)||lstatSync(root).isSymbolicLink()||!lstatSync(root).isDirectory())fail('missing reviewed source: supply PET_MANAGER_VOICE_SERVICE_SOURCE and PET_MANAGER_VOICE_SERVICE_MANIFEST_SHA256; no network fallback');
 if(!['macos','windows','linux'].includes(target)||!['arm64','x64'].includes(arch))fail('explicit supported target/architecture required');
 const manifestBytes=readFileSync(regular(root,manifestName)),manifestSha256=sha(manifestBytes);
 if(approvedManifestSha256&&manifestSha256!==approvedManifestSha256)fail('reviewed manifest SHA256 mismatch');
 const m=JSON.parse(manifestBytes);
 if(m.schema!==1||m.service!=='voice-service-node'||m.entry!=='src/index.mjs'||!['any',`${target}-${arch}`].includes(m.target)||!m.files||Array.isArray(m.files)||typeof m.files!=='object')fail('invalid reviewed runtime manifest/target');
 const names=Object.keys(m.files).sort();
 for(const required of ['src/index.mjs','package.json','package-lock.json'])if(!names.includes(required))fail(`manifest missing ${required}`);
 let totalBytes=0;
 for(const name of names){
  if(!(/^(src|node_modules)\//.test(name)||['package.json','package-lock.json','LICENSE','LICENSE.txt','LICENSE.md','NOTICE','NOTICE.txt','THIRD_PARTY_NOTICES.md'].includes(name)))fail('unreviewed top-level resource');
  if(name.split('/').some(p=>/^(\.env(?:\..*)?|\.npmrc|\.git|\.cache|\.bin|__pycache__|tests?|__tests__|coverage)$/i.test(p))||/\.(pem|key|p12|pfx|log|node|dylib|dll|exe|pyc)$/i.test(name))fail('private/development/native resource forbidden');
  if(!/^[a-f0-9]{64}$/.test(m.files[name]))fail('invalid per-file SHA256');
  const data=readFileSync(regular(root,name));totalBytes+=data.length;
  if(sha(data)!==m.files[name])fail(`resource checksum mismatch: ${name}`);
 }
 if(names.length>10000||totalBytes>64*1024*1024)fail('reviewed runtime exceeds bounded packaging size');
 const actual=files(root).filter(n=>n!==manifestName);
 if(JSON.stringify(actual)!==JSON.stringify(names))fail('source/stage contains unlisted files; do not copy a development checkout');
 const pkg=JSON.parse(readFileSync(join(root,'package.json'),'utf8'));
 const lock=JSON.parse(readFileSync(join(root,'package-lock.json'),'utf8'));
 if(pkg.type!=='module'||!lock.packages||![2,3].includes(lock.lockfileVersion)||lock.packages['']?.name!==pkg.name||lock.packages['']?.version!==pkg.version)fail('missing exact production dependency lock');
 const ordered=v=>JSON.stringify(Object.entries(v||{}).sort(([a],[b])=>a.localeCompare(b)));
 if(ordered(pkg.dependencies)!==ordered(lock.packages[''].dependencies))fail('root production dependencies differ from lock');
 const included=new Map();
 for(const name of names.filter(n=>n.startsWith('node_modules/')&&n.endsWith('/package.json'))){
  const dir=name.slice(0,-'/package.json'.length),entry=lock.packages[dir];
  if(!entry||entry.dev||entry.link)fail('unlocked/development/linked dependency');
  const d=JSON.parse(readFileSync(join(root,name),'utf8'));
  if(d.version!==entry.version||ordered(d.dependencies)!==ordered(entry.dependencies))fail('dependency version/requirements differ from lock');
  const compatible=(values,value)=>!values||Array.isArray(values)&&!values.includes(`!${value}`)&&(!values.some(v=>!v.startsWith('!'))||values.includes(value));
  const os=({macos:'darwin',windows:'win32',linux:'linux'})[target];
  if(!compatible(d.os,os)||!compatible(d.cpu,arch)||!compatible(entry.os,os)||!compatible(entry.cpu,arch))fail('dependency is incompatible with desktop target');
  included.set(dir,{pkg:d,lock:entry});
 }
 function dependency(owner,name){
  if(!/^(?:@[a-z0-9._-]+\/)?[a-z0-9._-]+$/i.test(name))fail('invalid dependency name');
  let cursor=owner;
  for(;;){const key=cursor?`${cursor}/node_modules/${name}`:`node_modules/${name}`;if(included.has(key))return key;
   if(!cursor)break;const i=cursor.lastIndexOf('/node_modules/');cursor=i<0?'':cursor.slice(0,i);}
  fail(`missing locked production dependency: ${name}`);
 }
 const reachable=new Set();const visit=(owner,d)=>{
 const required={...(d.dependencies||{}),...Object.fromEntries(Object.entries(d.peerDependencies||{}).filter(([name])=>!d.peerDependenciesMeta?.[name]?.optional))};
 for(const name of Object.keys(required)){
  const key=dependency(owner,name);if(!reachable.has(key)){reachable.add(key);visit(key,included.get(key).pkg);}
 }};
 visit('',pkg);
 // Optional dependencies may be omitted. If explicitly reviewed/included,
 // validate their required dependency closure too; never install them here.
 for(const [dir,d] of included)visit(dir,d.pkg);
 for(const name of names.filter(n=>n.startsWith('node_modules/'))){
  let owner='';for(const key of included.keys())if(name.startsWith(`${key}/`)&&key.length>owner.length)owner=key;
  if(!owner)fail('dependency file lacks reviewed package/lock metadata');
 }
 return {service:m.service,entry:m.entry,target:m.target,manifestSha256,files:names.length,bytes:totalBytes,dependencies:included.size,networkUsed:false,serviceStarted:false};
}
export function prepareVoiceRuntime({tauriRoot,target,arch}){
 const destination=join(tauriRoot,'generated-runtime','voice-service');
 const source=process.env.PET_MANAGER_VOICE_SERVICE_SOURCE;
 const approved=process.env.PET_MANAGER_VOICE_SERVICE_MANIFEST_SHA256;
 if(!source){
  // P4 uses built-in native ASR; the historical LiveKit service is optional.
  return {enabled:false,requiredForP4:false,resourceDirectory:null};
 }
 if(!/^[a-f0-9]{64}$/.test(approved||''))fail('explicit source requires its reviewed manifest SHA256');
 const info=verifyVoiceRuntime(source,{target,arch,approvedManifestSha256:approved});
 if(existsSync(destination)){
  const previous=verifyVoiceRuntime(destination,{target,arch});
  if(previous.manifestSha256!==info.manifestSha256)fail('different staged runtime exists; retain it and coordinate explicit replacement');
  return {...previous,enabled:true,resourceDirectory:'voice-service',tauriConfig:'src-tauri/tauri.voice-legacy.conf.json',reused:true};
 }
 const staging=join(tauriRoot,'generated-runtime',`voice-service-staging-${process.pid}-${Date.now()}`);mkdirSync(staging,{recursive:true});
 for(const name of [...Object.keys(JSON.parse(readFileSync(join(source,manifestName),'utf8')).files),manifestName]){
  const dest=join(staging,name);mkdirSync(dirname(dest),{recursive:true});writeFileSync(dest,readFileSync(regular(resolve(source),name)),{flag:'wx'});
 }
 verifyVoiceRuntime(staging,{target,arch,approvedManifestSha256:approved});renameSync(staging,destination);
 return {...info,enabled:true,resourceDirectory:'voice-service',tauriConfig:'src-tauri/tauri.voice-legacy.conf.json',reused:false};
}
export function verifyVoiceBundle(resourceRoot,{target,arch}){
 const directory=join(resourceRoot,'voice-service');
 if(!existsSync(directory))return {enabled:false,requiredForP4:false,runtimeStartVerified:false,windowsMachineVerified:false};
 const result=verifyVoiceRuntime(directory,{target,arch});
 const node=join(resourceRoot,'bridge/runtime',target==='windows'?'node.exe':'node');
 const bytes=readFileSync(regular(resourceRoot,relative(resourceRoot,node).split('\\').join('/')));
 if(target==='windows'){
  if(bytes.length<0x40||bytes.toString('ascii',0,2)!=='MZ')fail('missing Windows Node PE');
  const offset=bytes.readUInt32LE(0x3c);if(offset+6>bytes.length||bytes.toString('ascii',offset,offset+4)!=='PE\0\0'||bytes.readUInt16LE(offset+4)!==0x8664)fail('Windows Node must be x64 PE');
 }
 if(target==='macos'){
  if(bytes.length<8||bytes.readUInt32LE(0)!==0xfeedfacf||bytes.readUInt32LE(4)!==(arch==='arm64'?0x0100000c:0x01000007))fail('bundled Node is not the reviewed thin target Mach-O');
 }
 return {...result,nodeSha256:sha(bytes),nodeBytes:bytes.length,bundleResourcesVerified:true,runtimeStartVerified:false,windowsMachineVerified:false};
}
if(process.argv[1]&&resolve(process.argv[1])===fileURLToPath(import.meta.url)){
 const args=process.argv.slice(2);if(args.length!==3)throw Error('usage: voice-runtime-resources.mjs RESOURCE_ROOT macos|windows arm64|x64');
 console.log(JSON.stringify(verifyVoiceBundle(resolve(args[0]),{target:args[1],arch:args[2]})));
}
