#pragma once
#include "pet_p4_service_bridge.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
#include "pet_p4_canvas.h"
#include "pet_p4_host_collection.h"
#include "pet_p4_render_policy.h"
/* Resource ceilings. The v1 numbers bound every package without
 * graphics:"p4-graphics-v2"; the arrays hold the larger v2 ceilings only on a
 * PET_P4_GRAPHICS_V2 build so unflagged images stay byte-identical. */
#define PET_SURFACE_V1_IMAGES 4
#define PET_SURFACE_V1_IMAGE_PIXELS 98304U
#define PET_SURFACE_V1_FONTS 2
#define PET_SURFACE_V1_FONT_CHARS 49
#if PET_P4_GRAPHICS_V2
#define PET_SURFACE_IMAGES 6
#define PET_SURFACE_IMAGE_PIXELS 196608U
#define PET_SURFACE_IMAGE_BYTES (256U*1024U)
#define PET_SURFACE_FONTS 4
#define PET_SURFACE_FONT_CHARS 98
#else
#define PET_SURFACE_IMAGES 4
#define PET_SURFACE_IMAGE_PIXELS 98304U
#define PET_SURFACE_IMAGE_BYTES (128U*1024U)
#define PET_SURFACE_FONTS 2
#define PET_SURFACE_FONT_CHARS 49
#endif
#if PET_P4_KEYFRAME_BG
#define PET_SURFACE_KEYFRAMES 4
#endif
#if PET_P4_PARTICLES
#define PET_SURFACE_PARTICLE_EMITTERS 2
#define PET_SURFACE_PARTICLE_DOTS 64
/* One decorative emitter: dots live inside x/y/w/h, never collide, never move
 * nodes. trigger_action bursts `count` dots on that transition action;
 * trigger_tick_ms emits one dot per interval. when_* is an optional int var
 * guard resolved by the runtime owner into when_current/when_ready. */
