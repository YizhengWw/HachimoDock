/** Data-only canvas adapter. Raster operations execute the shared C/WASM core. */
export const CANVAS_ENGINE='p4-canvas-v1',CANVAS_ENGINE_V2='p4-canvas-v2';
// Budgets per declaration profile (canvas.md cost table): v1 stays exactly as shipped.
export const canvasBudget=engine=>engine===CANVAS_ENGINE_V2?{commands:64,names:16,spans:8192,points:64}:{commands:16,names:8,spans:2048,points:0};
export const canvasCapabilityAccepts=(engine,capability)=>engine===CANVAS_ENGINE_V2?capability===CANVAS_ENGINE_V2:engine===CANVAS_ENGINE&&(capability===CANVAS_ENGINE||capability===CANVAS_ENGINE_V2);
export function validateCanvas(c,runtime){
  const need=v=>{if(!v)throw Error('Invalid bounded canvas declaration');};
  const fields=(v,keys)=>v&&typeof v==='object'&&!Array.isArray(v)&&Object.keys(v).length===keys.length&&Object.keys(v).every(k=>keys.includes(k));
  const integer=(v,lo,hi)=>Number.isInteger(v)&&v>=lo&&v<=hi;
  need((fields(c,['engine','x','y','w','h','commands'])||fields(c,['engine','x','y','w','h','commands','page']))&&(c.engine===CANVAS_ENGINE||c.engine===CANVAS_ENGINE_V2));
  const v2=c.engine===CANVAS_ENGINE_V2,budget=canvasBudget(c.engine);
  const page=Object.hasOwn(c,'page')?c.page:4;need(integer(page,0,4)&&(!runtime||page===4||page<Math.max(1,runtime.pages?.length||0)));
  need(integer(c.x,0,639)&&integer(c.y,0,479)&&integer(c.w,1,320)&&integer(c.h,1,240)&&c.x+c.w<=640&&c.y+c.h<=480);
  need(Array.isArray(c.commands)&&integer(c.commands.length,1,budget.commands));
  const names=[],points=[];
  function value(v,lo,hi){
    if(integer(v,lo,hi))return v;
    need(fields(v,['var'])&&typeof v.var==='string'&&/^[A-Za-z0-9_.-]{1,31}$/.test(v.var));
    need(!runtime||runtime.vars?.[v.var]?.type==='int');
    if(!names.includes(v.var))names.push(v.var);need(names.length<=budget.names);return -1-names.indexOf(v.var);
  }
  const radius=Math.floor(Math.min(c.w,c.h)/2),side=Math.max(c.w,c.h);
  let cost=0;
  const commands=c.commands.map(p=>{
    need(p&&typeof p==='object');const ops=v2?['line','rect','circle','polyline','arc','ring']:['line','rect','circle'];const op=ops.indexOf(p.op);
    const keys=[['x2','y2'],['w','h'],['r'],['points'],['x','y','r','start','end'],['x','y','r','inner']][op];
    need(op>=0&&fields(p,['op','color',...(op===3||op>=4?[]:['x','y']),...keys])&&integer(p.color,0,65535));
    if(op===3){
      need(Array.isArray(p.points)&&integer(p.points.length,3,16)&&points.length+p.points.length<=budget.points);
      const start=points.length;
      for(const pt of p.points){need(Array.isArray(pt)&&pt.length===2);points.push([value(pt[0],0,c.w-1),value(pt[1],0,c.h-1)]);}
      cost+=(p.points.length-1)*side;need(cost<=budget.spans);return [op,0,0,start,p.points.length,p.color,0,0];
    }
    const x=value(p.x,0,c.w-1),y=value(p.y,0,c.h-1);let a,b=0,extra=[0,0];
    if(op===0){a=value(p.x2,0,c.w-1);b=value(p.y2,0,c.h-1);cost+=side;}
    else if(op===1){a=value(p.w,1,c.w);b=value(p.h,1,c.h);cost++;}
    else if(op===2){a=value(p.r,1,radius);cost+=2*(a<0?radius:a)+1;}
    else if(op===4){a=value(p.r,1,radius);extra=[value(p.start,0,359),value(p.end,0,359)];cost+=6*(a<0?radius:a)+8;}
    else{a=value(p.r,1,radius);b=value(p.inner,0,radius-1);need(a<0||b<0||b<a);cost+=4*(a<0?radius:a)+2;}
    need(cost<=budget.spans);return [op,x,y,a,b,p.color,...extra];
  });
  return {commands,names,points,v2};
}
export async function createCanvasCore(bytes,declaration,runtime){
  const {commands,names,points,v2}=validateCanvas(declaration,runtime);
  const module=bytes instanceof WebAssembly.Module?bytes:new WebAssembly.Module(bytes);
  if(WebAssembly.Module.imports(module).length)throw Error('Canvas imports are not permitted');
  const {exports:e}=new WebAssembly.Instance(module,{});
  if(v2&&!(e.canvas_v2&&e.canvas_point&&e.canvas_command_extra))throw Error('Canvas core lacks p4-canvas-v2');
  const capacity=e.canvas_spans_max?e.canvas_spans_max():2048,budget=canvasBudget(declaration.engine).spans;
  e.canvas_begin(declaration.x,declaration.y,declaration.w,declaration.h,commands.length,names.length);
  if(v2){e.canvas_v2(1);points.forEach(([x,y],i)=>e.canvas_point(i,x,y));}
  commands.forEach((c,i)=>{e.canvas_command(i,...c.slice(0,6));if(v2)e.canvas_command_extra(i,c[6],c[7]);});
  if(!e.canvas_valid())throw Error('Canvas core rejected the declaration');
  return {bindings:[...names],project(vars={}){
    const values=names.map(name=>{
      if(!Object.hasOwn(vars,name))throw Object.assign(Error('Canvas snapshot unavailable'),{code:'canvas_snapshot_unavailable'});
      const v=vars[name];if(!Number.isInteger(v)||v<-2147483648||v>2147483647)throw Object.assign(Error('Invalid canvas integer snapshot'),{code:'canvas_snapshot_invalid'});
      return v;
    });
    values.forEach((value,i)=>e.canvas_value(i,value));
    const ptr=e.canvas_render(),view=new Int32Array(e.memory.buffer,ptr,1+capacity*5),count=view[0];
    if(!Number.isInteger(count)||count<0||count>budget)throw Error('Invalid canvas span budget');
    return Array.from({length:count},(_,i)=>Array.from(view.slice(1+i*5,6+i*5)));
  }};
}
/** p4-particles-v1 preview core: the same integer emitter simulation as the device, two slots. */
export const PARTICLES_ENGINE='p4-particles-v1';
export async function createParticleCore(bytes){
  const module=bytes instanceof WebAssembly.Module?bytes:new WebAssembly.Module(bytes);
  if(WebAssembly.Module.imports(module).length)throw Error('Particle imports are not permitted');
  const {exports:e}=new WebAssembly.Instance(module,{});
  if(!e.particles_begin)throw Error('Canvas core lacks p4-particles-v1');
  return {
    begin(slot,c,seed=0){
      e.particles_begin(slot,c.x,c.y,c.w,c.h,c.color,c.life_ms,c.trigger?.tick??c.tick??0,c.spread,c.count,c.budget,c.speed,c.shape==='spark'?1:0,c.gravity,seed>>>0);
      if(!e.particles_valid(slot))throw Error('Particle core rejected the emitter');
    },
    burst:slot=>e.particles_burst(slot),
    step:(slot,now,emitting=true)=>e.particles_step(slot,now>>>0,emitting?1:0),
    dots(slot){const ptr=e.particles_dots(slot),view=new Int32Array(e.memory.buffer,ptr,1+64*4),count=view[0];
      if(!Number.isInteger(count)||count<0||count>64)throw Error('Invalid particle budget');
      return Array.from({length:count},(_,i)=>Array.from(view.slice(1+i*4,5+i*4)));}
  };
}
