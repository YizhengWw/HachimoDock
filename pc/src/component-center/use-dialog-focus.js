import {useEffect,useRef} from 'react';
export function useDialogFocus(onClose,busy=false) {
  const ref=useRef(null),latest=useRef({onClose,busy});latest.current={onClose,busy};
  const returnFocus=useRef(typeof document==='undefined'?null:document.activeElement);
  useEffect(()=>{
    if(!busy&&ref.current?.getClientRects().length&&!ref.current.contains(document.activeElement))
      (ref.current.querySelector('button:not([disabled])')||ref.current).focus();
  },[busy]);
  useEffect(()=>{
    const previous=returnFocus.current;
    const controls=()=>[...(ref.current?.querySelectorAll('button:not([disabled]),input:not([disabled]),select:not([disabled]),textarea:not([disabled]),a[href],[tabindex="0"]')||[])].filter(e=>e.getClientRects().length);
    (controls()[0] || ref.current)?.focus();
    const key=e=>{
      if(!ref.current?.getClientRects().length)return;
      const top=[...document.querySelectorAll('[aria-modal="true"]')].filter(node=>node.getClientRects().length).at(-1);
      if(top&&top!==ref.current)return;
      if(e.key==='Escape'){e.preventDefault();e.stopPropagation();if(!latest.current.busy)latest.current.onClose?.();}
      if(e.key==='Tab'){
        const c=controls(),first=c[0],last=c.at(-1);if(!first){e.preventDefault();return;}
        if(e.shiftKey&&(document.activeElement===first||!ref.current.contains(document.activeElement))){e.preventDefault();last.focus();}
        else if(!e.shiftKey&&(document.activeElement===last||!ref.current.contains(document.activeElement))){e.preventDefault();first.focus();}
      }
    };
    document.addEventListener('keydown',key,true);
    return()=>{document.removeEventListener('keydown',key,true);setTimeout(()=>{
      const modal=[...document.querySelectorAll('[aria-modal="true"]')].at(-1);
      if(previous?.isConnected&&(!modal||modal.contains(previous)))previous.focus();
      else if(modal&&!modal.contains(document.activeElement))(modal.querySelector('button:not([disabled])')||modal).focus();
    },0);};
  },[]);
  return ref;
}
