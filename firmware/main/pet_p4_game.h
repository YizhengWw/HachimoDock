/*
 * [Input] Validated legacy presets or declarative v1/v2 scene configuration.
 * [Output] Heap-free fixed-grid game state with semantic shapes and sprite references.
 * [Pos] Shared bounded gameplay model for mini-app parsing and rendering.
 * [Sync] If this file changes, update `firmware/.folder.md` and `protocol.md`.
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
/* p4-logic-v1: runtime int vars mirrored into the scene so rules can read
 * them (rule when) and write them (op var). Bounded to the runtime var slots. */
#define PET_P4_GAME_MAX_VARS 8
#define PET_P4_GAME_VAR_LIMIT 1000000000
/* gomoku preset: standard 15x15 board inside the shared 16x16 grid. Frame
 * cells pack stone | cursor | last-move | winning-line bits; the AI reads
 * every empty cell's four 9-cell lines exactly once per reply. */
#define PET_P4_GAME_GOMOKU_SIZE 15
#define PET_P4_GAME_GOMOKU_CELLS (PET_P4_GAME_GOMOKU_SIZE * PET_P4_GAME_GOMOKU_SIZE)
#define PET_P4_GAME_GOMOKU_STONE_MASK 3U
#define PET_P4_GAME_GOMOKU_CURSOR 4U
#define PET_P4_GAME_GOMOKU_LAST 8U
#define PET_P4_GAME_GOMOKU_WIN 16U
#define PET_P4_GAME_GOMOKU_NONE 255U
#define PET_P4_GAME_GOMOKU_AI_MAX_READS (PET_P4_GAME_GOMOKU_CELLS * 4 * 9)

typedef enum {
  PET_P4_GAME_NONE = 0,
  PET_P4_GAME_BOUNDED,
  PET_P4_GAME_BLOCKS,
  PET_P4_GAME_SNAKE,
  PET_P4_GAME_FLAPPY,
  PET_P4_GAME_GOMOKU,
} pet_p4_game_kind_t;

typedef enum {
  PET_P4_GAME_COMMAND_START = 0,
  PET_P4_GAME_COMMAND_LEFT,
  PET_P4_GAME_COMMAND_RIGHT,
  PET_P4_GAME_COMMAND_ROTATE,
  PET_P4_GAME_COMMAND_DROP,
  PET_P4_GAME_COMMAND_FLAP,
  PET_P4_GAME_COMMAND_SOFT_DROP,
  PET_P4_GAME_COMMAND_WEST,
  PET_P4_GAME_COMMAND_EAST,
  PET_P4_GAME_COMMAND_NORTH,
  PET_P4_GAME_COMMAND_SOUTH,
  PET_P4_GAME_COMMAND_PLACE,
  PET_P4_GAME_COMMAND_UNDO,
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
  int32_t x_q8, y_q8; /* Motion/bodies; integer grid ABI stays intact. */
  bool body, body_static, rolling;
  uint8_t mass, restitution;
  int32_t body_vx_q8, body_vy_q8, angle_q8;
  int32_t body_rx, body_ry, body_rg; /* division remainders; no lost slow motion */
} pet_p4_game_entity_t;

typedef enum {
  PET_P4_GAME_TRIGGER_ACTION = 0,
  PET_P4_GAME_TRIGGER_TICK,
  PET_P4_GAME_TRIGGER_COLLISION,
  PET_P4_GAME_TRIGGER_EDGE,
  PET_P4_GAME_TRIGGER_BLOCKED,
  PET_P4_GAME_TRIGGER_LANDED,
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
  PET_P4_GAME_OP_CHARGE,
  PET_P4_GAME_OP_LAUNCH,
  PET_P4_GAME_OP_CANCEL,
  PET_P4_GAME_OP_VAR_SET,
  PET_P4_GAME_OP_VAR_INC,
  PET_P4_GAME_OP_SFX,
  PET_P4_GAME_OP_IMPULSE,
} pet_p4_game_op_kind_t;

