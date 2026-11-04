/** Runtime snapshot and action handling shared by the particle overlay and tests. */
const compare={lte:(a,b)=>a<=b,lt:(a,b)=>a<b,gte:(a,b)=>a>=b,gt:(a,b)=>a>b,eq:(a,b)=>a===b,ne:(a,b)=>a!==b};
export function particleGuard(emitter,vars){
  const w=emitter.when;
  return !w||!!(vars&&Object.hasOwn(vars,w.var)&&Number.isInteger(vars[w.var])&&
    Number.isInteger(w.value)&&compare[w.op]?.(vars[w.var],w.value));
}
export function stepParticlePreview(core,emitters,{vars,page,clock=0,actions=[]}){
  if(!Number.isSafeInteger(clock)||clock<0)throw Error('Invalid particle clock');
  const pending=actions.slice(0,4); // same bounded action queue as the renderer
  const dots=[];
  emitters.forEach((e,i)=>{
    if(e.page!==undefined&&e.page!==4&&e.page!==page)return;
    const emitting=particleGuard(e,vars);
    if(emitting&&e.trigger?.action)for(const action of pending)if(action===e.trigger.action)core.burst(i);
    core.step(i,clock,emitting);
    for(const dot of core.dots(i))dots.push([...dot,i]);
  });
  return dots;
}
