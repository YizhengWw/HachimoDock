// Package rules interpreted by the same C collision/movement core as the board.
export const sceneShapes=['rect','player-ship','enemy-ship','bullet','star','paddle','ball','circle','capsule','triangle','diamond','heart','cloud','coin','character'];
export async function createSceneCore(bytes,scene){
  const {instance}=await WebAssembly.instantiate(bytes,{}),e=instance.exports;
  const string=s=>{const b=new TextEncoder().encode(s||'');if(b.length>47)throw Error('Scene identifier too long');const out=new Uint8Array(e.memory.buffer,e.scene_text(),64);out.fill(0);out.set(b);};
  if(!scene?.grid||scene.grid.rows||scene.grid.solid_tones||scene.sprites||scene.entities.length>12||scene.rules.length>20)throw Error('Unsupported scene surface');
  e.scene_begin(scene.grid.width,scene.grid.height,scene.tick_ms);
  const entity=id=>scene.entities.findIndex(n=>n.id===id);
  scene.entities.forEach((n,i)=>{string(n.id);e.scene_entity(i,n.x,n.y,n.width??1,n.height??1,n.tone??1,n.vx??0,n.vy??0,['clamp','wrap','bounce','hide','stop'].indexOf(n.bounds||'clamp'),sceneShapes.indexOf(n.shape||'rect'),n.active!==false,n.collidable!==false);});
  if(scene.motion){const m=scene.motion;e.scene_motion(entity(m.body),m.platforms.reduce((mask,id)=>mask|(1<<entity(id)),0),m.gravity,m.lift,m.speed[0],m.speed[1],m.charge_ms);}
  if(scene.motion?.track){const t=scene.motion.track;if(t.engine!=='p4-platform-track-v1'||!e.scene_track)throw Error('Unsupported platform track');e.scene_track(t.camera_x,...t.gap,...t.width,t.tones.reduce((mask,tone)=>mask|(1<<tone),0));}
  const triggers=['action','tick','collision','edge','blocked','landed'];
  scene.rules.forEach((r,i)=>{
    const trigger=triggers.includes(r.on)?triggers.indexOf(r.on):0;
    string(trigger===0?r.on:'');e.scene_rule(i,trigger,entity(r.entity),entity(r.with),['any','left','right','top','bottom'].indexOf(r.edge||'any'));
    if(r.do.length>4)throw Error('Too many scene operations');
    r.do.forEach((o,j)=>{
      const types=['move','velocity','accelerate','place','show','hide','score-add','score-set','run','stop','restart','bounce','tone','charge','launch','cancel'];
      const kind=o.op==='score'?(o.add!==undefined?'score-add':'score-set'):o.op;
      const x=Array.isArray(o.x)?o.x:[o.x??0,o.x??0],y=Array.isArray(o.y)?o.y:[o.y??0,o.y??0];
      const flags=(o.x!==undefined?1:0)|(o.y!==undefined?2:0)|(Array.isArray(o.x)?4:0)|(Array.isArray(o.y)?8:0);
      e.scene_op(i,j,types.indexOf(kind),entity(o.entity),entity(o.source),o.dx??0,o.dy??0,o.vx??0,o.vy??0,x[0],x[1],y[0],y[1],flags,o.add??o.set??0,o.tone??0,{x:1,y:2,both:3}[o.axis]??0);
    });
  });
  if(!e.scene_init(scene.auto_start===true))throw Error('Invalid scene configuration');
  return {
    action(action,now){string(action);e.scene_action(now>>>0);},
    release:(cancel,now)=>e.scene_release(!!cancel,now>>>0),
    pointer:(id,x,y,axes)=>e.scene_pointer(entity(id),x,y,axes),
    tick:now=>e.classic_tick(now>>>0),
    hold:now=>e.classic_hold_clock(now>>>0),
    snapshot(){
      const a=new Int32Array(e.memory.buffer,e.classic_snapshot(),262),b=new Int32Array(e.memory.buffer,e.scene_snapshot(),97);
      return {width:a[0],height:a[1],score:a[2],running:!!a[3],over:!!a[4],kind:a[5],charge:scene.motion?e.scene_charge():0,grounded:!!e.scene_grounded?.(),travel:scene.motion?.track?e.scene_travel():0,entities:Array.from({length:b[0]},(_,i)=>{const p=1+i*8;return {id:scene.entities[b[p+7]].id,x:b[p]/(scene.motion?256:1),y:b[p+1]/(scene.motion?256:1),width:b[p+2],height:b[p+3],tone:b[p+4],shape:sceneShapes[b[p+5]],active:!!b[p+6]};})};
    }
  };
}
