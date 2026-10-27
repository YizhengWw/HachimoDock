export const keyHint=(action,bindings=[])=>({
  'button.sw1.short_press':'SW1','button.sw2.short_press':'SW2','button.sw3.short_press':'SW3',
  'button.encoder.short_press':'中按','button.encoder.long_press':'中长',
  'knob.rotate_ccw':'左','knob.rotate_cw':'右','joystick.up':'上','joystick.down':'下',
}[bindings.find(b=>b.action===action)?.event]||'—');

export function glyphs(surface,images){
  const map=new Map();
  for(const font of surface.fonts||[]){
    const declaration=surface.images.find(im=>im.id===font.image),image=images.find(im=>im.id===font.image);
    if(!declaration||!image)continue;
    const [cw,ch]=font.cell||[32,32];
    [...font.chars].forEach((char,i)=>map.set(char,{...declaration,...image,metrics:font.metrics[i],cellWidth:cw,cellHeight:ch,x:i%(declaration.frame_width/cw)*cw,y:Math.floor(i/(declaration.frame_width/cw))*ch}));
  }return map;
}
