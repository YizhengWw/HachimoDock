// The exact firmware C core, not a decorative animation or a second ruleset.
export async function createClassicCore(bytes,definition){
  const game=definition.game||definition;
  const {instance}=await WebAssembly.instantiate(bytes,{});
  const e=instance.exports;
  if(!e.classic_init({blocks:2,snake:3,flappy:4}[game.type],game.tick_ms,17))throw Error('Unsupported game');
  if(game.vertical_speed_percent!==undefined&&(!Number.isInteger(game.vertical_speed_percent)||!e.classic_vertical_speed(game.vertical_speed_percent)))throw Error('Invalid vertical speed');
  const commands={start:0,left:game.direction_mode==="absolute"?7:1,right:game.direction_mode==="absolute"?8:2,rotate:3,drop:4,flap:5,soft_drop:6,up:9,down:10};
  let state=definition.initial_state,record=0;
  const sync=()=>{
    const a=new Int32Array(e.memory.buffer,e.classic_snapshot(),6);
    record=Math.max(record,a[2]);
    if(a[4])state=game.result_state;
    return a;
  };
  return {
    action(action,now){
      let resuming=false;
      if(definition.transitions){
        sync();const r=definition.transitions.find(r=>r.on===action&&(r.from==='*'||r.from===state));
        if(!r)return;
        resuming=r.to===game.playing_state&&state!==game.playing_state&&definition.transitions.some(t=>t.on===definition.lifecycle?.pause_action&&t.from===state&&t.to===game.playing_state);
        if(r.to&&r.to!==state){state=r.to;e.classic_hold_clock(now>>>0);}
      }
      const op=Object.keys(game.actions).find(k=>game.actions[k]===action);if(!resuming&&op in commands)e.classic_action(commands[op],now>>>0);
      if(!definition.transitions){const a=sync();state=a[4]?game.result_state:a[3]?game.playing_state:definition.initial_state;}
      sync();
    },
    tick(now){if(definition.transitions&&state!==game.playing_state)e.classic_hold_clock(now>>>0);else e.classic_tick(now>>>0);sync();},
    snapshot(){sync();const a=new Int32Array(e.memory.buffer,e.classic_snapshot(),262);return {width:a[0],height:a[1],score:a[2],running:!!a[3],over:!!a[4],kind:a[5],state,record,cells:Array.from(a.slice(6,6+a[0]*a[1]))};},
  };
}
