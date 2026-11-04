/** Bounded content coordinates; rendering and input share these exact bounds. */
export function surfaceViewports(surface){
 return surface?.viewports?.items??(surface?.viewport?[surface.viewport]:[]);
}
export function nodeViewport(surface,node){
 return node.view!==undefined?surface?.viewports?.items?.find(v=>v.id===node.view):node.in_view?surface?.viewport:undefined;
}
export function viewportOffset(surface,vars={},view){
 const v=view??surface?.viewport;if(!v)return 0;const n=vars[v.offset_var];
 return Math.min(v.content_height-v.h,Math.max(0,Number.isInteger(n)?n:0));
}
/** Stable bounded tuple: changing any region invalidates a pending gesture. */
export function viewportContext(surface,vars={}){
 return JSON.stringify(surfaceViewports(surface).map(v=>viewportOffset(surface,vars,v)));
}
export function projectNode(surface,node,vars={}){
 if(!node.in_view&&node.view===undefined)return node;const v=nodeViewport(surface,node);if(!v)return null;
 const n={...node,x:node.x+v.x,y:node.y+v.y-viewportOffset(surface,vars,v)};
 return n.x<v.x+v.w&&n.x+n.w>v.x&&n.y<v.y+v.h&&n.y+n.h>v.y?n:null;
}
export function hitNode(surface,node,vars,x,y){
 const n=projectNode(surface,node,vars),v=nodeViewport(surface,node);
 return !!n&&(!v||x>=v.x&&x<v.x+v.w&&y>=v.y&&y<v.y+v.h)&&x>=n.x&&x<n.x+n.w&&y>=n.y&&y<n.y+n.h;
}
