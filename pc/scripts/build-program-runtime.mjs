// Reproduce the shipped product interpreter, never compile a user's program.
// Compiler is local. No downloads, credentials, device or installer commands.
import {spawnSync} from 'node:child_process';
import {readFileSync,writeFileSync,copyFileSync,mkdtempSync,rmSync} from 'node:fs';
import {resolve,dirname,join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {tmpdir} from 'node:os';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'../..');
if(process.argv.slice(2).some(a=>a!=='--check'))throw Error('Only --check is supported');
const check=process.argv.includes('--check'),temporary=mkdtempSync(join(tmpdir(),'pet-program-core-'));
try{
 const sources=['pet_p4_program_core.c','pet_p4_program_image.c','pet_p4_program_runtime.c','pet_p4_program_timers.c'].map(n=>join(root,'firmware/main',n));
 // Two fixed interpreters from the same C: the v1 build and the p4-component-program-v2 build
 // (PET_P4_PROGRAM_V2 with the flags its #error gates require). Neither compiles user code.
 const variants=[
  {suffix:'',symbol:'encodedProgramCore',defines:[]},
  {suffix:'-v2',symbol:'encodedProgramCoreV2',defines:['-DPET_P4_PROGRAM_V2=1','-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_P4_TEXT_TABLE=1','-DPET_P4_APP_DIRTY_CACHE=1']},
 ];
 for(const variant of variants){
 const output=join(temporary,'program-core'+variant.suffix+'.wasm');
 const result=spawnSync(process.env.CLANG||'clang',['--target=wasm32','-O2','-ffreestanding','-fno-builtin','-nostdlib','-Wl,--no-entry','-Wl,--export-all','-Wl,--initial-memory=262144','-Wl,--max-memory=262144','-Wl,--strip-all',...variant.defines,'-I'+join(root,'pc/scripts/game-wasm'),'-I'+join(root,'firmware/main'),...sources,join(root,'pc/scripts/game-wasm/program_bridge.c'),'-o',output],{stdio:'inherit'});
 if(result.status!==0)throw Error('Shared program compiler failed');
 const wasm=readFileSync(output),module=new WebAssembly.Module(wasm);
 if(WebAssembly.Module.imports(module).length)throw Error('Program preview must have no imports');
 const e=new WebAssembly.Instance(module,{}).exports;
 if(e.memory.buffer.byteLength!==262144)throw Error('Program preview memory budget changed');
 try{e.memory.grow(1);throw Error('Program preview memory grew');}catch(error){if(!(error instanceof RangeError))throw error;}
 if(e.pp_version()!==(variant.suffix?2:1))throw Error('Program preview variant mismatch');
 const bytes='// Generated from the fixed product interpreter, not user code.\nexport const '+variant.symbol+'='+JSON.stringify(wasm.toString('base64'))+';\n';
 for(const directory of ['pc/src/component-center','pc/skills/petui/scripts']){
  const binary=join(root,directory,'program-core'+variant.suffix+'.wasm'),encoded=join(root,directory,'program-bytes'+variant.suffix+'.mjs');
  if(check){if(!wasm.equals(readFileSync(binary))||bytes!==readFileSync(encoded,'utf8'))throw Error('Shared program artifact is stale');}
  else{copyFileSync(output,binary);writeFileSync(encoded,bytes);}
 }
 }
 const source=join(root,'pc/src/component-center/program-core.js'),mirror=join(root,'pc/skills/petui/scripts/program-core.mjs');
 if(check){if(!readFileSync(source).equals(readFileSync(mirror)))throw Error('Shared program adapter is stale');}
 else copyFileSync(source,mirror);
}finally{rmSync(temporary,{recursive:true});}
