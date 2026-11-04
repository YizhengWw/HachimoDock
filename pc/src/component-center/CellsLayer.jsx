import React from 'react';
import {rgb565} from './media-surface-model.js';

/** Match draw_app_cells: bounded hex rows, whole-pixel tiles, and an inset cursor. */
export default function CellsLayer({node:n,grid,cells,vars={},images=[]}){
  const {cols,rows,bits}=grid||{};
  if(![cols,rows].every(v=>Number.isInteger(v)&&v>=1&&v<=16)||![2,4].includes(bits)
    ||!Array.isArray(cells)||cells.length!==rows||![n.x,n.y,n.w,n.h].every(Number.isFinite))return null;
  const max=(1<<bits)-1;
  // Reject malformed snapshots without allocating or iterating beyond 256 cells.
  if(cells.some(row=>typeof row!=='string'||row.length!==cols||!/^[0-9a-f]+$/i.test(row)
    ||[...row].some(d=>parseInt(d,16)>max)))return null;
  const tw=Math.trunc(n.w/cols),th=Math.trunc(n.h/rows);
  if(tw<1||th<1)return null;
  const atlas=n.image!==undefined,columns=n.columns;
  if(atlas&&(!Number.isInteger(columns)||columns<2||columns>16))return null;
  // Keep the package image ID unchanged; cells atlases are one row, not keyframes.
  const image=atlas?images.find(im=>im.id===n.image):null;
  const inset=tw<8||th<8?1:2,w=tw-2*inset,h=th-2*inset,tiles=[];
  for(let row=0;row<rows;row++)for(let col=0;col<cols;col++){
    const value=parseInt(cells[row][col],16);
    if(!value)continue;
    const x=n.x+col*tw,y=n.y+row*th,key=row*cols+col;
    if(atlas){
      if(value>=columns||!image?.dataUrl)continue;
      tiles.push(<svg key={key} x={x} y={y} width={tw} height={th} viewBox={`0 0 ${tw} ${th}`} overflow="hidden">
        <image href={image.dataUrl} x={-value*tw} y={0} width={tw*columns} height={th} preserveAspectRatio="none"/>
      </svg>);
    }else if(w>0&&h>0){
      const color=value===1?n.color:value===2?(n.foreground??n.color):n.stroke;
      tiles.push(<rect key={key} x={x+inset} y={y+inset} width={w} height={h}
        rx={value<=2?Math.trunc(Math.min(w,h)/2):0} fill={rgb565(color)}/>);
    }
  }
  const cx=vars[n.cursor?.x_var],cy=vars[n.cursor?.y_var];
  if(n.cursor&&Number.isInteger(cx)&&Number.isInteger(cy)&&cx>=0&&cx<cols&&cy>=0&&cy<rows){
    const x=n.x+cx*tw,y=n.y+cy*th;
    tiles.push(<g key="cursor" fill={rgb565(n.cursor.color)}>
      <rect x={x} y={y} width={tw} height={2}/><rect x={x} y={y+th-2} width={tw} height={2}/>
      <rect x={x} y={y} width={2} height={th}/><rect x={x+tw-2} y={y} width={2} height={th}/>
    </g>);
  }
  return <g data-content-view="cells">{tiles}</g>;
}