typedef struct {
  uint16_t x,y,w,h,color,life_ms,trigger_tick_ms,spread;
  uint8_t count,budget,speed,shape,page,when_op;
  int16_t gravity;
  char trigger_action[48],when_var[32];
  int32_t when_value,when_current;bool when_ready;
} pet_surface_particles_t;
#endif
#include "pet_p4_component_features.h"
#if PET_P4_APP_SHELL
#include "pet_p4_app_shell.h"
#endif
#if PET_P4_LONGTEXT
#include "pet_p4_text_flow.h"
#endif
#if PET_P4_DATA_ROWS
#include "pet_p4_widget_data.h"
#endif
#if PET_P4_QUOTA_V3
#define PET_SURFACE_NODES 64 /* array capacity = p4-quota-v3 ceiling (v2 packages stay at 48) */
#else
#define PET_SURFACE_NODES 48 /* array capacity = p4-quota-v2 ceiling */
#endif
#define PET_SURFACE_NODE_CELLS 9 /* p4-cells-v1 grid node type */
#if PET_P4_CLIP
#define PET_SURFACE_NODE_CLIP 20 /* p4-clip-v1 node (Track C); asset path kept in text */
#define PET_SURFACE_EXT_TYPE(t) ((t)==PET_SURFACE_NODE_CLIP)
#else
#define PET_SURFACE_EXT_TYPE(t) 0
#endif
#define PET_SURFACE_V1_NODES 32 /* budget for packages without quota p4-quota-v2 */
#define PET_SURFACE_VIEWPORTS 3
typedef struct {
  int16_t x,y;
  uint16_t w,h,color;
  uint8_t type,page,scale,radius,bind,opacity,opacity_end;
  char text[121];
  int8_t image;
  uint16_t rotate_ms;
  char action[48];
  bool floating;
  uint8_t font_size,line_height,align,valign,padding,gap,icon_size,weight;
  uint16_t foreground,stroke;
  uint8_t stroke_width;
  int8_t icon;
  bool vertical;
  char key_for[48];
  char var_bind[32];
#if PET_SERVICE_BRIDGE_ENABLED
  char image_bind[16],value_param[32];int32_t value_min,value_max;
#endif
  char field_bind[16];
  int32_t bound_value;
  char tile_var[32],tile_max_var[32],show_var[32];
  uint8_t tile_cols,tile_rows,tile_row,tile_index;
  int32_t show_min,show_max;
  bool hidden,disabled,pressed,in_view;
  uint8_t view_index;
#if PET_P4_QUOTA_V3
  uint16_t state_mask; /* 16 states under p4-quota-v3 */
#else
  uint8_t state_mask;
#endif
  uint8_t host_mask;
#if PET_P4_TEXT_TABLE
  char text_at[32]; /* p4-texts-v1: int var indexing the package text table */
#endif
#if PET_P4_CELLS_V1
  /* p4-cells-v1 node: image/tile_cols hold the optional atlas; the cursor outline follows two int vars. */
  char cursor_x[32],cursor_y[32];
  uint16_t cursor_color;
  int16_t cursor_col,cursor_row; /* resolved per snapshot, -1 = hidden */
#endif
#if PET_HOST_COLLECTION_ENABLED
  uint8_t collection_slot,collection_field;
#endif
#if PET_P4_LONGTEXT
  int8_t text_id; /* p4-longtext-v1 line node: index into surface texts, -1 = none */
  uint8_t text_line; /* line index inside the current page */
#endif
#if PET_P4_DATA_ROWS
  uint8_t row_bind; /* p4-data-rows-v1: 1 + selected-row field, 0 = none */
#endif
#if PET_P4_APP_SHELL
  uint8_t tab_slot; /* p4-app-shell-v1: 1 + tab index of an expanded tab button */
#endif
#if PET_P4_BIG_IMAGE
  int8_t big; char big_x_var[32],big_y_var[32]; int32_t big_x,big_y; /* p4-big-image-v1 pannable tiles (Track C) */
#endif
} pet_surface_node_t;
typedef struct {uint8_t image,count,cell_w,cell_h;uint32_t chars[PET_SURFACE_FONT_CHARS];uint8_t metrics[PET_SURFACE_FONT_CHARS][3];} pet_surface_font_t;
typedef struct {bool enabled,todos;
#if PET_P4_DATA_ROWS
  bool rows; /* generic label/value/detail/meta layout */
#endif
  uint16_t x,y,w,h,colors[5];} pet_surface_data_view_t;
#if PET_P4_LONGTEXT
typedef struct {char id[16];char text[PET_TEXT_FLOW_MAX_BYTES+1];char page_var[32],count_var[32];uint16_t w;uint8_t font_size,line_height,lines_per_page;bool used;} pet_surface_longtext_t;
#endif
typedef struct {char entity[16];uint8_t image,columns,fps;} pet_surface_scene_sprite_t;
#define PET_SURFACE_V1_SCENE_SPRITES 4
#if PET_P4_GRAPHICS_V2
#define PET_SURFACE_SCENE_SPRITES 8
#else
#define PET_SURFACE_SCENE_SPRITES 4
#endif
typedef struct {bool enabled;uint16_t x,y,w,h,colors[5];uint8_t count;pet_surface_scene_sprite_t sprites[PET_SURFACE_SCENE_SPRITES];} pet_surface_scene_view_t;
typedef struct {bool enabled;uint16_t x,y,w,h,content_height;char offset_var[32];int32_t offset;char id[16];} pet_surface_viewport_t;
typedef struct {
#if PET_SERVICE_BRIDGE_ENABLED
bool service_bridge;
#endif
bool app,graphics,typography,interaction,game_view,media_touch,variable_bindings;uint16_t game_x,game_y,game_w,game_h;pet_surface_data_view_t data_view;pet_surface_scene_view_t scene_view;pet_surface_viewport_t viewport;uint8_t viewport_count;pet_surface_viewport_t viewports[PET_SURFACE_VIEWPORTS];uint16_t background;uint8_t image_count; char image_ids[PET_SURFACE_IMAGES][16]; uint16_t image_w[PET_SURFACE_IMAGES],image_h[PET_SURFACE_IMAGES]; bool image_jpeg[PET_SURFACE_IMAGES]; uint8_t font_count;pet_surface_font_t fonts[PET_SURFACE_FONTS];uint8_t count;pet_surface_node_t nodes[PET_SURFACE_NODES];
#if PET_P4_GRAPHICS_V2
bool graphics_v2; /* parsed under the p4-graphics-v2 ceilings */
#endif
#if PET_P4_KEYFRAME_BG
uint8_t keyframes,keyframe_fps; /* images[0] JPEG background: 1 or 2..4 frames at 1..2 fps */
#endif
#if PET_P4_PARTICLES
uint8_t particle_count;pet_surface_particles_t particles[PET_SURFACE_PARTICLE_EMITTERS];
#endif
#if PET_CANVAS_ENABLED
pet_canvas_t canvas;
bool canvas_bindings_ready;
#endif
#if PET_HOST_COLLECTION_ENABLED
pet_host_collection_config_t collection;
#endif
#if PET_P4_APP_SHELL
bool shell; /* surface.shell = p4-app-shell-v1: pages 0..11, omitted page = shared */
pet_shell_tabs_t tabs;
#endif
#if PET_P4_LONGTEXT
uint8_t text_count;
pet_surface_longtext_t texts[2];
#endif
#if PET_P4_BIG_IMAGE
uint8_t big_count; char big_ids[2][16]; uint16_t big_w[2],big_h[2]; /* p4-big-image-v1 declarations (Track C) */
#endif
} pet_surface_t;
/* Page visibility shared by renderer/dirty/hit tests. Legacy nodes store 4 as
 * "every page"; shell nodes store PET_SHELL_PAGE_SHARED (12) or 16+page. */
