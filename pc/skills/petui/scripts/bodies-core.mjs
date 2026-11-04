/** Bodies preview delegates every rule and physics step to the device C/WASM. */
import {createSceneCore} from './scene-core.mjs';
import {validateBodiesScene} from './bodies-schema.mjs';
export {BODIES_ENGINE,validateBodiesScene} from './bodies-schema.mjs';
export async function createBodiesPreview(bytes,scene,vars,cues){
  validateBodiesScene(scene);
  return createSceneCore(bytes,structuredClone(scene),vars,cues);
}
