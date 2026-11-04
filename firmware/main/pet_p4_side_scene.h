/* Generic, bounded tile-world physics. No component IDs, assets, audio or heap. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PET_SIDE_WIDTH 160
#define PET_SIDE_HEIGHT 16
#define PET_SIDE_CELLS (PET_SIDE_WIDTH * PET_SIDE_HEIGHT)
#define PET_SIDE_ACTORS 24
#define PET_SIDE_TILES 16
#define PET_SIDE_CELL 16
#define PET_SIDE_STEP_MS 20

enum { PET_SIDE_SOLID=1, PET_SIDE_ONE_WAY=2, PET_SIDE_BREAK=4,
       PET_SIDE_HAZARD=8 };
typedef enum { PET_SIDE_CONTROLLED, PET_SIDE_WALKER, PET_SIDE_STATIC,
               PET_SIDE_PLATFORM } pet_side_behavior_t;
typedef enum { PET_SIDE_NONE, PET_SIDE_DAMAGE, PET_SIDE_COLLECT,
               PET_SIDE_GOAL, PET_SIDE_CHECKPOINT } pet_side_contact_t;
typedef enum { PET_SIDE_READY, PET_SIDE_RUNNING, PET_SIDE_PAUSED,
               PET_SIDE_WON, PET_SIDE_LOST } pet_side_phase_t;
typedef struct { uint8_t flags, replacement; uint16_t reward; } pet_side_tile_t;
typedef struct {
  char id[16];
  uint16_t x,y;
  uint8_t width,height,skin;
  pet_side_behavior_t behavior;
  pet_side_contact_t contact;
  int16_t speed;
  uint16_t range_min,range_max,value;
  bool vertical,solid,stompable,edge_turn;
} pet_side_actor_def_t;
typedef struct {
  uint8_t width,height,tile_count,actor_count,lives;
  uint16_t run_speed,acceleration,friction,gravity,jump_speed,terminal_speed;
  uint8_t cells[PET_SIDE_CELLS];
  pet_side_tile_t tiles[PET_SIDE_TILES];
  pet_side_actor_def_t actors[PET_SIDE_ACTORS];
} pet_side_config_t;
typedef struct {
  int32_t x,y,vx,vy; /* Q8 pixels. */
  int8_t facing;
  bool active,grounded;
} pet_side_actor_t;
typedef struct {
  pet_side_config_t config;
  uint8_t cells[PET_SIDE_CELLS];
  pet_side_actor_t actors[PET_SIDE_ACTORS];
  pet_side_phase_t phase;
  int32_t score;
  uint16_t coins;
  uint8_t lives,player,checkpoint;
  int8_t direction;
  int8_t carrier;
  uint16_t hurt_ms,invulnerable_ms,coyote_ms,jump_buffer_ms;
  bool jump_down,clock_set;
  uint64_t clock_ms;
  uint32_t elapsed_ms,revision;
} pet_side_scene_t;

bool pet_side_config_valid(const pet_side_config_t *config);
bool pet_side_init(pet_side_scene_t *scene,const pet_side_config_t *config);
void pet_side_restart(pet_side_scene_t *scene,uint64_t now_ms);
void pet_side_axis(pet_side_scene_t *scene,int horizontal);
void pet_side_jump(pet_side_scene_t *scene,bool down,uint64_t now_ms);
void pet_side_pause(pet_side_scene_t *scene,bool paused,uint64_t now_ms);
bool pet_side_process(pet_side_scene_t *scene,uint64_t now_ms);
/* A checkpoint is a declared marker index, never an arbitrary saved position. */
bool pet_side_restore_checkpoint(pet_side_scene_t *scene,unsigned checkpoint);
int32_t pet_side_camera(const pet_side_scene_t *scene,unsigned viewport_pixels);

#define PET_SIDE_SKINS 8
typedef struct {
  uint8_t idle[8],walk[8],air[8],hurt[8];
  uint8_t counts[4],fps,width,height;
} pet_side_skin_t;
typedef struct {
  char image[16];
  uint8_t columns,rows,scale,skin_count;
  uint16_t x,y,w,h,sky;
  uint8_t tiles[16];
  pet_side_skin_t skins[PET_SIDE_SKINS];
} pet_side_art_t;
typedef void (*pet_side_draw_fn)(void*context,int x,int y,int w,int h,unsigned frame,bool flip);
/* Shared bounded draw-list generation; device and preview use the same camera,
 * skin selection and animation phase. The target clips to art's viewport. */
void pet_side_draw(const pet_side_scene_t*scene,const pet_side_art_t*art,pet_side_draw_fn draw,void*context);
