// Mechanical mirror of shared pure runtime modules used by package/server smoke tests.
import fs from 'node:fs';
for(const name of ['app-runtime','input-policy','local-tools']){
 const source=fs.readFileSync(new URL(`../src/component-center/${name}.js`,import.meta.url),'utf8').replaceAll('./input-policy.js','./input-policy.mjs').replaceAll('./local-tools.js','./local-tools.mjs');
 fs.writeFileSync(new URL(`../skills/petui/scripts/${name}.mjs`,import.meta.url),source);
}
