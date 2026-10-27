// Explicitly authorized one-shot probe fixtures; no credentials or API calls here.
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { buildVolcanoImageEditPayload } from '../src/lib/avatar-pipeline/providers/volcano-image.js';
import { buildThinkingModelRequest } from '../src/lib/avatar-pipeline/thinking-model.js';
import { CUSTOM_GENERATION_FAMILIES } from '../src/lib/avatar-pipeline/families.js';
const [imagePath, outputDir] = process.argv.slice(2);
if (!imagePath || !outputDir) throw Error('image path and output directory required');
const { body } = buildThinkingModelRequest({ thinking:{}, image:{dataUrl:'REFERENCE'}, families:CUSTOM_GENERATION_FAMILIES, appearanceName:'歪' });
const plan = { imagePath, useOriginalForThinking:true, imageBody:buildVolcanoImageEditPayload({imageDataUrl:'REFERENCE'}), thinkingBody:body };
writeFileSync(join(outputDir,'plan.json'), JSON.stringify(plan), {mode:0o600});
