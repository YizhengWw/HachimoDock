#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
#define PET_SURFACE_NODES 32
typedef struct {
  uint16_t x,y,w,h,color;
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
  char tile_var[32],tile_max_var[32],show_var[32];
  uint8_t tile_cols,tile_rows,tile_row,tile_index;
  int32_t show_min,show_max;
  bool hidden,disabled,pressed;
  uint8_t state_mask,host_mask;
} pet_surface_node_t;
typedef struct {uint8_t image,count,cell_w,cell_h;uint32_t chars[49];uint8_t metrics[49][3];} pet_surface_font_t;
typedef struct {bool enabled,todos;uint16_t x,y,w,h,colors[5];} pet_surface_data_view_t;
typedef struct {char entity[16];uint8_t image,columns,fps;} pet_surface_scene_sprite_t;
typedef struct {bool enabled;uint16_t x,y,w,h,colors[5];uint8_t count;pet_surface_scene_sprite_t sprites[4];} pet_surface_scene_view_t;
typedef struct {bool app,graphics,typography,interaction,game_view,media_touch;uint16_t game_x,game_y,game_w,game_h;pet_surface_data_view_t data_view;pet_surface_scene_view_t scene_view;uint16_t background;uint8_t image_count; char image_ids[4][16]; uint16_t image_w[4],image_h[4]; bool image_jpeg[4]; uint8_t font_count;pet_surface_font_t fonts[2];uint8_t count;pet_surface_node_t nodes[PET_SURFACE_NODES];} pet_surface_t;
bool pet_surface_parse(const cJSON *json,pet_surface_t *out);
