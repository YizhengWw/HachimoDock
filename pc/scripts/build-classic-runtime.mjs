import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import fs from 'node:fs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const args=['--target=wasm32','-O2','-ffreestanding','-fno-builtin','-nostdlib','-Wl,--no-entry','-Wl,--export-all','-Wl,--initial-memory=262144','-Wl,--max-memory=262144','-Wl,--strip-all',
  '-I'+path.join(root,'pc/scripts/game-wasm'),'-I'+path.join(root,'firmware/main'),
  path.join(root,'firmware/main/pet_p4_game.c'),path.join(root,'pc/scripts/game-wasm/bridge.c'),
  '-o',path.join(root,'pc/src/component-center/classic-core.wasm')];
const result=spawnSync(process.env.CLANG||'clang',args,{stdio:'inherit'});
if(result.status!==0)process.exit(result.status??1);
for(const [source,target] of [['classic-core.wasm','classic-core.wasm'],['classic-core.js','classic-core.mjs']])fs.copyFileSync(path.join(root,'pc/src/component-center',source),path.join(root,'pc/skills/petui/scripts',target));
for(const name of ['scene-core','app-runtime','input-policy','local-tools']){
  const source=fs.readFileSync(path.join(root,'pc/src/component-center',`${name}.js`),'utf8').replaceAll('./input-policy.js','./input-policy.mjs').replaceAll('./local-tools.js','./local-tools.mjs');
  fs.writeFileSync(path.join(root,'pc/skills/petui/scripts',`${name}.mjs`),source);
}
