import {validateBodiesScene} from './bodies-schema.mjs';
// Package rules interpreted by the same C collision/movement core as the board.
export const sceneShapes=['rect','player-ship','enemy-ship','bullet','star','paddle','ball','circle','capsule','triangle','diamond','heart','cloud','coin','character'];
export async function createSceneCore(bytes,scene,vars,cues){
  const {instance}=await WebAssembly.instantiate(bytes,{}),e=instance.exports;
  const string=s=>{const b=new TextEncoder().encode(s||'');if(b.length>47)throw Error('Scene identifier too long');const out=new Uint8Array(e.memory.buffer,e.scene_text(),64);out.fill(0);out.set(b);};
  if(!scene?.grid||scene.grid.rows||scene.grid.solid_tones||scene.sprites||scene.entities.length>12||scene.rules.length>20)throw Error('Unsupported scene surface');
  e.scene_begin(scene.grid.width,scene.grid.height,scene.tick_ms);
  const entity=id=>scene.entities.findIndex(n=>n.id===id);
  scene.entities.forEach((n,i)=>{string(n.id);e.scene_entity(i,n.x,n.y,n.width??1,n.height??1,n.tone??1,n.vx??0,n.vy??0,['clamp','wrap','bounce','hide','stop'].indexOf(n.bounds||'clamp'),sceneShapes.indexOf(n.shape||'rect'),n.active!==false,n.collidable!==false);});
  if(scene.bodies!==undefined||scene.gravity!==undefined){
    validateBodiesScene(scene);
    if(!e.scene_bodies||!e.scene_body||!e.scene_physics_snapshot)throw Error('Scene core lacks p4-bodies-v1');
    e.scene_bodies(scene.gravity??0);
    scene.bodies.forEach(b=>e.scene_body(entity(b.entity),b.mass??1,b.restitution??100,b.static===true,b.rolling===true));
  }
  if(scene.motion){const m=scene.motion;e.scene_motion(entity(m.body),m.platforms.reduce((mask,id)=>mask|(1<<entity(id)),0),m.gravity,m.lift,m.speed[0],m.speed[1],m.charge_ms);}
  if(scene.motion?.track){const t=scene.motion.track;if(t.engine!=='p4-platform-track-v1'||!e.scene_track)throw Error('Unsupported platform track');e.scene_track(t.camera_x,...t.gap,...t.width,t.tones.reduce((mask,tone)=>mask|(1<<tone),0));}
  const triggers=['action','tick','collision','edge','blocked','landed'];
  // p4-logic-v1: runtime int vars are mirrored into the core by declaration order.
  const varNames=Object.keys(vars||{}).slice(0,8),varIndex=k=>varNames.indexOf(k);
  const cmp=['lte','lt','gte','gt','eq','ne'];
  scene.rules.forEach((r,i)=>{
    const trigger=triggers.includes(r.on)?triggers.indexOf(r.on):0;
    string(trigger===0?r.on:'');e.scene_rule(i,trigger,entity(r.entity),entity(r.with),['any','left','right','top','bottom'].indexOf(r.edge||'any'));
    if(r.when){
      if(!e.scene_rule_when)throw Error('Scene core lacks p4-logic-v1 guards');
      const conditions=r.when.all;
      if(conditions!==undefined&&(!Array.isArray(conditions)||conditions.length!==1))throw Error('Scene guards require one condition');
      const w=conditions?conditions[0]:r.when,reference=w.value&&typeof w.value==='object';
      const slot=name=>{const index=varIndex(name);if(index<0||vars[name]?.type!=='int')throw Error('Scene guards require int variables in the first eight declaration slots');return index;};
      e.scene_rule_when(i,slot(w.var),cmp.indexOf(w.op||'lte'),reference?slot(w.value.var):-1,reference?0:(w.value??w.lte??0));
    }
    if(r.do.length>4)throw Error('Too many scene operations');
    r.do.forEach((o,j)=>{
      const types=['move','velocity','accelerate','place','show','hide','score-add','score-set','run','stop','restart','bounce','tone','charge','launch','cancel','var-set','var-inc','sfx','impulse'];
      const kind=o.op==='score'?(o.add!==undefined?'score-add':'score-set'):o.op==='var'?(o.set!==undefined?'var-set':'var-inc'):o.op;
      const x=Array.isArray(o.x)?o.x:[o.x??0,o.x??0],y=Array.isArray(o.y)?o.y:[o.y??0,o.y??0];
      const flags=(o.x!==undefined?1:0)|(o.y!==undefined?2:0)|(Array.isArray(o.x)?4:0)|(Array.isArray(o.y)?8:0);
      const cueIndex=o.op==='sfx'?(cues||[]).indexOf(o.cue):-1;
      if(o.op==='sfx'&&cueIndex<0)throw Error('Unknown sfx cue');
      e.scene_op(i,j,types.indexOf(kind),o.op==='var'?varIndex(o.name):entity(o.entity),entity(o.source),o.dx??0,o.dy??0,o.vx??0,o.vy??0,x[0],x[1],y[0],y[1],flags,o.op==='sfx'?cueIndex:(o.add??o.set??o.inc??0),o.tone??0,{x:1,y:2,both:3}[o.axis]??0);
    });
  });
  if(varNames.length&&e.scene_var)varNames.forEach((k,i)=>e.scene_var(i,vars[k]?.type==='int'?Number(vars[k]?.init)||0:0));
  if(!e.scene_init(scene.auto_start===true))throw Error('Invalid scene configuration');
  return {
    action(action,now){string(action);e.scene_action(now>>>0);},
    release:(cancel,now)=>e.scene_release(!!cancel,now>>>0),
    pointer:(id,x,y,axes)=>e.scene_pointer(entity(id),x,y,axes),
    tick:now=>e.classic_tick(now>>>0),
    // p4-logic-v1 var mirror: owner pushes runtime ints before action/tick and reads them back after.
    setVars(values){if(!e.scene_var)return;varNames.forEach((k,i)=>e.scene_var(i,vars[k]?.type==='int'?Math.trunc(Number(values?.[k])||0):0));},
    readVars(){if(!e.scene_var_get)return {};return Object.fromEntries(varNames.filter(k=>vars[k]?.type==='int').map(k=>[k,e.scene_var_get(varIndex(k))]));},
    // p4-sfx-v1: cue ids queued by rules since the last call (preview may play them; device does).
    takeSfx(){if(!e.scene_sfx_take)return [];const m=e.scene_sfx_take();return (cues||[]).filter((_,i)=>m&(1<<i));},
    hold:now=>e.classic_hold_clock(now>>>0),
    snapshot(){
      const physics=scene.bodies?new Int32Array(e.memory.buffer,e.scene_physics_snapshot(),49):null;
      const a=new Int32Array(e.memory.buffer,e.classic_snapshot(),262),b=new Int32Array(e.memory.buffer,e.scene_snapshot(),97);
      return {width:a[0],height:a[1],score:a[2],running:!!a[3],over:!!a[4],kind:a[5],charge:scene.motion?e.scene_charge():0,grounded:!!e.scene_grounded?.(),travel:scene.motion?.track?e.scene_travel():0,entities:Array.from({length:b[0]},(_,i)=>{const p=1+i*8;return {id:scene.entities[b[p+7]].id,x:b[p]/((scene.motion||scene.bodies)?256:1),y:b[p+1]/((scene.motion||scene.bodies)?256:1),width:b[p+2],height:b[p+3],tone:b[p+4],shape:sceneShapes[b[p+5]],active:!!b[p+6],...(physics?{vx:physics[1+i*4]/256,vy:physics[2+i*4]/256,rotation:physics[3+i*4]/256,body:!!physics[4+i*4]}:{})};})};
    }
  };
}
