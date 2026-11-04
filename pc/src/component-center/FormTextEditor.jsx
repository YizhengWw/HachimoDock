/* Trusted PC text provider for the bounded local form, not a commit shortcut. */
import React,{useEffect,useRef,useState} from 'react';
import {formContext} from './app-form.js';
import {TEXT_FORM_ENGINE} from './form-v3-core.js';
import {MIXED_FORM_ENGINE} from './form-v2-core.js';
export function FormTextEditor({runtime,snapshot,enabled,onReplace}){
  const [composition,setComposition]=useState(null),[error,setError]=useState('');
  const composing=useRef(null),current=useRef('');
  const f=runtime.form,field=f?.fields[snapshot.vars[f?.focus_var]],context=formContext(runtime,snapshot);current.current=context;
  useEffect(()=>{composing.current=null;setComposition(null);setError('');},[runtime]);
  useEffect(()=>{
    if(composing.current!==null&&composing.current!==context){composing.current=null;setComposition(null);}
    if(!enabled||snapshot.page!==f?.page||snapshot.vars[f?.active_var]!==1){composing.current=null;setComposition(null);setError('');}
  },[context,enabled,snapshot.page,f]);
  if(![MIXED_FORM_ENGINE,TEXT_FORM_ENGINE].includes(f?.engine)||!enabled||snapshot.page!==f.page||snapshot.vars[f.active_var]!==1||field?.kind!=='text')return null;
  const submit=(text,identity)=>{
    if(current.current!==identity)return;
    if(new TextEncoder().encode(text).length>field.max_bytes){setError(`最多 ${field.max_bytes} 个 UTF-8 字节；未修改草稿`);return;}
    if([...text].some(c=>{const n=c.codePointAt(0);return n<32||(n>=127&&n<160)||(n>=0xd800&&n<=0xdfff);})){setError('不支持控制字符或不完整字符；未修改草稿');return;}
    setError('');onReplace(field.id,text,identity);
  };
  return <div aria-label="文本草稿输入"><label>文本草稿（确认后保存）<input type="text" aria-label="文本草稿" autoComplete="off" spellCheck={false}
    value={composition!==null?composition:snapshot.vars[field.draft_var]}
    onChange={event=>{if(composing.current!==null)setComposition(event.target.value);else submit(event.target.value,context);}}
    onCompositionStart={event=>{composing.current=context;setComposition(event.currentTarget.value);setError('');}}
    onCompositionEnd={event=>{const identity=composing.current;composing.current=null;setComposition(null);if(identity!==null)submit(event.currentTarget.value,identity);}}
    onBlur={()=>{composing.current=null;setComposition(null);}}
    onKeyDown={event=>{if(event.key==='Enter'&&!event.nativeEvent.isComposing)event.preventDefault();}}/></label>
    {error&&<div role="status">{error}</div>}
  </div>;
}
