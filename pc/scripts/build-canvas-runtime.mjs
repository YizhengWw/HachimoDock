// Compile the same pure raster core for preview; never bundle generated code in
// a component. Uses the already-installed compiler/linker, no downloads.
// The preview core carries p4-canvas-v2 (64 commands / 16 names / 8192 spans);
// a p4-canvas-v1 declaration still validates and rasterizes under v1 budgets.
import {spawnSync} from 'node:child_process';
import {readFileSync,copyFileSync,mkdtempSync,rmSync} from 'node:fs';
import {resolve,dirname,join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {tmpdir} from 'node:os';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'../..');
if(process.argv.slice(2).some(arg=>arg!=='--check'))throw Error('Only --check is supported');
const check=process.argv.includes('--check');
const temporary=check?mkdtempSync(join(tmpdir(),'pet-canvas-build-')):null;
const target=join(root,'pc/src/component-center/canvas-core.wasm');
const output=check?join(temporary,'canvas-core.wasm'):target;
const result=spawnSync(process.env.CLANG||'clang',['--target=wasm32','-O2','-ffreestanding','-fno-builtin','-nostdlib','-Wl,--no-entry','-Wl,--export-all','-Wl,--initial-memory=262144','-Wl,--max-memory=262144','-Wl,--strip-all','-DPET_P4_CANVAS_V2=1','-I'+join(root,'firmware/main'),join(root,'firmware/main/pet_p4_canvas.c'),join(root,'firmware/main/pet_p4_particles.c'),join(root,'pc/scripts/game-wasm/canvas_bridge.c'),'-o',output],{stdio:'inherit'});
try{
  if(result.status!==0)throw Error('Canvas compiler failed');
  const built=readFileSync(output),module=new WebAssembly.Module(built);
  if(WebAssembly.Module.imports(module).length)throw Error('Canvas core must have no ambient imports');
  const mirror=join(root,'pc/skills/petui/scripts/canvas-core.wasm'),source=join(root,'pc/src/component-center/canvas-core.js'),adapter=join(root,'pc/skills/petui/scripts/canvas-core.mjs');
  if(check){
    if(!built.equals(readFileSync(target))||!built.equals(readFileSync(mirror))||!readFileSync(source).equals(readFileSync(adapter)))throw Error('Canvas core or shared adapter is stale');
  }else{copyFileSync(output,mirror);copyFileSync(source,adapter);}
}finally{if(temporary)rmSync(temporary,{recursive:true});}
