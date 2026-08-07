/*
 * [Input] Validated legacy presets or declarative v1/v2 scene configuration.
 * [Output] Heap-free fixed-grid game state with semantic shapes and sprite references.
 * [Pos] Shared bounded gameplay model for mini-app parsing and rendering.
 * [Sync] If this file changes, update `esp-p4-runtime/.folder.md` and `protocol.md`.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_GAME_GRID_MAX_WIDTH 16
#define PET_P4_GAME_GRID_MAX_HEIGHT 16
#define PET_P4_GAME_GRID_MAX_CELLS \
  (PET_P4_GAME_GRID_MAX_WIDTH * PET_P4_GAME_GRID_MAX_HEIGHT)
#define PET_P4_GAME_MAX_ENTITIES 12
#define PET_P4_GAME_MAX_RULES 20
#define PET_P4_GAME_MAX_OPS_PER_RULE 4
#define PET_P4_GAME_ENTITY_ID_MAX 16
#define PET_P4_GAME_ACTION_MAX 48

typedef enum {
  PET_P4_GAME_NONE = 0,
  PET_P4_GAME_BOUNDED,
  PET_P4_GAME_BLOCKS,
  PET_P4_GAME_SNAKE,
  PET_P4_GAME_FLAPPY,
} pet_p4_game_kind_t;

typedef enum {
  PET_P4_GAME_COMMAND_START = 0,
  PET_P4_GAME_COMMAND_LEFT,
  PET_P4_GAME_COMMAND_RIGHT,
  PET_P4_GAME_COMMAND_ROTATE,
  PET_P4_GAME_COMMAND_DROP,
  PET_P4_GAME_COMMAND_FLAP,
} pet_p4_game_command_t;

typedef enum {
  PET_P4_GAME_BOUNDS_CLAMP = 0,
  PET_P4_GAME_BOUNDS_WRAP,
  PET_P4_GAME_BOUNDS_BOUNCE,
  PET_P4_GAME_BOUNDS_HIDE,
  PET_P4_GAME_BOUNDS_STOP,
} pet_p4_game_bounds_t;

typedef enum {
  PET_P4_GAME_EDGE_NONE = 0,
  PET_P4_GAME_EDGE_LEFT,
  PET_P4_GAME_EDGE_RIGHT,
  PET_P4_GAME_EDGE_TOP,
  PET_P4_GAME_EDGE_BOTTOM,
} pet_p4_game_edge_t;

typedef enum {
  PET_P4_GAME_SHAPE_RECT = 0,
  PET_P4_GAME_SHAPE_PLAYER_SHIP,
  PET_P4_GAME_SHAPE_ENEMY_SHIP,
  PET_P4_GAME_SHAPE_BULLET,
  PET_P4_GAME_SHAPE_STAR,
  PET_P4_GAME_SHAPE_PADDLE,
  PET_P4_GAME_SHAPE_BALL,
  PET_P4_GAME_SHAPE_CIRCLE,
  PET_P4_GAME_SHAPE_CAPSULE,
  PET_P4_GAME_SHAPE_TRIANGLE,
  PET_P4_GAME_SHAPE_DIAMOND,
  PET_P4_GAME_SHAPE_HEART,
  PET_P4_GAME_SHAPE_CLOUD,
  PET_P4_GAME_SHAPE_COIN,
  PET_P4_GAME_SHAPE_CHARACTER,
} pet_p4_game_shape_t;

typedef struct {
  char id[PET_P4_GAME_ENTITY_ID_MAX];
  int8_t x;
  int8_t y;
  uint8_t width;
  uint8_t height;
  uint8_t tone;
  int8_t vx;
  int8_t vy;
  pet_p4_game_bounds_t bounds;
  pet_p4_game_shape_t shape;
  int8_t sprite_index;
  bool active;
  bool collidable;
} pet_p4_game_entity_t;

typedef enum {
  PET_P4_GAME_TRIGGER_ACTION = 0,
  PET_P4_GAME_TRIGGER_TICK,
  PET_P4_GAME_TRIGGER_COLLISION,
  PET_P4_GAME_TRIGGER_EDGE,
  PET_P4_GAME_TRIGGER_BLOCKED,
} pet_p4_game_trigger_t;

typedef enum {
  PET_P4_GAME_OP_MOVE = 0,
  PET_P4_GAME_OP_VELOCITY,
  PET_P4_GAME_OP_ACCELERATE,
  PET_P4_GAME_OP_PLACE,
  PET_P4_GAME_OP_SHOW,
  PET_P4_GAME_OP_HIDE,
  PET_P4_GAME_OP_SCORE_ADD,
  PET_P4_GAME_OP_SCORE_SET,
  PET_P4_GAME_OP_RUN,
  PET_P4_GAME_OP_STOP,
  PET_P4_GAME_OP_RESTART,
  PET_P4_GAME_OP_BOUNCE,
  PET_P4_GAME_OP_TONE,
} pet_p4_game_op_kind_t;

typedef struct {
  pet_p4_game_op_kind_t kind;
  int8_t entity_index;
  int8_t source_index;
  int8_t dx;
  int8_t dy;
  int8_t vx;
  int8_t vy;
  int8_t x_min;
  int8_t x_max;
  int8_t y_min;
  int8_t y_max;
  bool has_x;
  bool has_y;
  bool random_x;
  bool random_y;
  int16_t value;
  uint8_t tone;
  uint8_t axis_mask;
} pet_p4_game_op_t;

typedef struct {
  pet_p4_game_trigger_t trigger;
  char action[PET_P4_GAME_ACTION_MAX];
  int8_t entity_index;
  int8_t with_index;
  pet_p4_game_edge_t edge;
  uint8_t op_count;
  pet_p4_game_op_t ops[PET_P4_GAME_MAX_OPS_PER_RULE];
} pet_p4_game_rule_t;

typedef struct {
  uint8_t width;
  uint8_t height;
  uint32_t tick_ms;
  uint8_t base_cells[PET_P4_GAME_GRID_MAX_CELLS];
  uint8_t solid_tone_mask;
  uint8_t entity_count;
  pet_p4_game_entity_t entities[PET_P4_GAME_MAX_ENTITIES];
  uint8_t rule_count;
  pet_p4_game_rule_t rules[PET_P4_GAME_MAX_RULES];
} pet_p4_bounded_game_config_t;

typedef struct {
  pet_p4_game_kind_t kind;
  uint8_t width;
  uint8_t height;
  uint8_t cells[PET_P4_GAME_GRID_MAX_CELLS];
  int32_t score;
  bool running;
  bool game_over;
  uint8_t entity_count;
  pet_p4_game_entity_t entities[PET_P4_GAME_MAX_ENTITIES];
  uint32_t revision;
} pet_p4_game_frame_t;

typedef struct {
  pet_p4_game_kind_t kind;
  uint32_t tick_ms;
  uint64_t last_tick_ms;
  uint32_t rng;
  int32_t score;
  bool running;
  bool game_over;
  uint8_t width;
  uint8_t height;
  uint8_t board[PET_P4_GAME_GRID_MAX_CELLS];

  uint8_t block_piece;
  uint8_t block_rotation;
  int8_t block_x;
  int8_t block_y;

  uint8_t snake_x[PET_P4_GAME_GRID_MAX_CELLS];
  uint8_t snake_y[PET_P4_GAME_GRID_MAX_CELLS];
  uint16_t snake_length;
  uint8_t snake_direction;
  uint8_t food_x;
  uint8_t food_y;

  int16_t flappy_y_q8;
  int16_t flappy_velocity_q8;
  int8_t flappy_pipe_x[2];
  uint8_t flappy_pipe_gap_top[2];
  bool flappy_pipe_scored[2];
  uint8_t flappy_scroll_phase;

  pet_p4_bounded_game_config_t bounded;
  pet_p4_game_entity_t bounded_entities[PET_P4_GAME_MAX_ENTITIES];

  pet_p4_game_frame_t frame;
} pet_p4_game_engine_t;

const char *pet_p4_game_kind_name(pet_p4_game_kind_t kind);
pet_p4_game_kind_t pet_p4_game_kind_from_name(const char *name);
bool pet_p4_game_configure(
  pet_p4_game_engine_t *game,
  pet_p4_game_kind_t kind,
  uint32_t tick_ms,
  uint32_t seed
);
bool pet_p4_game_configure_bounded(
  pet_p4_game_engine_t *game,
  const pet_p4_bounded_game_config_t *config,
  uint32_t seed
);
bool pet_p4_game_command(
  pet_p4_game_engine_t *game,
  pet_p4_game_command_t command,
  uint64_t now_ms
);
bool pet_p4_game_dispatch_action(
  pet_p4_game_engine_t *game,
  const char *action,
  uint64_t now_ms
);
bool pet_p4_game_process(pet_p4_game_engine_t *game, uint64_t now_ms);
void pet_p4_game_get_frame(
  const pet_p4_game_engine_t *game,
  pet_p4_game_frame_t *out
);

#ifdef __cplusplus
}
#endif
