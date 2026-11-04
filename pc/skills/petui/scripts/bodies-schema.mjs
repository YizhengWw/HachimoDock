export const BODIES_ENGINE='p4-bodies-v1';
const field=(o,k,fallback)=>Object.hasOwn(o,k)?o[k]:fallback;
const integer=(v,lo,hi)=>Number.isInteger(v)&&v>=lo&&v<=hi;
const need=v=>{if(!v)throw Error('Invalid bounded bodies declaration');};
export function validateBodiesScene(s){
  need(s&&!Object.hasOwn(s,'motion')&&integer(s.tick_ms,20,200));
  const g=s.grid;need(g&&Object.keys(g).length===2&&integer(g.width,4,16)&&integer(g.height,4,16));
  need(integer(field(s,'gravity',0),-64,64)&&Array.isArray(s.entities)&&s.entities.length>=1&&s.entities.length<=12);
  const ids=new Map();for(const e of s.entities){
    need(typeof e.id==='string'&&!ids.has(e.id));ids.set(e.id,e);
    need(integer(field(e,'width',1),1,8)&&integer(field(e,'height',1),1,8)&&integer(e.x,0,g.width-(field(e,'width',1)))&&integer(e.y,0,g.height-(field(e,'height',1))));
    need(integer(field(e,'vx',0),-4,4)&&integer(field(e,'vy',0),-4,4));
  }
  need(Array.isArray(s.bodies)&&s.bodies.length>=1&&s.bodies.length<=8);const seen=new Set();
  for(const b of s.bodies){
    need(b&&Object.keys(b).every(k=>['entity','mass','restitution','static','rolling'].includes(k))&&ids.has(b.entity)&&!seen.has(b.entity));seen.add(b.entity);
    need(integer(field(b,'mass',1),1,64)&&integer(field(b,'restitution',100),0,100)&&['static','rolling'].every(k=>!Object.hasOwn(b,k)||typeof b[k]==='boolean'));
    const e=ids.get(b.entity);need((field(e,'collidable',true))===true&&(field(e,'bounds','clamp'))==='clamp');
    if(b.rolling)need(['rect','circle','ball','coin','capsule','triangle','diamond'].includes(field(e,'shape','rect')));
    if(b.static)need(!(field(e,'vx',0))&&!(field(e,'vy',0)));
  }
  for(const e of s.entities)if(!seen.has(e.id))need(!(field(e,'vx',0))&&!(field(e,'vy',0)));
  need(Array.isArray(s.rules)&&s.rules.length>=1&&s.rules.length<=20);
  for(const r of s.rules){need(Array.isArray(r.do)&&r.do.length>=1&&r.do.length<=4);for(const o of r.do){
    need(!['move','place','velocity','accelerate','bounce','charge','launch','cancel'].includes(o.op));
    if(o.op==='impulse')need(Object.keys(o).length===4&&Object.keys(o).every(k=>['op','entity','dx','dy'].includes(k))&&seen.has(o.entity)&&integer(o.dx,-64,64)&&integer(o.dy,-64,64)&&(o.dx||o.dy));
  }}
}
