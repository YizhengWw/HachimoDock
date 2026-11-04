import {textForm,TEXT_FORM_ENGINE} from './form-v3-core.js';
// Opt-in local form state. No storage, host calls, or physical-key assumptions.
import {mixedForm,MIXED_FORM_ENGINE} from './form-v2-core.js';
import {legacyForm,LEGACY_FORM_ENGINE} from './form-v1-core.js';
export const FORM_ENGINE=LEGACY_FORM_ENGINE;
export const formOperation=(runtime,action)=>Object.entries(runtime.form?.actions||{}).find(([,a])=>a===action)?.[0];
// Structural proof only, not an exploration waiver: a one-field editor cannot
// move focus; a text-only, one-scalar keyboard cannot move its selection.
// Call after declaration validation. Unknown engines/actions are never waived.
export function formStructuralDisabled(runtime){
  const f=runtime.form,result={};
  if(!f||![LEGACY_FORM_ENGINE,MIXED_FORM_ENGINE].includes(f.engine)||!Array.isArray(f.fields)||!f.fields.length)return result;
  const mark=(op,why)=>{if(typeof f.actions?.[op]==='string')result[f.actions[op]]=why;};
  if(f.fields.length===1){mark('previous','single_field');mark('next','single_field');}
  if(f.engine===MIXED_FORM_ENGINE&&f.fields.every(q=>q.kind==='text')&&f.keyboard?.characters?.length===1){
    mark('decrease','single_character_keyboard');mark('increase','single_character_keyboard');
  }
  return result;
}
export function formCancel(runtime,state){if(runtime.form?.engine===TEXT_FORM_ENGINE){textForm(runtime,state,'cancel');return state;}
  if(runtime.form?.engine===MIXED_FORM_ENGINE){mixedForm(runtime,state,'cancel');return state;}
  const changed=legacyForm(runtime,state,'cancel');
  if(changed)state.formEpoch=(state.formEpoch||0)+1;return state;
}
export function formInitialize(runtime,state){if(runtime.form?.engine===TEXT_FORM_ENGINE){textForm(runtime,state,'initialize');return state;}if(runtime.form?.engine===MIXED_FORM_ENGINE){mixedForm(runtime,state,'initialize');return state;}if(legacyForm(runtime,state,'initialize'))state.formEpoch=(state.formEpoch||0)+1;return state;}
export function formAllowed(runtime,state,op){if(runtime.form?.engine===TEXT_FORM_ENGINE){return textForm(runtime,state,'allowed',op);}
  if(runtime.form?.engine===MIXED_FORM_ENGINE)return mixedForm(runtime,state,'allowed',op);
  return legacyForm(runtime,state,'allowed',op);
}
export function formApply(runtime,state,op){if(runtime.form?.engine===TEXT_FORM_ENGINE){return textForm(runtime,state,'apply',op);}
  if(runtime.form?.engine===MIXED_FORM_ENGINE)return mixedForm(runtime,state,'apply',op);
  if(!legacyForm(runtime,state,'apply',op))return false;
  state.formEpoch=(state.formEpoch||0)+1;
  return true;
}
export function formText(runtime,state,id){if(runtime.form?.engine===TEXT_FORM_ENGINE){return textForm(runtime,state,'text',id);}
  if(runtime.form?.engine===MIXED_FORM_ENGINE)return mixedForm(runtime,state,'text',id);
  return runtime.form?legacyForm(runtime,state,'text',id):'';
}
// Pointer-only identity; does not cancel directional hold/repeat scheduling.
export function formContext(runtime,state){const f=runtime.form;return f?JSON.stringify([...(f.engine===TEXT_FORM_ENGINE?Object.values(f.keyboard).map(n=>state.vars[n]):[]),state.formEpoch||0,state.vars[f.focus_var],state.vars[f.active_var],...f.fields.flatMap(q=>[state.vars[q.var],state.vars[q.draft_var]]),...(f.engine===MIXED_FORM_ENGINE?[state.vars[f.keyboard.index_var],state.vars[f.keyboard.label_var]]:[])]):'';}
