// Existing native form state machine, never compile or execute package code.
import {spawnSync} from 'node:child_process';
import {readFileSync,writeFileSync,copyFileSync,mkdtempSync,rmSync} from 'node:fs';
import {resolve,dirname,join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {tmpdir} from 'node:os';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'../..');
if(process.argv.slice(2).some(a=>a!=='--check'))throw Error('Only --check is supported');
const check=process.argv.includes('--check'),temporary=mkdtempSync(join(tmpdir(),'pet-legacy-form-'));
try{
 const output=join(temporary,'form-v1-core.wasm');
 const result=spawnSync(process.env.CLANG||'clang',['--target=wasm32','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-O2','-ffreestanding','-fno-builtin','-nostdlib','-Wl,--no-entry','-Wl,--export-all','-Wl,--initial-memory=65536','-Wl,--max-memory=65536','-Wl,-z,stack-size=16384','-Wl,--strip-all','-I'+join(root,'pc/scripts/game-wasm'),'-I'+join(root,'firmware/main'),join(root,'pc/scripts/game-wasm/form_v1_bridge.c'),'-o',output],{stdio:'inherit'});
 if(result.status!==0)throw Error('Native legacy form compiler failed');
 const wasm=readFileSync(output),module=new WebAssembly.Module(wasm);
 if(WebAssembly.Module.imports(module).length)throw Error('Legacy form has unexpected imports');
 const e=new WebAssembly.Instance(module,{}).exports;if(e.memory.buffer.byteLength!==65536)throw Error('Legacy form memory budget changed');
 try{e.memory.grow(1);throw Error('Legacy form memory grew');}catch(error){if(!(error instanceof RangeError))throw error;}
 const bytes='// Generated from the existing device form core. Instantiate only for v1.\nexport const encodedLegacyFormCore='+JSON.stringify(wasm.toString('base64'))+';\n';
 for(const directory of ['pc/src/component-center','pc/skills/petui/scripts']){
  const binary=join(root,directory,'form-v1-core.wasm'),encoded=join(root,directory,'form-v1-bytes.mjs');
  if(check){if(!wasm.equals(readFileSync(binary))||bytes!==readFileSync(encoded,'utf8'))throw Error('Legacy form artifact is stale');}
  else{copyFileSync(output,binary);writeFileSync(encoded,bytes);}
 }
 const source=join(root,'pc/src/component-center/form-v1-core.js'),mirror=join(root,'pc/skills/petui/scripts/form-v1-core.mjs');
 if(check){if(!readFileSync(source).equals(readFileSync(mirror)))throw Error('Legacy form adapter is stale');}else copyFileSync(source,mirror);
}finally{rmSync(temporary,{recursive:true});}
