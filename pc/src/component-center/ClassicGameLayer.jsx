import React from 'react';
export function ClassicGameLayer({view:v,frame:g}){
  if(!v||!g)return null;
  const c=Math.floor(Math.min(v.w/g.width,v.h/g.height)),ox=v.x+(v.w-c*g.width)/2,oy=v.y+(v.h-c*g.height)/2;
  return <g aria-label="真实游戏局面" data-content-view="game">{g.cells.map((t,i)=>{
    if(!t)return null;
    const x=ox+i%g.width*c,y=oy+Math.floor(i/g.width)*c,cx=x+c/2,cy=y+c/2;
    if(g.kind===3)return t===2?<g key={i}><ellipse cx={cx} cy={cy+1} rx={c/3} ry={c/3} fill="#dd634a"/><ellipse cx={cx+c/6} cy={y+c/5} rx={c/6} ry={c/12} fill="#49764c"/><circle cx={cx-c/8} cy={cy-c/9} r={c/12} fill="#ffdcB0"/></g>:<g key={i}><rect x={x+1} y={y+2} width={c-2} height={c-3} rx={c/3} fill="#437b56"/><rect x={x+3} y={y+3} width={c-6} height={c/3} rx={c/7} fill="#76a670"/>{t===3&&[-1,1].map(d=><g key={d}><circle cx={cx+d*c/5} cy={cy-c/8} r={c/9} fill="#fff9e2"/><ellipse cx={cx+d*c/5} cy={cy-c/8} rx="1" ry="2" fill="#233c2c"/></g>)}</g>;
    if(g.kind===5){
      // gomoku packs stone|cursor|last|win bits; board lines come from the package background.
      const stone=t&3,r=Math.max(3,c/2-2),parts=[];
      if(t&16)parts.push(<rect key="w" x={x+1} y={y+1} width={c-2} height={c-2} rx={c/3} fill="#ecba42"/>);
      if(stone)parts.push(<circle key="s0" cx={cx+1} cy={cy+1} r={r} fill="#463018"/>,<circle key="s1" cx={cx} cy={cy} r={r} fill={stone===1?"#1c1c22":"#f6f2e6"}/>,<circle key="s2" cx={cx-r/3} cy={cy-r/3} r={r/4+1} fill={stone===1?"#60606c":"#ffffff"}/>);
      if(stone&&(t&8))parts.push(<circle key="l" cx={cx} cy={cy} r={r/4+1} fill="#e04838"/>);
      if(t&4)parts.push(<rect key="c" x={x+1} y={y+1} width={c-2} height={c-2} fill="none" stroke="#e66030" strokeWidth="2"/>);
      return <g key={i}>{parts}</g>;
    }
    if(g.kind===4){
      if(t===3)return <g key={i}><ellipse cx={cx} cy={cy} rx={c/2-1} ry={c/2-2} fill="#f2b34e"/><ellipse cx={cx} cy={cy+c/5} rx={c/3} ry={c/5} fill="#ffe7aa"/><circle cx={cx+c/5} cy={cy-c/6} r={c/7} fill="#fffef0"/><ellipse cx={cx+c/4} cy={cy-c/6} rx="1" ry="2" fill="#3f3732"/><rect x={x+c-6} y={cy} width="6" height="4" rx="2" fill="#d67342"/></g>;
      if(t===4)return <ellipse key={i} cx={x+c*.75} cy={cy+c/8} rx={c/4} ry={c/5} fill="#db9246"/>;
      return <g key={i}><rect x={x} y={y} width={c} height={c} fill="#5e8f7e"/><rect x={x+2} y={y} width={c/5} height={c} fill="#94b996"/>{t===2&&<><rect x={x} y={y} width={c} height={c} rx={c/6} fill="#457364"/><rect x={x+3} y={y+3} width={c-6} height="3" fill="#a4c5a1"/></>}</g>;
    }
    return <g key={i}><rect x={x+1} y={y+1} width={c-2} height={c-2} rx={c/5} fill={['','#837ad3','#62b8cc','#e09d75','#adce90'][t]}/><rect x={x+3} y={y+3} width={c-6} height="2" fill="#ffffff" opacity=".4"/><rect x={x+3} y={y+c-4} width={c-6} height="2" fill="#000000" opacity=".18"/></g>;
  })}</g>;
}
