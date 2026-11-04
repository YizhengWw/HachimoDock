/** Device string representation only; not JSON grammar/schema/cycle validation. */
function unicodeScalars(text){
  for(let i=0;i<text.length;i++){
    const c=text.charCodeAt(i);
    if(c>=0xd800&&c<=0xdbff){
      if(++i===text.length)return false;const low=text.charCodeAt(i);
      if(low<0xdc00||low>0xdfff)return false;
    }else if(c>=0xdc00&&c<=0xdfff)return false;
  }
  return true;
}
function stringsValid(value,unicodeText){
  const pending=[value],seen=new WeakSet();
  while(pending.length){
    const item=pending.pop();
    if(typeof item==='string'){if(item.includes('\0')||(unicodeText&&!unicodeScalars(item)))return false;}
    else if(item&&typeof item==='object'){
      if(seen.has(item))continue;seen.add(item);
      for(const [key,child]of Object.entries(item)){
        if(key.includes('\0')||(unicodeText&&!unicodeScalars(key)))return false;pending.push(child);
      }
    }
  }
  return true;
}
export function deviceJsonStringsNoNul(value){return stringsValid(value,false);}
export function deviceJsonStringsValidText(value){return stringsValid(value,true);}
export const DEVICE_JSON_MAX_DEPTH=32;
export function deviceJsonWithinDepth(value,maximum=DEVICE_JSON_MAX_DEPTH){
  if(!Number.isInteger(maximum)||maximum<1||maximum>DEVICE_JSON_MAX_DEPTH)return false;
  const pending=[[value,1]],seen=new WeakMap();
  while(pending.length){
    const [item,depth]=pending.pop();if(!item||typeof item!=='object')continue;
    if(depth>maximum)return false;
    if((seen.get(item)||0)>=depth)continue;seen.set(item,depth);
    for(const child of Object.values(item))pending.push([child,depth+1]);
  }
  return true;
}
