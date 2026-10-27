// Fixture only: no credentials, network, thinking or video requests.
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { buildVolcanoImageEditPayload } from '../src/lib/avatar-pipeline/providers/volcano-image.js';
const [imagePath, outputDir] = process.argv.slice(2);
if (!imagePath || !outputDir) throw Error('image path and output directory required');
const imageBody = buildVolcanoImageEditPayload({ model:'doubao-seedream-5-0-flash-260915', imageDataUrl:'REFERENCE' });
// Flash is a single-image request; legacy sequential-generation option is omitted.
delete imageBody.sequential_image_generation;
writeFileSync(join(outputDir,'plan.json'), JSON.stringify({imagePath,imageBody}), {mode:0o600,flag:'wx'});
