/** Exact desktop artifact paths/identity. No build, signing, mount or network. */
import {createHash} from 'node:crypto';
import {readFileSync,statSync} from 'node:fs';
import {resolve,join,basename,isAbsolute} from 'node:path';
import {spawnSync} from 'node:child_process';
const fail=message=>{throw Error(`desktop artifact: ${message}`);};
const sha=bytes=>createHash('sha256').update(bytes).digest('hex');

export function desktopBuildPaths({repositoryRoot,target,env=process.env,cwd=process.cwd()}){
  if(!['macos','windows'].includes(target))fail('unsupported desktop target');
  const tauriRoot=resolve(repositoryRoot,'pc/src-tauri');
  const targetDirectory=env.CARGO_TARGET_DIR?resolve(cwd,env.CARGO_TARGET_DIR):join(tauriRoot,'target');
  const triple=target==='windows'?'x86_64-pc-windows-msvc':(env.CARGO_BUILD_TARGET||'');
  if(triple&&!/^[A-Za-z0-9_.-]+$/.test(triple))fail('invalid Cargo target triple');
  const releaseRoot=join(targetDirectory,...(triple?[triple]:[]),'release');
  return {targetDirectory,releaseRoot,bundleRoot:join(releaseRoot,'bundle')};
}
function versionTuple(version){
  const m=/^(\d+)\.(\d+)\.(\d+)(?:[-+][0-9A-Za-z.-]+)?$/.exec(version);
  if(!m||m.slice(1).some(n=>Number(n)>65535))fail('invalid expected version');
  return [...m.slice(1).map(Number),0];
}
function plistField(file,key){
  const result=spawnSync('/usr/bin/plutil',['-extract',key,'raw','-o','-',file],{encoding:'utf8',timeout:10000});
  if(result.error||result.status!==0)fail(`missing Info.plist ${key}`);
  return result.stdout.trim();
}
export function macAppIdentity(appPath,version,{readField=plistField}={}){
  versionTuple(version);
  const plist=join(appPath,'Contents/Info.plist');
  if(readField(plist,'CFBundleShortVersionString')!==version)fail('app version differs from requested package version');
  const executable=readField(plist,'CFBundleExecutable');
  if(!executable||basename(executable)!==executable||!/^[A-Za-z0-9_.-]+$/.test(executable))fail('unsafe bundle executable');
  const binary=join(appPath,'Contents/MacOS',executable);
  if(!statSync(binary).isFile())fail('missing app executable');
  return {schema:1,target:'macos',version,path:resolve(appPath),executable,sha256:sha(readFileSync(binary))};
}
// Read RT_VERSION/VS_FIXEDFILEINFO from PE resources, not an arbitrary version
// string elsewhere in the executable. No execution of a Windows binary.
export function peProductVersion(bytes){
  const range=(at,n)=>Number.isSafeInteger(at)&&at>=0&&n>=0&&at+n<=bytes.length;
  if(!range(0,64)||bytes.toString('ascii',0,2)!=='MZ')fail('invalid PE image');
  const pe=bytes.readUInt32LE(60);
  if(!range(pe,24)||bytes.toString('ascii',pe,pe+4)!=='PE\0\0'||bytes.readUInt16LE(pe+4)!==0x8664)fail('expected Windows x64 PE');
  const count=bytes.readUInt16LE(pe+6),optional=pe+24,optionalSize=bytes.readUInt16LE(pe+20);
  if(!range(optional,optionalSize)||optionalSize<136||bytes.readUInt16LE(optional)!==0x20b)fail('invalid PE32+ optional header');
  const resourceRva=bytes.readUInt32LE(optional+128),resourceSize=bytes.readUInt32LE(optional+132);
  const sections=optional+optionalSize;
  if(!range(sections,count*40))fail('invalid PE sections');
  const fileOffset=(rva,length)=>{
    for(let i=0;i<count;i++){
      const at=sections+i*40,start=bytes.readUInt32LE(at+12),size=bytes.readUInt32LE(at+16),raw=bytes.readUInt32LE(at+20);
      if(rva>=start&&rva-start+length<=size&&range(raw+rva-start,length))return raw+rva-start;
    }fail('PE resource lies outside file-backed section');
  };
  if(!resourceRva||!resourceSize)fail('PE has no version resources');
  const root=fileOffset(resourceRva,resourceSize);
  const resourceRange=(at,n)=>at>=root&&at+n<=root+resourceSize&&range(at,n);
  const entries=offset=>{
    const at=root+offset;if(!resourceRange(at,16))fail('invalid resource directory');
    const n=bytes.readUInt16LE(at+12)+bytes.readUInt16LE(at+14);
    if(n>4096||!resourceRange(at+16,n*8))fail('invalid resource entries');
    return Array.from({length:n},(_,i)=>({id:bytes.readUInt32LE(at+16+i*8),next:bytes.readUInt32LE(at+20+i*8)}));
  };
  const type=entries(0).find(e=>e.id===16);
  if(!type||(type.next&0x80000000)===0)fail('missing RT_VERSION');
  const versions=[];
  for(const name of entries(type.next&0x7fffffff)){
    if(!(name.next&0x80000000))fail('invalid version name');
    for(const language of entries(name.next&0x7fffffff)){
      if(language.next&0x80000000)fail('invalid version leaf');
      const data=root+language.next;if(!resourceRange(data,16))fail('invalid version resource data');
      const size=bytes.readUInt32LE(data+4),start=fileOffset(bytes.readUInt32LE(data),size);
      if(size<6)fail('truncated VS_VERSION_INFO');
      const length=bytes.readUInt16LE(start),valueLength=bytes.readUInt16LE(start+2);
      if(length>size||valueLength<52)fail('invalid VS_VERSION_INFO length');
      let at=start+6,key='';
      while(at+2<=start+length){const c=bytes.readUInt16LE(at);at+=2;if(!c)break;key+=String.fromCharCode(c);}
      at=Math.ceil(at/4)*4;
      if(key!=='VS_VERSION_INFO'||at+52>start+length||bytes.readUInt32LE(at)!==0xfeef04bd)fail('missing fixed version information');
      const ms=bytes.readUInt32LE(at+16),ls=bytes.readUInt32LE(at+20);
      versions.push([ms>>>16,ms&65535,ls>>>16,ls&65535]);
    }
  }
  if(!versions.length||versions.some(v=>v.join('.')!==versions[0].join('.')))fail('inconsistent PE product versions');
  return versions[0];
}
export function windowsExeIdentity(path,version){
  const bytes=readFileSync(path),actual=peProductVersion(bytes),expected=versionTuple(version);
  if(actual.join('.')!==expected.join('.'))fail('PE product version differs from requested package version');
  // PE fixed versions do not represent semver prerelease/build suffixes. Do
  // not pretend they bind an unsupported qualified Windows package label.
  if(version!==expected.slice(0,3).join('.'))fail('qualified Windows version requires a separate reviewed version contract');
  return {schema:1,target:'windows',version,path:resolve(path),sha256:sha(bytes)};
}
export function verifyBuildReceipt(identity,receipt,{relocated=false}={}){
  if(!receipt||Object.keys(receipt).some(k=>!['schema','target','version','path','executable','sha256'].includes(k))
    ||receipt.schema!==1||typeof receipt.path!=='string'||!isAbsolute(receipt.path)||!(/^[a-f0-9]{64}$/.test(receipt.sha256||''))
    ||receipt.target!==identity.target||receipt.version!==identity.version
    ||receipt.sha256!==identity.sha256||receipt.executable!==identity.executable
    ||(!relocated&&receipt.path!==identity.path))fail('build receipt does not match selected artifact');
  return identity;
}
export function readBuildReceipt(path){return JSON.parse(readFileSync(path,'utf8'));}
export function receiptArgument(args){
  const i=args.indexOf('--receipt');
  if(i<0)return process.env.PET_MANAGER_BUILD_RECEIPT||null;
  if(!args[i+1]||args[i+1].startsWith('--'))fail('missing receipt path');
  return resolve(args[i+1]);
}
