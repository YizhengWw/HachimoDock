import React from "react";
import {rgb565} from "./media-surface-model.js";

// Same five-row contract as the firmware. No invented values in covers.
export function dataSurfacePage(snapshot, page=0) {
  const rows=Array.isArray(snapshot?.rows)?snapshot.rows:[],pages=Math.max(1,Math.ceil(rows.length/5));
  const at=((page%pages)+pages)%pages;
  return {...snapshot,rows:rows.slice(at*5,at*5+5),page:at,pages};
}
export default function DataSurfaceLayer({view:v,snapshot,page,source}) {
  if(!v)return null;
  const d=dataSurfacePage(snapshot,page),[ink,muted,accent,done,card]=v.colors.map(rgb565),todos=v.layout==="todos";
  const rowHeight=Math.floor((v.h-58)/5);
  const label=(value,x,y,w,size,color,height=24)=><svg x={x} y={y} width={w} height={height} overflow="hidden"><text x="0" y={size} fontSize={size} fill={color} fontFamily='"PingFang SC","Microsoft YaHei",sans-serif'>{value}</text></svg>;
  return <g data-content-view="data">
    {label(d.date?(todos?d.date.slice(5):d.date):"等待连接",v.x,v.y,v.w-110,14,muted)}
    {label(`${d.page+1} / ${d.pages}`,v.x+v.w-85,v.y,85,14,muted)}
    {d.rows.map((r,i)=>{const y=v.y+28+i*rowHeight,tone=d.stale?muted:todos?(r.tone<0?done:accent):source==="stocks.watchlist"?(r.tone>0?"#ff6466":r.tone<0?"#4fd495":ink):ink;
      return <g key={r.id||i}><rect x={v.x} y={y} width={v.w} height={rowHeight-5} rx="10" fill={card}/>
        {todos?<><rect x={v.x+10} y={y+10} width="3" height={rowHeight-25} rx="1" fill={tone}/>{label(`${r.label||""}${r.meta||""}`,v.x+24,y+4,v.w-38,20,tone,27)}{label(r.value,v.x+24,y+30,v.w-146,13,muted,21)}{label(r.detail,v.x+v.w-112,y+30,100,13,tone,21)}</>:<>{label(r.label,v.x+14,y+5,v.w/2-22,16,muted)}{label(r.value,v.x+v.w/2,y+3,v.w/2-14,22,tone,28)}{label(r.meta,v.x+14,y+30,v.w/2-22,12,muted,21)}{label(r.detail,v.x+v.w/2,y+32,v.w/2-14,12,source==="stocks.watchlist"?tone:muted,19)}</>}
      </g>;
    })}
    {!d.rows.length&&label(snapshot?"暂无内容":"等待 PC 数据",v.x+18,v.y+110,v.w-36,24,ink,40)}
    {label(!snapshot?"连接 Pet Manager 后显示":d.stale?"数据已过期，请检查连接":d.message,v.x,v.y+v.h-24,v.w,13,muted)}
  </g>;
}
