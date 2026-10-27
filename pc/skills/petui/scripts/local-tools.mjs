// Bounded offline tools. No package code, network, storage or widget-ID dispatch.
export const LOCAL_ENGINE='p4-local-tools-v1';
export const LOCAL_STATES=['ready','busy','result','editing','empty','offline'];
const mod=(v,n)=>(v%n+n)%n, clamp=(v,a,b)=>Math.max(a,Math.min(b,v)), pad=v=>String(v).padStart(2,'0');
export const civilDay=(y,m,d)=>Math.floor(Date.UTC(y,m-1,d)/86400000);
const date=day=>{const d=new Date(day*86400000);return [d.getUTCFullYear(),d.getUTCMonth()+1,d.getUTCDate()];};
const monthDays=(y,m)=>new Date(Date.UTC(y,m,0)).getUTCDate();
export function localTarget(kind,day,minute,now){
  if(now<0||minute<0||minute>1439)return -1;
  if(kind==='date')return day>=civilDay(2020,1,1)&&day<=civilDay(2099,12,31)?day*86400+minute*60:-1;
  const today=Math.floor(now/86400),dow=mod(today+4,7);
  if(kind==='weekend')return (today+(dow===0?-1:6-dow))*86400+minute*60;
  for(let i=0;i<8;i++){const d=today+i,w=mod(d+4,7),t=d*86400+minute*60;if((kind==='weekdays'?w>=1&&w<=5:w===6)&&(kind==='weekdays'||t>=now))return t;}return -1;
}
export function localEnabled(runtime,s,action){
  const c=runtime.local;if(!c)return true;const op=c.actions[action],phase=s.local?.phase||0;
  if(!op)return false;
  if(c.kind==='countdown')return ['cancel','up','down'].includes(op)?phase===3:true;
  if(phase===1)return op==='close';
  return op!=='exclude'||phase===2;
}
export function localInitial(s,wall){
  // Browser preview uses local civil time; firmware receives it via the heartbeat.
  const now=new Date();s.wall=wall??Math.floor(now.getTime()/1000)-now.getTimezoneOffset()*60;
  s.local={phase:0,field:0,pending:-1,revealAt:0,draftDay:0,draftMinute:0};return s;
}
const seconds=s=>s.wall<0?-1:s.wall+Math.floor(s.clock/1000);
const values=s=>Array.from({length:7},(_,i)=>s.vars[`local${i}`]);
function repair(c,v){v[0]=mod(v[0],c.groups.length);c.groups.forEach((g,i)=>{if(c.kind==='countdown'){v[1+2*i]=clamp(v[1+2*i],0,civilDay(2099,12,31));v[2+2*i]=clamp(v[2+2*i],0,1439);}else{v[1+2*i]&=(1<<g.items.length)-1;v[2+2*i]=clamp(v[2+2*i],-1,g.items.length-1);}});}
function assign(s,v){v.forEach((v,i)=>s.vars[`local${i}`]=v);s.state=LOCAL_STATES[s.local.phase];return s;}
export function localAction(runtime,state,action,entropy){
  if(!localEnabled(runtime,state,action))return state;
  const s=structuredClone(state),c=runtime.local,l=s.local,op=c.actions[action],v=values(s);repair(c,v);
  const at=v[0],a=1+at*2,b=a+1,g=c.groups[at],now=seconds(s);
  if(c.kind==='deck'){
    if(op==='previous'||op==='next'){v[0]=mod(at+(op==='next'?1:-1),c.groups.length);l.phase=0;}
    else if(op==='close'){l.phase=0;l.pending=-1;}
    else if(op==='reset'){v[a]=0;l.phase=0;}
    else {
      if(op==='exclude'){if(l.phase!==2||v[b]<0)return state;v[a]|=1<<v[b];l.phase=0;}
      let candidates=g.items.map((_,i)=>i).filter(i=>!(v[a]&(1<<i)));
      if(!candidates.length){l.phase=4;l.pending=-1;}
      else {if(candidates.length>1){const i=candidates.indexOf(v[b]);if(i>=0){candidates[i]=candidates[candidates.length-1];candidates.pop();}}
        const random=entropy??globalThis.crypto.getRandomValues(new Uint32Array(1))[0];
        l.pending=candidates[Math.floor((random>>>0)*candidates.length/4294967296)];l.revealAt=s.clock+c.reveal_ms;l.phase=1;}
    }
  } else {
    if(op==='cancel')l.phase=0;
    if(op==='edit'){if(l.phase===3){v[a]=l.draftDay;v[b]=l.draftMinute;l.phase=0;}else if(now<0)l.phase=5;else {l.draftDay=v[a]>=civilDay(2020,1,1)?v[a]:Math.floor(now/86400)+1;l.draftMinute=v[b];l.field=0;l.phase=3;}}
    if(op==='previous'||op==='next'){const step=op==='next'?1:-1;if(l.phase===3)l.field=mod(l.field+step,g.kind==='date'?5:2);else {v[0]=mod(at+step,c.groups.length);l.phase=0;}}
    if(op==='up'||op==='down'){const step=op==='up'?1:-1,f=l.field+(g.kind==='date'?0:3);let[y,m,d]=date(l.draftDay);
      if(f===0)y=clamp(y+step,2020,2099);if(f===1)m=mod(m-1+step,12)+1;if(f===2)d=mod(d-1+step,monthDays(y,m))+1;
      l.draftDay=civilDay(y,m,clamp(d,1,monthDays(y,m)));let h=Math.floor(l.draftMinute/60),min=l.draftMinute%60;
      if(f===3)h=mod(h+step,24);if(f===4)min=mod(min+step,60);l.draftMinute=h*60+min;
    }
  }return assign(s,v);
}
export function localAdvance(runtime,s){const c=runtime.local,l=s.local,v=values(s);repair(c,v);
  if(c.kind==='deck'&&l.phase===1&&s.clock>=l.revealAt){v[2+2*v[0]]=l.pending;l.phase=2;l.pending=-1;}
  if(c.kind==='countdown'&&l.phase===5&&seconds(s)>=0)l.phase=0;
  assign(s,v);s.state=LOCAL_STATES[localView(runtime,s).phase];return s;
}
export function localView(runtime,s){const c=runtime.local,l=s.local,v=values(s),at=mod(v[0],c.groups.length),g=c.groups[at],a=1+2*at,b=a+1,now=seconds(s);
  const o={phase:l.phase,eyebrow:g.title,badge:`${at+1}/${c.groups.length}`,note:c.hint,headline:'',metricValue:'',metricLabel:'',metricUnit:'',progress:0};
  if(c.kind==='deck'){
    o.metricValue=String(g.items.filter((_,i)=>!(v[a]&(1<<i))).length);o.metricLabel='可选内容';const item=g.items[v[b]];
    o.headline=l.phase===1?c.busy:l.phase===4?'选项都排除啦':l.phase===2&&item?item.title:c.idle;
    if(l.phase===2&&item)o.note=item.note;if(l.phase===4)o.note='点击恢复选项，或切换一个分类。';
    o.progress=l.phase===1?clamp(100-Math.trunc((l.revealAt-s.clock)*100/c.reveal_ms),0,100):l.phase===2?100:0;return o;
  }
  const day=l.phase===3?l.draftDay:v[a],minute=l.phase===3?l.draftMinute:v[b];
  if(l.phase===3){const[y,m,d]=date(day),f=l.field+(g.kind==='date'?0:3);o.headline=`调整${['年','月','日','小时','分钟'][f]}`;o.metricValue=pad([y,m,d,Math.floor(minute/60),minute%60][f]);o.note=g.kind==='date'?`${y}-${pad(m)}-${pad(d)}  ${pad(Math.floor(minute/60))}:${pad(minute%60)}`:`目标时间  ${pad(Math.floor(minute/60))}:${pad(minute%60)}`;o.metricLabel='左右选项，上下调整';return o;}
  if(now<0){o.phase=5;o.headline='等待时间校准';o.note='连接新版 Pet Manager 校准时间后可离线使用。';o.metricValue='—';return o;}
  const target=localTarget(g.kind,day,minute,now);
  if(target<0){o.headline='给期待定个日子';o.metricValue='—';o.note='点设置时间，保存属于你的小期待。';return o;}
  const left=target-now;
  if(left<=0){o.phase=2;o.headline='到站啦，去享受吧';o.metricValue='已到站';o.progress=100;}
  else {o.headline=`下一站，${g.title}`;o.metricLabel='距离出发还有';if(left>=86400){o.metricValue=String(Math.ceil(left/86400));o.metricUnit='天';}else o.metricValue=`${pad(Math.floor(left/3600))}:${pad(Math.floor(left/60)%60)}:${pad(left%60)}`;o.progress=clamp(100-(left>86400?100:Math.trunc(left*100/86400)),0,99);}
  const[y,m,d]=date(Math.floor(target/86400));o.note=`${y}-${pad(m)}-${pad(d)}  ${pad(Math.floor(target%86400/3600))}:${pad(Math.floor(target%3600/60))}`;return o;
}
