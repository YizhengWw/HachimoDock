// Replay an authorized saved response through the production parser; no network.
import fs from 'node:fs';
import { join } from 'node:path';
import { callThinkingModel } from '../src/lib/avatar-pipeline/thinking-model.js';
import { normalizePromptResponse } from '../src/lib/avatar-pipeline/prompts.js';
import { CUSTOM_GENERATION_FAMILIES } from '../src/lib/avatar-pipeline/families.js';
const directory = process.argv[2];
const response = JSON.parse(fs.readFileSync(join(directory,'thinking-response.private.json'),'utf8'));
globalThis.fetch = async () => Response.json(response);
const parsed = await callThinkingModel({thinking:{apiKey:'offline-replay-only'},image:{dataUrl:'data:image/png;base64,AA=='},families:CUSTOM_GENERATION_FAMILIES});
const manifest = normalizePromptResponse({response:parsed,families:CUSTOM_GENERATION_FAMILIES});
const report = {status:response.status,model:response.model,usage:response.usage,parserPassed:true,familyCount:manifest.entries.length,families:manifest.entries.map(e=>({family:e.family,promptCharacters:e.prompt.length}))};
fs.writeFileSync(join(directory,'validation.json'),JSON.stringify(report,null,2),{mode:0o600});
console.log(JSON.stringify(report));