typedef enum {
  PET_P4_GAME_CMP_LTE = 0,
  PET_P4_GAME_CMP_LT,
  PET_P4_GAME_CMP_GTE,
  PET_P4_GAME_CMP_GT,
  PET_P4_GAME_CMP_EQ,
  PET_P4_GAME_CMP_NE,
} pet_p4_game_cmp_t;

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
  /* p4-logic-v1 optional guard: rule fires only when vars[when_var] <op> value. */
  bool has_when;
  int8_t when_var;
  int8_t when_value_var; /* -1 for literal when_value */
  uint8_t when_op;       /* pet_p4_game_cmp_t */
  int32_t when_value;
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
  bool bodies;
  int8_t body_gravity;
  bool motion;
  int8_t motion_body;
  uint16_t platform_mask;
  uint16_t charge_ms;
  uint8_t gravity, launch_min, launch_max, lift;
  bool track;
  uint8_t camera_x, gap_min, gap_max, width_min, width_max, tone_mask;
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
  bool motion;
  bool bodies;
  uint8_t charge_percent;
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
  uint8_t snake_pending_direction;
  bool snake_turn_pending;
  uint8_t food_x;
  uint8_t food_y;

  int16_t flappy_y_q8;
  int16_t flappy_velocity_q8;
  uint8_t flappy_vertical_speed_percent;
  int8_t flappy_pipe_x[2];
  uint8_t flappy_pipe_gap_top[2];
  bool flappy_pipe_scored[2];
  uint8_t flappy_scroll_phase;

  /* gomoku: board[] holds 0 empty / 1 black (player) / 2 white (device). */
  uint8_t gomoku_cursor_x;
  uint8_t gomoku_cursor_y;
  uint8_t gomoku_rounds;      /* black stones on the board */
  uint8_t gomoku_stones;      /* all stones on the board (score while playing) */
  uint8_t gomoku_last_player; /* cell index or PET_P4_GAME_GOMOKU_NONE */
  uint8_t gomoku_last_ai;
  uint8_t gomoku_outcome;     /* 0 open, 1 black wins, 2 white wins, 3 draw */
  uint8_t gomoku_win_index;   /* first cell of the winning line */
  uint8_t gomoku_win_dir;
  uint8_t gomoku_win_len;
  bool gomoku_ai_pending;     /* white replies on the next tick */
  uint16_t gomoku_ai_reads;   /* line reads of the last reply, <= AI_MAX_READS */

  pet_p4_bounded_game_config_t bounded;
  pet_p4_game_entity_t bounded_entities[PET_P4_GAME_MAX_ENTITIES];
  int32_t motion_vx_q8, motion_vy_q8;
  uint32_t charge_elapsed_ms;
  bool charging, grounded;
  uint16_t track_visited;
  uint64_t camera_travel_q8;

  /* p4-logic-v1 mirror of runtime int vars; owner copies in before dispatch/
   * process and copies out afterwards. Never reset by restart. */
  uint8_t var_count;
  int32_t vars[PET_P4_GAME_MAX_VARS];
  /* p4-sfx-v1: cue bits queued by rules; the owner drains them to the speaker. */
  uint8_t sfx_pending;

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
bool pet_p4_game_set_vertical_speed(pet_p4_game_engine_t *game, unsigned percent);
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
/* Pointer coordinates are bounded grid centers; no collision/tick replay. */
bool pet_p4_game_pointer(pet_p4_game_engine_t *game,unsigned entity,int x,int y,unsigned axes);
bool pet_p4_game_release(pet_p4_game_engine_t *game,bool cancel,uint64_t now_ms);
bool pet_p4_game_process(pet_p4_game_engine_t *game, uint64_t now_ms);
/* p4-logic-v1 shared comparison used by scene rule guards and the bounded runtime. */
bool pet_p4_game_compare(int32_t left, unsigned op, int32_t right);
void pet_p4_game_get_frame(
  const pet_p4_game_engine_t *game,
  pet_p4_game_frame_t *out
);

#ifdef __cplusplus
}
#endif
