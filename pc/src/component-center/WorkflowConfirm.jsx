import React from 'react';
import {useDialogFocus} from './use-dialog-focus.js';
export default function WorkflowConfirm({title,description,confirmLabel='确认',cancelLabel='继续编辑',onCancel,onConfirm}){
  const ref=useDialogFocus(onCancel);
  return <div className="modal-backdrop component-workflow-confirm"><section ref={ref} tabIndex={-1} className="modal-card" role="alertdialog" aria-modal="true" aria-label={title}>
    <div className="modal-header"><h3>{title}</h3></div>
    <div className="modal-body"><p>{description}</p></div>
    <div className="modal-footer"><button className="btn-secondary" onClick={onCancel}>{cancelLabel}</button><button className="btn-primary" onClick={onConfirm}>{confirmLabel}</button></div>
  </section></div>;
}
