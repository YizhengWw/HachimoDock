import test from 'node:test';
import assert from 'node:assert/strict';
import {composerFailureEvidence,withoutComposerEvidence} from './composer-failure-evidence.js';
const snapshot={schema:1,stage:'firstAmbiguousSelection',candidateCount:2,structure:{controls:[{index:0},{index:1}]}};
const encoded=`失败现场\n[COMPOSER_EVIDENCE]${JSON.stringify(snapshot)}[/COMPOSER_EVIDENCE]`;
test('native evidence remains the original failed snapshot, without another UI scan',()=>{
  assert.deepEqual(composerFailureEvidence(undefined,encoded),snapshot);
  assert.equal(withoutComposerEvidence(encoded),'失败现场');
  assert.deepEqual(composerFailureEvidence(encoded),snapshot);
});
test('malformed, truncated, wrong schema and oversized errors are not interpreted',()=>{
  for(const input of ['',null,'[COMPOSER_EVIDENCE]{','[COMPOSER_EVIDENCE]null[/COMPOSER_EVIDENCE]',encoded.replace('"schema":1','"schema":2'),'x'.repeat(131073)+encoded]) {
    assert.equal(composerFailureEvidence(input),null);
  }
});
test('normal errors retain their wording and evidence never triggers an action',()=>{
  assert.equal(withoutComposerEvidence('  输入框未稳定  '),'输入框未稳定');
  assert.equal(withoutComposerEvidence(undefined),'');
  assert.equal(composerFailureEvidence('正常'),null);
});
