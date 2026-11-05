/** Global status WAV persistence. No video preparation or appearance manifests. */
import {BaseDirectory,exists,mkdir,readFile,readTextFile,writeFile,writeTextFile,rename,remove} from '@tauri-apps/plugin-fs';
import {FAMILIES} from './avatar-pipeline/families.js';
export const GLOBAL_AUDIO_DIR='global-audio-cues';
export const GLOBAL_AUDIO_STATES=Object.freeze([...new Set([...FAMILIES.map(f=>f.family),'idle','permission_request','permission_approved','permission_denied','permission_expired','session_terminated'])]);
const options={baseDir:BaseDirectory.AppLocalData};
const mapPath=`${GLOBAL_AUDIO_DIR}/audio-overrides.json`;
const dirtyKey='pet-manager.global-audio-cues-dirty';
let pendingWrite=Promise.resolve();
function transaction(action){const result=pendingWrite.then(action);pendingWrite=result.catch(()=>{});return result;}
function familyCheck(family){if(!GLOBAL_AUDIO_STATES.includes(family))throw new Error('不支持的提示音状态');}
function validateMap(value){
  if(!value||Array.isArray(value)||typeof value!=='object'||Object.keys(value).length>24)throw new Error('全局提示音设置损坏');
  for(const [family,path] of Object.entries(value)){
    familyCheck(family);
    if(path!==`${GLOBAL_AUDIO_DIR}/audio-overrides/${family}.wav`)throw new Error('全局提示音文件位置无效');
  }
  return value;
}
async function writeMap(value){
  await writeTextFile(`${mapPath}.tmp`,JSON.stringify(validateMap(value)),options);
  await rename(`${mapPath}.tmp`,mapPath,{oldPathBaseDir:BaseDirectory.AppLocalData,newPathBaseDir:BaseDirectory.AppLocalData});
}
export async function readGlobalAudioCues(){
  if(!await exists(mapPath,options))return {};
  return validateMap(JSON.parse(await readTextFile(mapPath,options)));
}
export function globalAudioRevision(){return globalThis.localStorage?.getItem(dirtyKey)||'';}
export function globalAudioDirty(){return Boolean(globalAudioRevision());}
export function markGlobalAudioDirty(dirty,expected){if(dirty)globalThis.localStorage?.setItem(dirtyKey,(globalThis.crypto?.randomUUID?.()||`${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`));else if(expected===undefined||expected===globalAudioRevision())globalThis.localStorage?.removeItem(dirtyKey);}
async function saveCue(family,bytes){
  familyCheck(family);
  if(!(bytes instanceof Uint8Array)||bytes.length<44||bytes.length>1048576+4096)throw new Error('提示音大小不符合要求');
  const overrides=await readGlobalAudioCues();
  const dir=`${GLOBAL_AUDIO_DIR}/audio-overrides`;
  await mkdir(dir,{...options,recursive:true});
  const path=`${dir}/${family}.wav`;
  await writeFile(`${path}.tmp`,bytes,options);
  await rename(`${path}.tmp`,path,{oldPathBaseDir:BaseDirectory.AppLocalData,newPathBaseDir:BaseDirectory.AppLocalData});
  overrides[family]=path;
  await writeMap(overrides);markGlobalAudioDirty(true);
}
async function removeCue(family){
  familyCheck(family);const overrides=await readGlobalAudioCues();const path=overrides[family];
  delete overrides[family];await mkdir(GLOBAL_AUDIO_DIR,{...options,recursive:true});
  await writeMap(overrides);markGlobalAudioDirty(true);
  if(path)await remove(path,options).catch(()=>{});
}
export async function readGlobalAudioCue(family){
  familyCheck(family);const map=await readGlobalAudioCues();return map[family]?readFile(map[family],options):null;
}
// Preserve an existing built-in cue edit once; never choose an arbitrary custom
// appearance or alter its files. Existing global settings always win.
async function migrateCues(){
  if(await exists(mapPath,options))return;
  const legacy='custom-appearances/builtin-terrier/audio-overrides.json';
  if(!await exists(legacy,options))return;
  const map=JSON.parse(await readTextFile(legacy,options));
  if(!map||Array.isArray(map)||typeof map!=='object'||Object.keys(map).length>24)throw new Error('原提示音设置损坏，未自动迁移');
  const staged=[];
  for(const [family,path] of Object.entries(map)){
    familyCheck(family);
    if(path!==`custom-appearances/builtin-terrier/audio-overrides/${family}.wav`)throw new Error('原提示音文件位置无效');
    const bytes=await readFile(path,options);
    if(bytes.length<44||bytes.length>1048576+4096)throw new Error('原提示音大小不符合要求');
    staged.push({family,bytes});
  }
  if(!staged.length)return;
  await mkdir(`${GLOBAL_AUDIO_DIR}/audio-overrides`,{...options,recursive:true});
  const migrated={};
  for(const {family,bytes} of staged){
    const path=`${GLOBAL_AUDIO_DIR}/audio-overrides/${family}.wav`;
    await writeFile(`${path}.tmp`,bytes,options);
    await rename(`${path}.tmp`,path,{oldPathBaseDir:BaseDirectory.AppLocalData,newPathBaseDir:BaseDirectory.AppLocalData});
    migrated[family]=path;
  }
  await writeMap(migrated);markGlobalAudioDirty(true);
}

export function saveGlobalAudioCue(family,bytes){return transaction(()=>saveCue(family,bytes));}
export function removeGlobalAudioCue(family){return transaction(()=>removeCue(family));}
export function migrateBuiltinGlobalAudioCues(){return transaction(migrateCues);}
