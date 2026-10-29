import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';

// All inputs are isolated synthetic files; no installed app or user data is read.
function fixture(run) {
  const root=fs.mkdtempSync(path.join(os.tmpdir(),'builtin-release-test-'));
  const write=(file,value)=>{
    fs.mkdirSync(path.dirname(file),{recursive:true});
    fs.writeFileSync(file,typeof value==='string'?value:JSON.stringify(value));
  };
  const ids=['decision-capsule','answer-book','hope-train'];
  const source=path.join(root,'source'),resources=path.join(root,'resources');
  const script=path.join(source,'pc/scripts/verify-builtin-release.mjs');
  write(script,fs.readFileSync(new URL('../scripts/verify-builtin-release.mjs',import.meta.url),'utf8'));
  write(path.join(source,'pc/package.json'),{version:'0.0.0'});
  write(path.join(source,'firmware/factory-config.json'),{components:{ids}});
  for(const id of ids)for(const prefix of [path.join(source,'pc'),resources])
    write(path.join(prefix,'builtin-clawpkgs',id,'component.json'),{id,version:'1.0.0'});
  for(const prefix of [path.join(source,'pc'),resources])
    write(path.join(prefix,'skills/petui/SKILL.md'),'Synthetic fixture only.');
  for(const name of ['firmware.bin','firmware-v3.bin'])for(const prefix of [path.join(source,'pc/src-tauri'),resources])
    write(path.join(prefix,'firmware/esp32-p4',name),'0.7.103-p4-test');
  const verify=()=>spawnSync(process.execPath,[script,'--resources',resources,'--output',path.join(root,'report.json')],{encoding:'utf8'});
  try {run({root,resources,write,verify});} finally {fs.rmSync(root,{recursive:true,force:true});}
}

test('release verifier derives component count from the canonical catalog',()=>fixture(({verify})=>{
  const result=verify();assert.equal(result.status,0,result.stderr);
  assert.match(result.stdout,/Verified 3 complete components/);
}));
test('release verifier rejects an extra bundled component',()=>fixture(({resources,write,verify})=>{
  write(path.join(resources,'builtin-clawpkgs','unexpected','component.json'),{});
  const result=verify();assert.notEqual(result.status,0);assert.match(result.stderr,/bundled component list differs/);
}));
test('release verifier rejects loose files in the component root',()=>fixture(({resources,write,verify})=>{
  write(path.join(resources,'builtin-clawpkgs','unexpected.txt'),'fixture');
  const result=verify();assert.notEqual(result.status,0);assert.match(result.stderr,/unexpected file or symlink/);
}));
test('release verifier rejects a missing packaged component',()=>fixture(({resources,verify})=>{
  fs.rmSync(path.join(resources,'builtin-clawpkgs','answer-book'),{recursive:true});
  const result=verify();assert.notEqual(result.status,0);assert.match(result.stderr,/bundled component list differs/);
}));
