import test from "node:test";
import assert from "node:assert/strict";
import { componentPetVoice, createComponentVoiceSync } from "./component-pet-voice.js";
test("component speech shares pet voice and clone, not persona or legacy gain", () => {
  const a={id:"pet-a",personaVoice:{persona:{intro:"private"},voice:{speaker:"test",clone_speaker_id:"S_clone",speed:1.2,volume:0.3}}};
  assert.deepEqual(componentPetVoice(a,{connected:true,boardDeviceId:"board"}),{
    boardDeviceId:"board",appearanceId:"pet-a",speaker:"test",cloneSpeakerId:"S_clone",speed:1.2,
  });
  assert.equal(componentPetVoice(a,{connected:false,boardDeviceId:"board"}),null);
  assert.equal(componentPetVoice(null,{connected:true,boardDeviceId:"board"}),null);
});
test("late calls cannot replace the latest pet or disconnected target", async () => {
  const calls=[];let release;
  const sync=createComponentVoiceSync(async(cmd,payload)=>{
    assert.equal(cmd,"component_speech_set_pet_voice");calls.push(payload.input);
    if(calls.length===1)await new Promise(r=>{release=r;});
  });
  const work=sync({appearanceId:"a"});
  void sync({appearanceId:"b"});void sync(null);release();await work;
  assert.deepEqual(calls,[{appearanceId:"a"},null]);
  await sync({appearanceId:"c"});assert.equal(calls.at(-1).appearanceId,"c");
});
test("a failed update can be retried without an unhandled rejection", async()=>{
  let count=0;const sync=createComponentVoiceSync(async()=>{count++;if(count===1)throw Error("offline");});
  await sync(null);await sync(null);assert.equal(count,2);
});
