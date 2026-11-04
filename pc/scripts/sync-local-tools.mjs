// Mechanical mirror of shared pure runtime modules used by package/server smoke tests.
import fs from 'node:fs';
const names=['app-runtime','input-policy','local-tools','app-navigation','canvas-core','app-form','form-v1-core','form-v2-core','form-v3-core','program-core','device-json','app-viewport','component-program','variable-owners','store-budget','app-transitions','d-surface','app-writers','texts-schema'];
const dependencies=[...names,'service-bridge'];
const selected=process.argv.slice(2);
if(selected.some(name=>!names.includes(name)))throw Error('Unknown shared runtime mirror');
for(const name of selected.length?selected:names){
 let source=fs.readFileSync(new URL(`../src/component-center/${name}.js`,import.meta.url),'utf8');
 for(const dependency of dependencies)source=source.replaceAll(`./${dependency}.js`,`./${dependency}.mjs`);
 fs.writeFileSync(new URL(`../skills/petui/scripts/${name}.mjs`,import.meta.url),source);
}
