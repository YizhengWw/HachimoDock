import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
test('all shared runtime mirrors regenerate consistently including JSON guards and viewport imports',()=>{
  const names=['app-runtime','input-policy','local-tools','app-navigation','canvas-core','app-form','form-v1-core','form-v2-core','form-v3-core','program-core','device-json','app-viewport','component-program','variable-owners','store-budget','app-transitions','d-surface','app-writers','texts-schema'];
  const dependencies=[...names,'service-bridge'];
  for(const name of names){
    let source=readFileSync(new URL(`./${name}.js`,import.meta.url),'utf8');
    for(const dependency of dependencies)source=source.replaceAll(`./${dependency}.js`,`./${dependency}.mjs`);
    assert.equal(readFileSync(new URL(`../../skills/petui/scripts/${name}.mjs`,import.meta.url),'utf8'),source,name);
  }
});
