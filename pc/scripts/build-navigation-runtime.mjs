// Reproduce a fixed product core. Never compile or execute component code.
import {spawnSync} from 'node:child_process';
import {readFileSync,writeFileSync,copyFileSync,mkdtempSync,rmSync} from 'node:fs';
import {resolve,dirname,join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {tmpdir} from 'node:os';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'../..');
if(process.argv.slice(2).some(a=>a!=='--check'))throw Error('Only --check is supported');
const check=process.argv.includes('--check'),temporary=mkdtempSync(join(tmpdir(),'pet-navigation-'));
try{
 const output=join(temporary,'navigation-core.wasm');
 const result=spawnSync(process.env.CLANG||'clang',['--target=wasm32','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-O2','-ffreestanding','-fno-builtin','-nostdlib','-Wl,--no-entry','-Wl,--export-all','-Wl,--initial-memory=65536','-Wl,--max-memory=65536','-Wl,-z,stack-size=8192','-Wl,--strip-all','-I'+join(root,'firmware/main'),join(root,'pc/scripts/game-wasm/navigation_bridge.c'),'-o',output],{stdio:'inherit'});
 if(result.status!==0)throw Error('Native navigation compiler failed');
 const wasm=readFileSync(output),module=new WebAssembly.Module(wasm);
 if(WebAssembly.Module.imports(module).length)throw Error('Navigation must not import providers');
 const e=new WebAssembly.Instance(module,{}).exports;
 if(e.memory.buffer.byteLength!==65536)throw Error('Navigation memory budget changed');
 try{e.memory.grow(1);throw Error('Navigation memory grew');}catch(error){if(!(error instanceof RangeError))throw error;}
 const bytes='// Generated from the fixed device navigation core.\nexport const encodedNavigationCore='+JSON.stringify(wasm.toString('base64'))+';\n';
 for(const directory of ['pc/src/component-center','pc/skills/petui/scripts']){
  const binary=join(root,directory,'navigation-core.wasm'),encoded=join(root,directory,'navigation-bytes.mjs');
  if(check){if(!wasm.equals(readFileSync(binary))||bytes!==readFileSync(encoded,'utf8'))throw Error('Navigation artifact is stale');}
  else{copyFileSync(output,binary);writeFileSync(encoded,bytes);}
 }
}finally{rmSync(temporary,{recursive:true});}
