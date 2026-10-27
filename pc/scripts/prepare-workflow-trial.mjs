// Offline fixtures from production builders. No credentials or API calls.
import fs from 'node:fs';
import { join } from 'node:path';
import { randomInt } from 'node:crypto';
import { CUSTOM_GENERATION_FAMILIES } from '../src/lib/avatar-pipeline/families.js';
import { buildThinkingModelRequest, callThinkingModel } from '../src/lib/avatar-pipeline/thinking-model.js';
import { normalizePromptResponse } from '../src/lib/avatar-pipeline/prompts.js';
import { buildVolcanoTaskPayload } from '../src/lib/avatar-pipeline/providers/volcano.js';
const [dir, mode='prepare']=process.argv.slice(2);
if (mode==='prepare') {
  const ids=CUSTOM_GENERATION_FAMILIES.map(f=>f.family);
  const selected=[];
  while(selected.length<2) selected.push(ids.splice(randomInt(ids.length),1)[0]);
  const {body}=buildThinkingModelRequest({thinking:{},image:{dataUrl:'REFERENCE'},families:CUSTOM_GENERATION_FAMILIES,appearanceName:'小歪'});
  fs.writeFileSync(join(dir,'workflow-plan.json'),JSON.stringify({selected,thinkingBody:body}),{mode:0o600,flag:'wx'});
  console.log(JSON.stringify({selected}));
} else {
  const plan=JSON.parse(fs.readFileSync(join(dir,'workflow-plan.json'),'utf8'));
  const response=JSON.parse(fs.readFileSync(join(dir,'thinking-response.private.json'),'utf8'));
  globalThis.fetch=async()=>Response.json(response);
  const parsed=await callThinkingModel({thinking:{apiKey:'offline-only'},image:{dataUrl:'REFERENCE'},families:CUSTOM_GENERATION_FAMILIES});
  const manifest=normalizePromptResponse({response:parsed,families:CUSTOM_GENERATION_FAMILIES});
  const entries=plan.selected.map(family=>({family,body:buildVolcanoTaskPayload({model:'doubao-seedance-2-0-mini-260615',prompt:manifest.entries.find(e=>e.family===family).prompt,imageDataUrl:'REFERENCE',duration:5,resolution:'480p',ratio:'4:3',generateAudio:false,watermark:false,cameraFixed:true})}));
  fs.writeFileSync(join(dir,'video-plan.json'),JSON.stringify(entries,null,2),{mode:0o600,flag:'wx'});
  fs.writeFileSync(join(dir,'manifest.private.json'),JSON.stringify(manifest,null,2),{mode:0o600,flag:'wx'});
  console.log(JSON.stringify({parserPassed:true,prompts:manifest.entries.length,selected:plan.selected,usage:response.usage}));
}
