// Build the fixed product collection model, not user-supplied executable code.
import {spawnSync} from 'node:child_process';
import {readFileSync,writeFileSync,copyFileSync,mkdtempSync,rmSync} from 'node:fs';
import {resolve,dirname,join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {tmpdir} from 'node:os';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'../..');
if(process.argv.slice(2).some(a=>a!=='--check'))throw Error('Only --check is supported');
const check=process.argv.includes('--check'),temporary=mkdtempSync(join(tmpdir(),'pet-collection-'));
const api=['cv_buffer','cv_seed_key','cv_stride','cv_load','cv_apply','cv_state_offset','cv_state_key',
 'cv_text','cv_metric','cv_selected_index','cv_row_text','cv_row_metric'];
try{
 const output=join(temporary,'collection-core.wasm');
 const sources=[join(root,'firmware/main/pet_p4_collection.c'),...['collection_bridge.c','collection_memory.c'].map(n=>join(root,'pc/scripts/game-wasm',n))];
 const result=spawnSync(process.env.CLANG||'clang',['--target=wasm32','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-O2','-ffreestanding','-fno-builtin','-nostdlib','-Wl,--no-entry','-Wl,--export-memory',...api.map(name=>'-Wl,--export='+name),'-Wl,--initial-memory=262144','-Wl,--max-memory=262144','-Wl,-z,stack-size=16384','-Wl,--strip-all','-I'+join(root,'pc/scripts/game-wasm'),'-I'+join(root,'firmware/main'),...sources,'-o',output],{stdio:'inherit'});
 if(result.status!==0)throw Error('Shared collection compiler failed');
 const wasm=readFileSync(output),module=new WebAssembly.Module(wasm);
 if(WebAssembly.Module.imports(module).length)throw Error('Collection must have zero imports');
 if(JSON.stringify(WebAssembly.Module.exports(module).map(item=>item.name).sort())!==JSON.stringify(['memory',...api].sort()))throw Error('Collection export surface changed');
 const e=new WebAssembly.Instance(module,{}).exports;
 if(e.memory.buffer.byteLength!==262144||e.cv_stride()!==1090)throw Error('Collection memory/layout budget changed');
 try{e.memory.grow(1);throw Error('Collection memory grew');}catch(error){if(!(error instanceof RangeError))throw error;}
 const bytes='// Generated from the fixed device collection model, not package code.\nexport const encodedCollectionCore='+JSON.stringify(wasm.toString('base64'))+';\n';
 for(const directory of ['pc/src/component-center','pc/skills/petui/scripts']){
  const binary=join(root,directory,'collection-core.wasm'),encoded=join(root,directory,'collection-bytes.mjs');
  if(check){if(!wasm.equals(readFileSync(binary))||bytes!==readFileSync(encoded,'utf8'))throw Error('Collection artifact is stale');}
  else{copyFileSync(output,binary);writeFileSync(encoded,bytes);}
 }
 const adapter=readFileSync(join(root,'pc/src/component-center/collection-core.js'));
 const mirror=join(root,'pc/skills/petui/scripts/collection-core.mjs');
 if(check){if(!adapter.equals(readFileSync(mirror)))throw Error('Collection adapter is stale');}else writeFileSync(mirror,adapter);
}finally{rmSync(temporary,{recursive:true});}
