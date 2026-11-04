/** Preview-only ownership and bounded effects; never grants native/host authorization. */
import {canvasCapabilityAccepts} from './canvas-core.js';
const FORM_CAPABILITIES=Object.freeze({
  'p4-app-form-v1':'widgetAppForm',
  'p4-app-form-v2':'widgetAppFormV2',
  'p4-app-form-v3':'widgetAppFormV3',
});
export function previewFormCapabilityAccepts(engine,capabilities={}){
  return typeof engine==='string'&&Object.hasOwn(FORM_CAPABILITIES,engine)&&capabilities?.[FORM_CAPABILITIES[engine]]===engine;
}
export const previewCanvasCapabilityAccepts=(engine,capability)=>canvasCapabilityAccepts(engine,capability);
/** Read during render: effect cleanup alone cannot hide a previous generation. */
export const previewOwnedValue=(record,owner,fallback=null)=>record?.owner===owner?record.value:fallback;
export const clearPreviewOwner=(record,owner)=>record?.owner===owner?null:record;
/** One instance per preview owner. FIFO saturation drops NEW actions, never replays pre-ready input. */
export function createParticleActionQueue(){
  let declarations=null,active=false,page,ready=false,pending=[],generation=0;
  return {
    configure(emitters,visible,currentPage){
      const enabled=visible===true&&Array.isArray(emitters)&&emitters.length>0;
      if(declarations!==emitters||active!==enabled||page!==currentPage)pending=[];
      if(declarations!==emitters||active!==enabled){ready=false;generation++;}
      declarations=emitters;active=enabled;page=currentPage;
    },
    generation:()=>generation,
    isCurrent:(emitters,expected)=>declarations===emitters&&generation===expected,
    isReady:(emitters,expected)=>declarations===emitters&&generation===expected&&active&&ready,
    setReady(emitters,value,expected){
      if(declarations!==emitters||generation!==expected)return;
      ready=active&&value===true;
      if(!ready)pending=[];
    },
    enqueue(action){
      if(!active||!ready||pending.length>=4||typeof action!=='string'
        ||!declarations.some(e=>e.trigger?.action===action))return false;
      pending.push(action);return true;
    },
    take(emitters,expected){
      if(declarations!==emitters||generation!==expected||!active||!ready)return [];
      const result=pending;pending=[];return result;
    },
    clear(){pending=[];},
  };
}
