const need=q=>{if(!q)throw new TypeError('Invalid bounded data rows declaration');};
export function validateDataRows(r){
 const d=r.data,s=r.surface||{},nodes=s.elements||[];
 if(d!==undefined){
  need(d&&typeof d==='object'&&!Array.isArray(d)&&Object.hasOwn(d,'source')&&Object.hasOwn(d,'page_var')&&Object.keys(d).every(k=>['source','page_var','select_var','filters','refresh_action'].includes(k)));
  const int=k=>need(typeof k==='string'&&/^[A-Za-z0-9_.-]{1,31}$/.test(k)&&k!==d.page_var&&r.vars?.[k]?.type==='int');
  if(Object.hasOwn(d,'select_var'))int(d.select_var);
  if(Object.hasOwn(d,'refresh_action'))need(d.refresh_action==='data.refresh'&&r.transitions?.some(t=>t.on===d.refresh_action));
  if(Object.hasOwn(d,'filters')){need(Array.isArray(d.filters)&&d.filters.length>=1&&d.filters.length<=4);const seen=new Set();
   for(const f of d.filters){need(f&&typeof f==='object'&&Object.keys(f).length===3&&['var','field','values'].every(k=>Object.hasOwn(f,k))&&!seen.has(f.var)&&['label','value','detail','meta','tone'].includes(f.field));int(f.var);seen.add(f.var);need(Array.isArray(f.values)&&f.values.length>=1&&f.values.length<=8);
    for(const v of f.values)need(f.field==='tone'&&Number.isInteger(v)&&v>=-1&&v<=1||typeof v==='string'&&new TextEncoder().encode(v).length>=1&&new TextEncoder().encode(v).length<=24&&!/[\u0000-\u001f\u007f]/u.test(v));
   }
  }
 }
 const declared=d&&['select_var','filters','refresh_action'].some(k=>Object.hasOwn(d,k))||s.data_view?.layout==='rows'||nodes.some(n=>Object.hasOwn(n,'row_bind'));
 if(declared&&s.data_view){const v=s.data_view;need(Object.keys(v).length===7&&['engine','x','y','w','h','layout','colors'].every(k=>Object.hasOwn(v,k))&&v.engine==='p4-data-surface-v1'&&['todos','metrics','rows'].includes(v.layout));}
 if(declared){need(d&&s.data_view&&typeof s.data_view==='object'&&s.bindings==='p4-variable-bindings-v1');for(const n of nodes)if(Object.hasOwn(n,'row_bind'))need(['label','value','detail','meta'].includes(n.row_bind)&&['text','button'].includes(n.type)&&!['text','bind','var_bind','field_bind','key_for','text_at'].some(k=>Object.hasOwn(n,k)));}
}
