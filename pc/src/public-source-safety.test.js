/** Public source export boundaries; no credentials or paid service calls. */
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync,existsSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
const read=p=>readFileSync(new URL(p,import.meta.url),'utf8');
test('public checkout resources do not depend on LFS quota',()=>{
  assert.doesNotMatch(read('../../.gitattributes'),/filter=lfs/);
  const image=readFileSync(new URL('../src-tauri/firmware/esp32-p4/firmware.bin',import.meta.url));
  assert.notEqual(image.subarray(0,7).toString(),'version');
});
test('public sources have no credential or enterprise-certificate build features',()=>{
  const cargo=read('../src-tauri/Cargo.toml'),build=read('../src-tauri/build.rs');
  assert.doesNotMatch(cargo,/^(?:internal-network|bundled-service-keys)\s*=/m);
  assert.doesNotMatch(build,/prepare_internal_ca|prepare_bundled_service_keys|internal-credentials/);
  for(const file of ['build-internal-macos','prepare-internal-ca','prepare-internal-credentials','prepare-bundled-service-keys'])
    assert.equal(existsSync(new URL(`../scripts/${file}.mjs`,import.meta.url)),false);
});
test('public build rejects credential injection inputs',()=>{
  for(const name of ['PET_MANAGER_INTERNAL_CA_FILE','PET_MANAGER_INTERNAL_CREDENTIALS_FILE','PET_MANAGER_BUNDLED_SERVICE_KEYS_FILE']){
    const child=spawnSync(process.execPath,['scripts/check-public-build-env.mjs'],{
      cwd:new URL('..',import.meta.url),env:{...process.env,[name]:'test-only'},encoding:'utf8',
    });
    assert.notEqual(child.status,0);
    assert.match(child.stderr,new RegExp(name));
  }
});
