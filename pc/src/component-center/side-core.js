/** Trusted C/WASM side-scene adapter. No network, filesystem or package code. */
export function validateSideRuntime(r){
  const int=(v,lo,hi)=>Number.isInteger(v)&&v>=lo&&v<=hi;
  const id=(v,n)=>typeof v==='string'&&v.length>=1&&v.length<=n&&!/[^A-Za-z0-9_.-]/.test(v);
  const fields=(v,allowed,required=allowed)=>v&&typeof v==='object'&&!Array.isArray(v)&&Object.keys(v).every(k=>allowed.includes(k))&&required.every(k=>Object.hasOwn(v,k));
  const optional=(v,k,fallback)=>Object.hasOwn(v,k)?v[k]:fallback;
  const need=v=>{if(!v)throw Error('Invalid bounded side-scene declaration');};
  const s=r?.scene,p=s?.physics,a=s?.art;
  need(fields(s,['engine','width','height','rows','tiles','actors','physics','actions','art','active_state','result_state','win_state','score_var','coins_var','lives_var','checkpoint_var']));
  need(s.engine==='p4-side-scene-v1'&&r.engine==='p4-bounded-runtime-v4'&&Array.isArray(r.tick)&&!r.tick.length);
  need(!['host','game','data','media','instrument','local','lifecycle','pages'].some(k=>Object.hasOwn(r,k)));
  need(r.surface?.engine==='p4-app-surface-v1'&&r.surface.graphics==='p4-app-graphics-v1'&&!['game_view','scene_view','data_view'].some(k=>Object.hasOwn(r.surface,k)));
  need(int(s.width,20,160)&&int(s.height,8,16)&&Array.isArray(s.tiles)&&int(s.tiles.length,1,16)&&Array.isArray(s.actors)&&int(s.actors.length,1,24));
  s.tiles.forEach((t,i)=>{need(fields(t,['flags','reward','replacement'],['flags'])&&int(t.flags,0,15)&&int(optional(t,'reward',0),0,10000)&&int(optional(t,'replacement',0),0,s.tiles.length-1)&&!((t.flags&2)&&(t.flags&5)));if(!i)need(t.flags===0&&!t.reward);});
  s.tiles.forEach(t=>{if(t.reward)need(!(s.tiles[t.replacement??0].reward??0));});
  need(Array.isArray(s.rows)&&s.rows.length===s.height&&s.rows.every(row=>typeof row==='string'&&row.length===s.width&&[...row].every(c=>'0123456789abcdef'.slice(0,s.tiles.length).includes(c))));
  const bounds={run_speed:[32,240],acceleration:[100,2000],friction:[100,2400],gravity:[200,1800],jump_speed:[100,600],terminal_speed:[240,640],lives:[1,9]};
  need(fields(p,Object.keys(bounds))&&Object.entries(bounds).every(([k,b])=>int(p[k],...b)));
  const names=new Set();let controlled=0;
  s.actors.forEach(v=>{
    need(fields(v,['id','behavior','contact','x','y','width','height','skin','speed','range_min','range_max','value','vertical','solid','stompable','edge_turn'],['id','behavior','contact','x','y','width','height','skin']));
    need(id(v.id,15)&&!names.has(v.id));names.add(v.id);
    need(['controlled','walker','static','platform'].includes(v.behavior)&&['none','damage','collect','goal','checkpoint'].includes(v.contact));
    need(int(v.x,0,s.width*16)&&int(v.y,0,s.height*16)&&int(v.width,1,32)&&int(v.height,1,32)&&v.x+v.width<=s.width*16&&v.y+v.height<=s.height*16&&int(v.skin,0,7));
    need(['vertical','solid','stompable','edge_turn'].every(k=>!Object.hasOwn(v,k)||typeof v[k]==='boolean'));
    need(int(optional(v,'speed',0),-160,160)&&int(optional(v,'range_min',0),0,2560)&&int(optional(v,'range_max',0),0,2560)&&int(optional(v,'value',0),0,10000));
    if(v.behavior==='controlled'){
      controlled++;need(v.contact==='none'&&!v.solid&&!v.speed);
      for(let y=Math.floor(v.y/16);y<=Math.floor((v.y+v.height-1)/16);y++)for(let x=Math.floor(v.x/16);x<=Math.floor((v.x+v.width-1)/16);x++)need(!(s.tiles[parseInt(s.rows[y][x],16)].flags&1));
    }
    if(v.behavior==='static')need(!v.speed&&!v.solid);
    if(v.behavior==='walker'||v.behavior==='platform'){
      const lo=v.range_min??0,hi=v.range_max??0,pos=v.vertical?v.y:v.x,size=v.vertical?v.height:v.width,extent=(v.vertical?s.height:s.width)*16;
      need(v.speed&&lo<=pos&&pos<=hi&&hi+size<=extent);
      if(v.behavior==='walker')need(!v.vertical&&!v.solid);else need(v.solid&&v.contact==='none');
    }
  });
  need(controlled===1&&Array.isArray(r.states));
  const states=['active_state','result_state','win_state'].map(k=>s[k]);
  need(new Set(states).size===3&&states.every(v=>id(v,23)&&r.states.includes(v)));
  const vars=['score_var','coins_var','lives_var','checkpoint_var'].map(k=>s[k]);
  need(new Set(vars).size===4&&vars.every(v=>id(v,31)&&r.vars?.[v]?.type==='int'));
  vars.forEach((v,i)=>{if(r.limits&&Object.hasOwn(r.limits,v))need(Array.isArray(r.limits[v])&&r.limits[v].length===2&&int(r.limits[v][0],-1e9,0)&&int(r.limits[v][1],[1e9,65535,p.lives,s.actors.length][i],1e9));});
  need(fields(s.actions,['left','right','jump','pause'])&&new Set(Object.values(s.actions)).size===4&&Object.values(s.actions).every(v=>id(v,47)));
  need(r.input?.engine==='p4-input-v1'&&r.input.hold===s.actions.jump&&Array.isArray(r.transitions));
  need(r.transitions.every(t=>fields(t,['from','on','to'],['from','on'])&&Object.values(s.actions).includes(t.on))&&new Set(r.transitions.map(t=>t.on)).size===4);
  const pauses=r.transitions.filter(t=>t.on===s.actions.pause&&t.from===s.active_state&&r.states.includes(t.to)&&!states.includes(t.to));
  need(pauses.length===1&&r.transitions.some(t=>t.on===s.actions.pause&&t.from===pauses[0].to&&t.to===s.active_state)&&r.states.includes(r.initial_state)&&![...states,pauses[0].to].includes(r.initial_state));
  need(fields(a,['image','columns','rows','scale','x','y','w','h','sky','tiles','skins'])&&id(a.image,15)&&int(a.columns,1,32)&&int(a.rows,1,15)&&a.columns*a.rows<=256);
  need(int(a.scale,1,3)&&int(a.x,0,639)&&int(a.y,0,479)&&int(a.w,160,640-a.x)&&int(a.h,128,480-a.y)&&int(a.sky,0,65535));
  need(a.h>=s.height*16*a.scale);
  const image=r.surface.images?.find(im=>im.id===a.image);
  need(image&&!Object.hasOwn(image,'encoding')&&image.frame_width===a.columns*16&&image.frame_height===a.rows*16);
  need(Array.isArray(a.tiles)&&a.tiles.length===s.tiles.length&&a.tiles.every(v=>int(v,0,a.columns*a.rows-1))&&Array.isArray(a.skins)&&int(a.skins.length,1,8)&&s.actors.every(v=>v.skin<a.skins.length));
  a.skins.forEach(v=>{need(fields(v,['idle','walk','air','hurt','fps','width','height'],['idle','walk','air','hurt','fps'])&&int(v.fps,1,20)&&int(optional(v,'width',16),8,32)&&int(optional(v,'height',16),8,32));need(['idle','walk','air','hurt'].every(k=>Array.isArray(v[k])&&int(v[k].length,1,8)&&v[k].every(frame=>int(frame,0,a.columns*a.rows-1))));});
  return true;
}
export async function createSideCore(bytes,runtime){
  validateSideRuntime(runtime);
  const s=runtime?.scene,p=s?.physics,a=s?.art;
  if(s?.engine!=='p4-side-scene-v1'||!p||!a||s.actors?.length>24||s.tiles?.length>16||s.width>160||s.height>16)throw Error('Unsupported side-scene declaration');
  const {instance}=await WebAssembly.instantiate(bytes,{}),e=instance.exports;
  const string=value=>{const v=new TextEncoder().encode(value||'');if(v.length>15)throw Error('Side-scene identifier too long');const out=new Uint8Array(e.memory.buffer,e.side_text(),64);out.fill(0);out.set(v);};
  e.side_begin(s.width,s.height,s.tiles.length,s.actors.length,p.lives,p.run_speed,p.acceleration,p.friction,p.gravity,p.jump_speed,p.terminal_speed);
  const cells=s.rows.flatMap(row=>[...row].map(value=>parseInt(value,16)));
  if(cells.length!==s.width*s.height||cells.some(v=>!Number.isInteger(v)||v<0||v>=s.tiles.length))throw Error('Invalid side-scene tile map');
  new Uint8Array(e.memory.buffer,e.side_map(),cells.length).set(cells);
  s.tiles.forEach((t,i)=>e.side_tile(i,t.flags,t.reward||0,t.replacement||0));
  s.actors.forEach((actor,i)=>{string(actor.id);e.side_actor(i,actor.x,actor.y,actor.width,actor.height,actor.skin,['controlled','walker','static','platform'].indexOf(actor.behavior),['none','damage','collect','goal','checkpoint'].indexOf(actor.contact),actor.speed||0,actor.range_min||0,actor.range_max||0,actor.value||0,(actor.vertical?1:0)|(actor.solid?2:0)|(actor.stompable?4:0)|(actor.edge_turn!==false?8:0));});
  e.side_art(a.columns,a.rows,a.scale,a.x,a.y,a.w,a.h,a.sky,a.skins.length);
  a.tiles.forEach((frame,i)=>e.side_art_tile(i,frame));
  a.skins.forEach((skin,i)=>{e.side_skin(i,skin.fps,skin.width||16,skin.height||16);['idle','walk','air','hurt'].forEach((mode,j)=>{if(!Array.isArray(skin[mode])||skin[mode].length<1||skin[mode].length>8||skin[mode].some(v=>!Number.isInteger(v)||v<0||v>=a.columns*a.rows))throw Error('Invalid skin frame');new Uint8Array(e.memory.buffer,e.side_skin_frames(i,j,skin[mode].length),skin[mode].length).set(skin[mode]);});});
  if(!e.side_commit())throw Error('Invalid bounded side-scene configuration');
  let direction=0,jumpRelease=null;
  const pauseState=runtime.transitions.find(t=>t.on===s.actions.pause&&t.from===s.active_state)?.to||runtime.initial_state;
  function snapshot(draw=true){
    const raw=new Int32Array(e.memory.buffer,e.side_snapshot(),11+24*7),count=raw[9];
    const buffer=draw?new Int32Array(e.memory.buffer,e.side_commands(),1+704*6):null,commands=draw?Array.from({length:buffer[0]},(_,i)=>Array.from(buffer.slice(1+i*6,7+i*6))):[];
    const phase=raw[0],state=[runtime.initial_state,s.active_state,pauseState,s.win_state,s.result_state][phase];
    return {phase,state,score:raw[1],coins:raw[2],lives:raw[3],checkpoint:raw[4],clock:raw[5],revision:raw[6],hurt:raw[7],invulnerable:raw[8],player:raw[10],
      vars:{[s.score_var]:raw[1],[s.coins_var]:raw[2],[s.lives_var]:raw[3],[s.checkpoint_var]:raw[4]},
      actors:Array.from({length:count},(_,i)=>{const at=11+i*7;return {id:s.actors[i].id,x:raw[at]/256,y:raw[at+1]/256,vx:raw[at+2]/256,vy:raw[at+3]/256,facing:raw[at+4],active:!!raw[at+5],grounded:!!raw[at+6]};}),commands};
  }
  return {snapshot,axis(value){direction=Math.sign(value);e.side_axis(direction);},
    jump(down,now){jumpRelease=null;e.side_jump(down,now>>>0);},
    action(action,now){if(action===s.actions.jump){e.side_jump(1,now>>>0);jumpRelease=now+120;}else if(action===s.actions.pause){e.side_pause(snapshot(false).phase!==2,now>>>0);direction=0;}else if(action===s.actions.left||action===s.actions.right){direction=action===s.actions.left?-1:1;e.side_axis(direction);return now+160;}return null;},
    pause(value,now){direction=0;e.side_pause(value,now>>>0);},
    tick(now){if(jumpRelease!=null&&now>=jumpRelease){e.side_jump(0,now>>>0);jumpRelease=null;}e.side_tick(now>>>0);},
    checkpoint(value){return !!e.side_checkpoint(value);},
    cells(){return Array.from(new Uint8Array(e.memory.buffer,e.side_cells(),s.width*s.height));}};
}
