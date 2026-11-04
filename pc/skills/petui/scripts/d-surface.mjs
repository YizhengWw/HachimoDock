// D surface projection: pure display state, no providers or mutation of declarations.
import {validateDerivedWriters} from './app-writers.mjs';
const utf8=new TextEncoder();
export function flowLines(text,width,font=16,cap=256){
 const chars=[...text],out=[],bytes=[0];for(const c of chars)bytes.push(bytes.at(-1)+utf8.encode(c).length);let start=0,i=0,used=0,br=0;
 const wide=c=>c.codePointAt(0)>=0x2e80,advance=c=>wide(c)?font:Math.ceil(font/2);
 const emit=end=>{out.push(chars.slice(start,end).join('').replace(/ +$/,''));};
 while(i<chars.length&&out.length<cap){const c=chars[i];if(c==='\n'){emit(i);start=++i;used=0;br=0;continue;}
  if(wide(c)&&i>start)br=i;
  if((used+advance(c)>width||bytes[i+1]-bytes[start]>120)&&i>start){const cut=br>start?br:i;emit(cut);start=cut;while(chars[start]===' ')start++;used=chars.slice(start,i).reduce((s,c)=>s+advance(c),0);br=0;if(start>i)i=start;continue;}
  used+=advance(c);i++;if(c===' '||wide(c))br=i;
 }
 if(out.length<cap&&(i>start||!out.length))emit(i);return out;
}
export function dataProjection(runtime,snapshot){
 const vars=snapshot.vars||{},filters=runtime.data?.filters||[];
 const rows=(snapshot.data?.rows||[]).slice(0,20).filter(row=>filters.every(f=>{const n=Number(vars[f.var])||0;return n<=0||n<=f.values.length&&String(row[f.field]??'')===String(f.values[n-1]);}));
 const pages=Math.max(1,Math.ceil(rows.length/5)),value=Number(vars[runtime.data?.page_var])||0;
 return {rows,pages,page:((value%pages)+pages)%pages};
}
export function visibleRows(runtime,snapshot){return dataProjection(runtime,snapshot).rows;}
export function dSnapshot(runtime,snapshot){
 validateDerivedWriters(runtime);
 const next={...snapshot,vars:{...snapshot.vars}};
 for(const n of runtime.surface?.elements||[])if(n.type==='longtext'&&n.count_var){const text=runtime.surface.texts?.find(t=>t.id===n.text_id)?.text||'';const count=flowLines(text,n.w,n.font_size||16).length,per=Math.max(1,Math.floor(n.h/(n.line_height||Math.ceil((n.font_size||16)*1.25))));next.vars[n.count_var]=Math.max(1,Math.ceil(count/per));}
 return next;
}
export function dSurface(runtime,snapshot,surface=runtime.surface){
 const vars=snapshot.vars||{},shell=surface.shell==='p4-app-shell-v1',page=Math.max(0,(runtime.pages||[]).findIndex(p=>p.id===snapshot.page));
 const rows=visibleRows(runtime,snapshot),sel=rows.length?rows[Math.min(rows.length-1,Math.max(0,Number(vars[runtime.data?.select_var])||0))]:null;
 const elements=[];
 for(const raw of surface.elements||[]){let n={...raw};if(shell&&n.page!==undefined){n.hidden=n.hidden||n.page!==page;delete n.page;}
  if(n.row_bind){n.text=String(sel?.[n.row_bind]??'');delete n.row_bind;}
  if(n.type==='longtext'){
   const font=n.font_size||16,lh=n.line_height||Math.ceil(font*1.25),per=Math.max(1,Math.floor(n.h/lh)),text=surface.texts?.find(t=>t.id===n.text_id)?.text||'',lines=flowLines(text,n.w,font),pages=Math.max(1,Math.ceil(lines.length/per)),at=((Number(vars[n.page_var])||0)%pages+pages)%pages;
   for(let i=0;i<per;i++)elements.push({...n,type:'text',_flowLine:true,text:lines[at*per+i]||'',y:n.y+i*lh,h:lh});
  }else if(n.type==='figure'){const lh=n.line_height||Math.ceil((n.font_size||16)*1.25);elements.push({...n,type:'image',h:n.h-lh},{...n,type:'text',image:undefined,y:n.y+n.h-lh,h:lh,text:n.caption});}
  else elements.push(n);
 }
 const tabs=surface.tabs;if(tabs){const active=((Number(vars[tabs.var])||0)%tabs.items.length+tabs.items.length)%tabs.items.length;
  tabs.items.forEach((t,i)=>{const x=tabs.x+Math.floor(tabs.w*i/tabs.items.length),w=Math.floor(tabs.w*(i+1)/tabs.items.length)-Math.floor(tabs.w*i/tabs.items.length);elements.push({type:'button',x,y:tabs.y,w,h:tabs.h,color:tabs.colors[i===active?1:0],foreground:tabs.colors[i===active?3:2],text:t.label,icon:t.icon,action:'tab.next',_tabIndex:i});});}
 return {...surface,elements};
}
export function dAction(runtime,state,action,node){
 const t=runtime.surface?.tabs;if(!t||!['tab.next','tab.prev'].includes(action))return null;
 const next=structuredClone(state),n=t.items.length,at=((Number(next.vars[t.var])||0)%n+n)%n,index=Number.isInteger(node?._tabIndex)?node._tabIndex:(at+(action==='tab.next'?1:n-1))%n;
 next.vars[t.var]=index;next.page=runtime.pages[t.items[index].page].id;next.history=[];next.pointerEpoch=(next.pointerEpoch||0)+1;return next;
}
