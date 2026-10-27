// A small shared clock avoids focus/visibility/interval refresh storms.
export function refreshGate(intervalMs=60_000,now=Date.now){
  let last=now();
  return (force=false)=>{
    const at=now();
    if(!force&&at>=last&&at-last<intervalMs)return false;
    last=at;return true;
  };
}
