// The device and preview execute the same bounded C game (trusted app asset).
export async function createFruitCore(bytes,config) {
  const {instance}=await WebAssembly.instantiate(bytes,{});
  const e=instance.exports;
  e.fruit_init(config.duration_sec,config.lives,config.sound_volume);
  return {
    start:(now,seed=17)=>e.fruit_action(0,now>>>0,seed>>>0),
    pause:now=>e.fruit_action(1,now>>>0,0),
    mode:value=>e.fruit_mode(value),
    suspend:now=>e.fruit_suspend(now>>>0),
    tick:now=>e.fruit_tick(now>>>0),
    pointer:(kind,x,y,now)=>e.fruit_pointer(kind,x,y,now>>>0),
    sound:()=>e.fruit_sound(),
    snapshot() {
      const a=new Float32Array(e.memory.buffer,e.fruit_snapshot(),512);
      const s={phase:a[0],score:a[1],best:a[2],misses:a[3],lives:a[4],duration:a[5],elapsed:a[6],combo:a[7],maxCombo:a[8],comboMs:a[9],bombHit:!!a[10],down:!!a[11],fruit:[],particles:[],trail:[]};
      Object.assign(s,{mode:a[12],level:a[13],energy:a[14],feverMs:a[15],feedbackMs:a[16],points:a[17],feedbackKind:a[18],feedbackX:a[19],feedbackY:a[20],cutCount:a[21]});
      let at=22;
      for(let i=0;i<8;i++,at+=10)s.fruit.push({active:!!a[at],cut:!!a[at+1],bomb:!!a[at+2],kind:a[at+3],x:a[at+4],y:a[at+5],angle:a[at+6],cutAge:a[at+7],radius:a[at+8],golden:!!a[at+9]});
      for(let i=0;i<40;i++,at+=4)s.particles.push({x:a[at],y:a[at+1],life:a[at+2],kind:a[at+3]});
      const count=a[at++];
      for(let i=0;i<count;i++,at+=3)s.trail.push({x:a[at],y:a[at+1],start:!!a[at+2]});
      return s;
    },
  };
}
