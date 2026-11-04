/** p4-keyframe-bg-v1 preview helpers: which frame the clock shows and which preview image carries it. */
export function keyframeDeclaration(surface){
  const im=surface?.images?.[0];
  if(!im||im.encoding!=='jpeg'||!(im.frames>1))return null;
  return {id:im.id,asset:im.asset,frames:im.frames,fps:im.fps||1};
}
export const keyframeAsset=(asset,frame)=>frame<2?asset:`${asset.replace(/\.png$/,'')}.${frame}.png`;
export const keyframeId=(id,frame)=>frame<2?id:`${id}.${frame}`;
/** Frame index 0..frames-1 for a clock in ms; swaps never faster than every 500 ms (fps <= 2). */
export const keyframeIndex=(declaration,clock)=>declaration?Math.floor((Math.max(0,clock||0)*declaration.fps)/1000)%declaration.frames:0;
/** Preview image for an image node: the current keyframe when the node shows the keyframe background. */
export function keyframeImage(images,surface,node,clock){
  const declaration=keyframeDeclaration(surface);
  if(!declaration||node.image!==declaration.id)return images.find(im=>im.id===node.image);
  const frame=keyframeIndex(declaration,clock)+1;
  return images.find(im=>im.id===keyframeId(declaration.id,frame))||images.find(im=>im.id===declaration.id);
}
/** Extra preview image entries for frames 2..N of a declared keyframe background. */
export function keyframeImageEntries(images,lookup){
  const declaration=keyframeDeclaration({images});
  if(!declaration)return [];
  return Array.from({length:declaration.frames-1},(_,k)=>({id:keyframeId(declaration.id,k+2),frame_width:640,frame_height:480,frames:1,fps:1,encoding:'jpeg',asset:keyframeAsset(declaration.asset,k+2),dataUrl:lookup(keyframeAsset(declaration.asset,k+2))}));
}
