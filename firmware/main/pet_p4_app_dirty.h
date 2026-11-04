/* Exact bounded dirty reuse of the existing clean-frame cache. No new DSL. */
#pragma once
#include "pet_p4_app_cache.h"
#include <stddef.h>
typedef struct { int x,y,right,bottom; } pet_app_dirty_rect_t;
static inline bool pet_app_dirty_empty(const pet_app_dirty_rect_t *r){
  return r->x>=r->right||r->y>=r->bottom;
}
static inline void pet_app_dirty_add(pet_app_dirty_rect_t *r,int x,int y,int right,int bottom){
  if(x<0)x=0;if(y<0)y=0;if(right>640)right=640;if(bottom>480)bottom=480;
  if(x>=right||y>=bottom)return;
  if(pet_app_dirty_empty(r)){*r=(pet_app_dirty_rect_t){x,y,right,bottom};return;}
  if(x<r->x)r->x=x;if(y<r->y)r->y=y;
  if(right>r->right)r->right=right;if(bottom>r->bottom)r->bottom=bottom;
}
static inline void pet_app_dirty_node_bounds(const pet_surface_t *s,const pet_surface_node_t *source,
    uint8_t page,pet_app_dirty_rect_t *r){
  if(source->hidden||!pet_surface_node_on_page(source,page))return;
  const pet_surface_viewport_t *v=pet_surface_node_viewport(s,source);
  pet_surface_node_t n;
  if(!pet_surface_project(v,source,v?v->offset:0,&n))return;
  int x=n.x,y=n.y,right=x+n.w,bottom=y+n.h;
  if(v){if(x<v->x)x=v->x;if(y<v->y)y=v->y;
    if(right>v->x+v->w)right=v->x+v->w;if(bottom>v->y+v->h)bottom=v->y+v->h;}
  pet_app_dirty_add(r,x,y,right,bottom);
}
static inline bool pet_app_dirty_supported(const pet_surface_t *s){
  if(!s||s->count>PET_SURFACE_NODES||!pet_app_frame_eligible(s)||!s->graphics||s->image_count||s->font_count)return false;
#if PET_CANVAS_ENABLED
  if(s->canvas.count)return false;
#endif
  for(unsigned i=0;i<s->count;i++){
    const pet_surface_node_t *n=&s->nodes[i];
    if(n->type!=0&&n->type!=1&&n->type!=2&&n->type!=7&&n->type!=8)return false;
    /* Label icons can intentionally extend outside a too-small button. */
    if(n->type==8&&n->icon>=0)return false;
  }
  return true;
}
/* The first visible layer's exact opaque center is overwritten by that layer's
 * own opaque RGB565 fill before any later node, overlay, blend read or cache
 * copy can observe it, so whatever the clear left there is never visible.
 * Fonts, images and canvas content only draw after that layer; a false
 * pet_app_opaque_center keeps the full clear. No buffer, order or raster change. */
static inline bool pet_app_clear_center_supported(const pet_surface_t *s){
  return s&&s->count<=PET_SURFACE_NODES&&pet_app_frame_eligible(s)&&s->graphics;
}
/* Exact keys, never hash authority. All layers intersecting this rectangle
 * must be replayed in their ORIGINAL order through the same raster functions.
 * A false result requires full legacy repaint. No extra framebuffer or owner. */
static inline bool pet_app_dirty_content_plan(const pet_app_frame_key_t *k,const pet_surface_t *s,
    const char *const values[9],uint8_t page,int progress,pet_app_dirty_rect_t *out){
  if(!k||!s||!values||!out||k->page!=page
      ||!pet_app_dirty_supported(s)||!pet_app_dirty_supported(&k->surface))return false;
  const size_t head=offsetof(pet_surface_t,nodes),tail=head+sizeof(s->nodes);
  if(memcmp(&k->surface,s,head)||memcmp((const char*)&k->surface+tail,(const char*)s+tail,sizeof(*s)-tail))return false;
  pet_app_dirty_rect_t r={0};
  for(unsigned i=0;i<s->count;i++){
    const pet_surface_node_t *old=&k->surface.nodes[i],*n=&s->nodes[i];
    bool changed=memcmp(old,n,sizeof(*n))!=0;
    if(n->type==1||n->type==8){
      if(n->bind<9)changed|=strcmp(k->values[n->bind],values[n->bind]?values[n->bind]:"")!=0;
      else if(n->bind==9)changed|=k->progress!=progress;
    }else if(n->type==2&&!n->var_bind[0])changed|=k->progress!=progress;
    if(changed){pet_app_dirty_node_bounds(&k->surface,old,page,&r);pet_app_dirty_node_bounds(s,n,page,&r);}
  }
  *out=r;return true;
}
static inline bool pet_app_dirty_plan(const pet_app_frame_key_t *k,const pet_surface_t *s,
    const char *const values[9],uint8_t page,int progress,pet_app_dirty_rect_t *out){
  return k&&k->valid&&pet_app_dirty_content_plan(k,s,values,page,progress,out);
}
/* One exact opaque center of the first visible layer. Its RGB565 stores
 * overwrite all old background values there before any consumer sees them.
 * AA corners, opacity, stroke, unsupported and malformed geometry fall back. */
static inline bool pet_app_opaque_center(const pet_surface_t *s,uint8_t page,pet_app_dirty_rect_t *out){
  if(!s||!out||!s->graphics||s->count>PET_SURFACE_NODES)return false;
  for(unsigned i=0;i<s->count;i++){
    const pet_surface_node_t *source=&s->nodes[i];
    if(source->hidden||!pet_surface_node_on_page(source,page))continue;
    const pet_surface_viewport_t *v=pet_surface_node_viewport(s,source);
    pet_surface_node_t n;
    if(!pet_surface_project(v,source,v?v->offset:0,&n))continue;
    if((n.type!=0&&n.type!=8)||n.opacity!=255||n.opacity_end!=255||n.stroke_width
        ||!n.w||!n.h||n.radius>n.w/2||n.radius>n.h/2)return false;
    int x=n.x,y=n.y+n.radius,right=n.x+n.w,bottom=n.y+n.h-n.radius;
    if(v){if(x<v->x)x=v->x;if(y<v->y)y=v->y;
      if(right>v->x+v->w)right=v->x+v->w;if(bottom>v->y+v->h)bottom=v->y+v->h;}
    pet_app_dirty_rect_t r={0};pet_app_dirty_add(&r,x,y,right,bottom);
    if(pet_app_dirty_empty(&r))return false;
    *out=r;return true;
  }
  return false;
}
/* Copy only exact dirty rows back to the EXISTING clean-frame cache after
 * rasterization and before any confirmation/common overlays are painted. */
static inline void pet_app_dirty_copy(uint16_t *to,const uint16_t *from,const pet_app_dirty_rect_t *r){
  if(!to||!from||!r||r->x<0||r->y<0||r->right>640||r->bottom>480||pet_app_dirty_empty(r))return;
  for(int y=r->y;y<r->bottom;y++)memcpy(to+y*640+r->x,from+y*640+r->x,(size_t)(r->right-r->x)*sizeof(*to));
}
