/** The package profile owns binding capacity; atlases can be shared by entities. */
export function validateSceneViewBindings(surface,runtime){
  const v=surface?.scene_view;if(v===undefined)return;
  const max=runtime?.graphics==='p4-graphics-v2'?8:4;
  const need=value=>{if(!value)throw Error('Invalid profile-scoped scene sprite bindings');};
  need(v?.engine==='p4-scene-surface-v1'&&Array.isArray(v.sprites)&&v.sprites.length<=max);
  const seen=new Set();
  for(const s of v.sprites){
    need(s&&Object.keys(s).length===4&&Object.keys(s).every(k=>['entity','image','columns','fps'].includes(k)));
    need(typeof s.entity==='string'&&/^[A-Za-z0-9_.-]{1,15}$/.test(s.entity)&&!seen.has(s.entity));seen.add(s.entity);
    need(!runtime||runtime.scene?.entities?.some(e=>e.id===s.entity));
    const im=surface.images?.find(im=>im.id===s.image);
    need(im&&im.encoding===undefined&&Number.isInteger(s.columns)&&s.columns>=1&&s.columns<=8&&im.frame_width%s.columns===0&&im.frame_width/s.columns>=8&&Number.isInteger(s.fps)&&s.fps>=1&&s.fps<=20);
  }
}
