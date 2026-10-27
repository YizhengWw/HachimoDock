import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import fs from 'node:fs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const output=path.join(root,'pc/src/component-center/fruit-core.wasm');
const args=['--target=wasm32','-O2','-ffreestanding','-fno-builtin','-nostdlib','-Wl,--no-entry','-Wl,--export-all',
  '-Wl,--initial-memory=131072','-Wl,--max-memory=131072','-Wl,--strip-all',
  '-I'+path.join(root,'pc/scripts/fruit-wasm'),'-I'+path.join(root,'firmware/main'),
  path.join(root,'firmware/main/pet_p4_fruit_core.c'),path.join(root,'pc/scripts/fruit-wasm/bridge.c'),'-o',output];
const result=spawnSync(process.env.CLANG||'clang',args,{stdio:'inherit'});
if(result.status!==0)process.exit(result.status||1);
fs.copyFileSync(output,path.join(root,'pc/skills/petui/scripts/fruit-core.wasm'));
fs.copyFileSync(path.join(root,'pc/src/component-center/fruit-core.js'),path.join(root,'pc/skills/petui/scripts/fruit-core.mjs'));
console.log('Built shared bounded fruit core (no imports, 128 KiB memory).');
