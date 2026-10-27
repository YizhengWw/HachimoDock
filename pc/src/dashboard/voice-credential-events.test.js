import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {subscribeAsrCredentialUpdates} from './voice-credential-events.js';
import {API_CONFIGURATION_UPDATED_EVENT} from '../lib/api-configuration.js';
const emit=(target,detail)=>target.dispatchEvent(new CustomEvent(API_CONFIGURATION_UPDATED_EVENT,{detail}));

test('saving ASR on another tab restores listening using the newly persisted configuration',async()=>{
  const target=new EventTarget();
  let saved=false,restores=0,message='please configure ASR';
  let pending;
  const dispose=subscribeAsrCredentialUpdates(target,()=>{pending=Promise.resolve().then(()=>{
    assert(saved,'restore must read the latest committed configuration');
    restores++;message='listening ready';
  });});
  // The visible voice panel is absent throughout the save. Only its persistent
  // owner is subscribed, exactly as when API settings replaces the dashboard.
  saved=true;
  emit(target,{providerId:'volcengine-asr',configured:true});
  await pending;
  assert.equal(restores,1);assert.equal(message,'listening ready');
  dispose();emit(target,{providerId:'volcengine-asr',configured:true});
  assert.equal(restores,1);
});
test('other providers, incomplete saves and missing detail never arm the microphone',()=>{
  const target=new EventTarget();let starts=0;
  const dispose=subscribeAsrCredentialUpdates(target,()=>starts++);
  for(const detail of [undefined,{}, {providerId:'voice-chat-llm',configured:true},{providerId:'volcengine-asr',configured:false},{providerId:'volcengine-asr'}])emit(target,detail);
  assert.equal(starts,0);dispose();
});
test('persistent controller owns restoration, while visible panel only refreshes status',()=>{
  const dashboard=readFileSync(new URL('../DeviceDashboard.jsx',import.meta.url),'utf8');
  const panel=readFileSync(new URL('./VoiceAssistantPanel.jsx',import.meta.url),'utf8');
  assert(dashboard.indexOf('subscribeAsrCredentialUpdates(window,') < dashboard.indexOf('{active && <PageShell'));
  assert.match(dashboard,/subscribeAsrCredentialUpdates\(window, resumeDeviceVoiceAfterCredentialAccess\)/);
  assert.doesNotMatch(panel,/onCredentialReady|resume: true/);
});
