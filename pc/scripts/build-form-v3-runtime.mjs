// Reproducible product C preview; no downloads or package-executable code.
import {spawnSync} from 'node:child_process';
import {readFileSync,writeFileSync,copyFileSync,mkdtempSync,rmSync} from 'node:fs';
import {resolve,dirname,join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {tmpdir} from 'node:os';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'../..');
if(process.argv.slice(2).some(a=>a!=='--check'))throw Error('Only --check is supported');
const check=process.argv.includes('--check'),temporary=mkdtempSync(join(tmpdir(),'pet-form-core-'));
try{
 const output=join(temporary,'form-v3-core.wasm');
 const result=spawnSync(process.env.CLANG||'clang',['--target=wasm32','-include',join(root,'pc/scripts/game-wasm/form_v3_libc.h'),'-O2','-ffreestanding','-fno-builtin','-nostdlib','-Wl,--no-entry','-Wl,--export-all','-Wl,--initial-memory=262144','-Wl,--max-memory=262144','-Wl,--strip-all','-I'+join(root,'pc/scripts/game-wasm'),'-I'+join(root,'firmware/main'),join(root,'firmware/main/pet_p4_form_v2.c'),join(root,'firmware/main/pet_p4_form_v3.c'),join(root,'pc/scripts/game-wasm/form_v3_bridge.c'),'-o',output],{stdio:'inherit'});
 if(result.status!==0)throw Error('Shared form compiler failed');
 const wasm=readFileSync(output),module=new WebAssembly.Module(wasm);
 if(WebAssembly.Module.imports(module).length)throw Error('Form preview must have no imports');
 const bytes='// Generated from the bounded native form core. Instantiate only for v3.\nexport const encodedFormCore='+JSON.stringify(wasm.toString('base64'))+';\n';
 for(const directory of ['pc/src/component-center','pc/skills/petui/scripts']){
  const binary=join(root,directory,'form-v3-core.wasm'),encoded=join(root,directory,'form-v3-bytes.mjs');
  if(check){if(!wasm.equals(readFileSync(binary))||bytes!==readFileSync(encoded,'utf8'))throw Error('Shared form artifact is stale');}
  else{copyFileSync(output,binary);writeFileSync(encoded,bytes);}
 }
 const source=join(root,'pc/src/component-center/form-v3-core.js'),mirror=join(root,'pc/skills/petui/scripts/form-v3-core.mjs');
 const adapter=readFileSync(source,'utf8').replaceAll('./variable-owners.js','./variable-owners.mjs');
 if(check){if(adapter!==readFileSync(mirror,'utf8'))throw Error('Shared form adapter is stale');}
 else writeFileSync(mirror,adapter);
}finally{rmSync(temporary,{recursive:true});}