#define PET_SURFACE_SHELL_PAGE_BASE 16
static inline bool pet_surface_node_on_page(const pet_surface_node_t *n,int page){
  return n->page==4||n->page==12||n->page==page||(n->page>=PET_SURFACE_SHELL_PAGE_BASE&&n->page-PET_SURFACE_SHELL_PAGE_BASE==page);
}
/* Declared page index of a node, -1 when it is shared. */
static inline int pet_surface_node_page_index(const pet_surface_node_t *n){
  return n->page==4||n->page==12?-1:n->page>=PET_SURFACE_SHELL_PAGE_BASE?n->page-PET_SURFACE_SHELL_PAGE_BASE:n->page;
}
bool pet_surface_parse(const cJSON *json,pet_surface_t *out);
bool pet_surface_parse_capped(const cJSON *json,pet_surface_t *out,unsigned max_nodes);
/* graphics_v2 selects the p4-graphics-v2 ceilings (6 images / 196608 palette
 * pixels / 4 fonts of 98 chars); false keeps the v1 numbers on every build. */
bool pet_surface_parse_profile(const cJSON *json,pet_surface_t *out,unsigned max_nodes,bool graphics_v2);
const pet_surface_viewport_t *pet_surface_node_viewport(const pet_surface_t *surface,const pet_surface_node_t *node);
int32_t pet_surface_clamp_offset(const pet_surface_viewport_t *viewport,int32_t offset);
bool pet_surface_project(const pet_surface_viewport_t*v,const pet_surface_node_t*node,int32_t offset,pet_surface_node_t*out);
bool pet_surface_hit(const pet_surface_viewport_t*v,const pet_surface_node_t*node,int32_t offset,int x,int y);
#if PET_HOST_COLLECTION_ENABLED
void pet_surface_resolve_collection(pet_surface_t *surface,const pet_host_collection_snapshot_t *snapshot,bool available);
#endif
#if PET_P4_APP_SHELL
/* Highlight the active tab button (value of surface.tabs.var). */
void pet_surface_resolve_tabs(pet_surface_t *surface,int32_t active);
#endif
#if PET_P4_LONGTEXT
/* Fill the expanded line nodes of text `id` for the page selected by its
 * page_var value; returns the page count (for count_var). */
unsigned pet_surface_resolve_longtext(pet_surface_t *surface,int id,int32_t page_value);
#endif
#if PET_P4_DATA_ROWS
/* Fill row_bind nodes from the selected row (NULL = no selection -> empty). */
void pet_surface_resolve_rows(pet_surface_t *surface,const pet_p4_data_row_t *selected);
#endif
