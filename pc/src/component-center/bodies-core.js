/** Bodies preview delegates every rule and physics step to the device C/WASM. */
import {createSceneCore} from './scene-core.js';
import {validateBodiesScene} from './bodies-schema.js';
export {BODIES_ENGINE,validateBodiesScene} from './bodies-schema.js';
export async function createBodiesPreview(bytes,scene,vars,cues){
  validateBodiesScene(scene);
  return createSceneCore(bytes,structuredClone(scene),vars,cues);
}
