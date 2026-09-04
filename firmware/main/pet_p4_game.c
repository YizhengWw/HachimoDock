/*
 * [Input] A validated preset or declarative scene, bounded tick rate, and semantic controls.
 * [Output] A deterministic fixed-grid frame retaining semantic shapes and sprite references.
 * [Pos] Heap-free, script-free gameplay engine used by the declarative mini-app runtime.
 */

#include "pet_p4_game.h"

#include <stddef.h>
#include <string.h>

#define BLOCKS_WIDTH 10
#define BLOCKS_HEIGHT 16
#define SNAKE_WIDTH 16
#define SNAKE_HEIGHT 10
#define FLAPPY_WIDTH 16
#define FLAPPY_HEIGHT 10
#define FLAPPY_BIRD_X 3
#define FLAPPY_PIPE_WIDTH 2
#define FLAPPY_PIPE_GAP 4
#define FLAPPY_PIPE_SPACING 9
#define FLAPPY_GRAVITY_Q8 80
#define FLAPPY_FLAP_VELOCITY_Q8 -256
#define FLAPPY_MAX_FALL_VELOCITY_Q8 512
#define GAME_TICK_MIN_MS 100U
#define GAME_TICK_MAX_MS 2000U
#define BOUNDED_EVENT_MAX 32
#define BOUNDED_AXIS_X 1U
#define BOUNDED_AXIS_Y 2U

typedef struct {
  pet_p4_game_trigger_t trigger;
  int8_t entity_index;
  pet_p4_game_edge_t edge;
} bounded_event_t;

static const uint16_t BLOCK_MASKS[7][4] = {
  {0x00f0, 0x2222, 0x00f0, 0x2222}, /* I */
  {0x0066, 0x0066, 0x0066, 0x0066}, /* O */
  {0x0072, 0x0262, 0x0270, 0x0232}, /* T */
  {0x0036, 0x0462, 0x0036, 0x0462}, /* S */
  {0x0063, 0x0264, 0x0063, 0x0264}, /* Z */
  {0x0071, 0x0226, 0x0470, 0x0322}, /* J */
  {0x0074, 0x0622, 0x0170, 0x0223}, /* L */
};

static uint32_t game_random(pet_p4_game_engine_t *game) {
  game->rng = game->rng * 1664525U + 1013904223U;
  return game->rng;
}

static int game_cell_index(const pet_p4_game_engine_t *game, int x, int y) {
  if (!game || x < 0 || y < 0 || x >= game->width || y >= game->height) return -1;
  return y * game->width + x;
}

static int bounded_clamp(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static int32_t bounded_score(int64_t value) {
  if (value < -1000000000LL) return -1000000000;
  if (value > 1000000000LL) return 1000000000;
  return (int32_t) value;
}

static void bounded_queue_event(
  bounded_event_t *events,
  uint8_t *event_count,
  pet_p4_game_trigger_t trigger,
  int entity_index,
  pet_p4_game_edge_t edge
) {
  if (!events || !event_count || *event_count >= BOUNDED_EVENT_MAX) return;
  events[*event_count].trigger = trigger;
  events[*event_count].entity_index = (int8_t) entity_index;
  events[*event_count].edge = edge;
  *event_count += 1;
}

static bool bounded_hits_solid(
  const pet_p4_game_engine_t *game,
  const pet_p4_game_entity_t *entity,
  int x,
  int y
) {
  if (!game || !entity || game->bounded.solid_tone_mask == 0) return false;
  for (int py = 0; py < entity->height; py += 1) {
    for (int px = 0; px < entity->width; px += 1) {
      int index = game_cell_index(game, x + px, y + py);
      if (index < 0) continue;
      uint8_t tone = game->bounded.base_cells[index];
      if (tone > 0 && (game->bounded.solid_tone_mask & (1U << tone)) != 0) {
        return true;
      }
    }
  }
  return false;
}

static bool bounded_entities_overlap(
  const pet_p4_game_entity_t *left,
  const pet_p4_game_entity_t *right
) {
  if (!left || !right || !left->active || !right->active
      || !left->collidable || !right->collidable) {
    return false;
  }
  return left->x < right->x + right->width
    && left->x + left->width > right->x
    && left->y < right->y + right->height
    && left->y + left->height > right->y;
}

static void bounded_reset(
  pet_p4_game_engine_t *game,
  uint64_t now_ms,
  bool running
) {
  if (!game) return;
  memcpy(
    game->bounded_entities,
    game->bounded.entities,
    sizeof(game->bounded_entities)
  );
  game->score = 0;
  game->running = running;
  game->game_over = false;
  game->last_tick_ms = now_ms;
}

static bool bounded_move_entity(
  pet_p4_game_engine_t *game,
  int entity_index,
  int dx,
  int dy,
  bounded_event_t *events,
  uint8_t *event_count
) {
  pet_p4_game_entity_t *entity;
  int old_x;
  int old_y;
  int next_x;
  int next_y;
  int max_x;
  int max_y;
  bool changed = false;
  bool crossed_x = false;
  bool crossed_y = false;
  pet_p4_game_edge_t edge_x = PET_P4_GAME_EDGE_NONE;
  pet_p4_game_edge_t edge_y = PET_P4_GAME_EDGE_NONE;
  if (!game || entity_index < 0 || entity_index >= game->bounded.entity_count) return false;
  entity = &game->bounded_entities[entity_index];
  if (!entity->active || (dx == 0 && dy == 0)) return false;
  old_x = entity->x;
  old_y = entity->y;
  next_x = old_x + dx;
  next_y = old_y + dy;
  max_x = game->width - entity->width;
  max_y = game->height - entity->height;

  if (next_x < 0) {
    crossed_x = true;
    edge_x = PET_P4_GAME_EDGE_LEFT;
  } else if (next_x > max_x) {
    crossed_x = true;
    edge_x = PET_P4_GAME_EDGE_RIGHT;
  }
  if (next_y < 0) {
    crossed_y = true;
    edge_y = PET_P4_GAME_EDGE_TOP;
  } else if (next_y > max_y) {
    crossed_y = true;
    edge_y = PET_P4_GAME_EDGE_BOTTOM;
  }

  if (crossed_x || crossed_y) {
    if (entity->bounds == PET_P4_GAME_BOUNDS_WRAP) {
      if (next_x < 0) next_x = max_x;
      else if (next_x > max_x) next_x = 0;
      if (next_y < 0) next_y = max_y;
      else if (next_y > max_y) next_y = 0;
    } else {
      next_x = bounded_clamp(next_x, 0, max_x);
      next_y = bounded_clamp(next_y, 0, max_y);
      if (entity->bounds == PET_P4_GAME_BOUNDS_BOUNCE) {
        if (crossed_x) entity->vx = (int8_t) -entity->vx;
        if (crossed_y) entity->vy = (int8_t) -entity->vy;
      } else if (entity->bounds == PET_P4_GAME_BOUNDS_HIDE) {
        entity->active = false;
      } else if (entity->bounds == PET_P4_GAME_BOUNDS_STOP) {
        game->running = false;
        game->game_over = true;
      }
    }
    if (crossed_x) {
      bounded_queue_event(
        events,
        event_count,
        PET_P4_GAME_TRIGGER_EDGE,
        entity_index,
        edge_x
      );
    }
    if (crossed_y) {
      bounded_queue_event(
        events,
        event_count,
        PET_P4_GAME_TRIGGER_EDGE,
        entity_index,
        edge_y
      );
    }
  }

  if (entity->active && bounded_hits_solid(game, entity, next_x, next_y)) {
    next_x = old_x;
    next_y = old_y;
    if (entity->bounds == PET_P4_GAME_BOUNDS_BOUNCE) {
      if (dx != 0) entity->vx = (int8_t) -entity->vx;
      if (dy != 0) entity->vy = (int8_t) -entity->vy;
    }
    bounded_queue_event(
      events,
      event_count,
      PET_P4_GAME_TRIGGER_BLOCKED,
      entity_index,
      PET_P4_GAME_EDGE_NONE
    );
  }
  if (next_x != old_x || next_y != old_y) {
    entity->x = (int8_t) next_x;
    entity->y = (int8_t) next_y;
    changed = true;
  }
  return changed || crossed_x || crossed_y;
}

static int bounded_random_range(
  pet_p4_game_engine_t *game,
  int minimum,
  int maximum
) {
  if (maximum <= minimum) return minimum;
  return minimum + (int) (game_random(game) % (uint32_t) (maximum - minimum + 1));
}

static bool bounded_place_entity(
  pet_p4_game_engine_t *game,
  const pet_p4_game_op_t *op
) {
  pet_p4_game_entity_t *entity;
  const pet_p4_game_entity_t *source = NULL;
  int next_x;
  int next_y;
  int max_x;
  int max_y;
  if (!game || !op || op->entity_index < 0
      || op->entity_index >= game->bounded.entity_count) {
    return false;
  }
  entity = &game->bounded_entities[op->entity_index];
  if (op->source_index >= 0 && op->source_index < game->bounded.entity_count) {
    source = &game->bounded_entities[op->source_index];
  }
  next_x = source ? source->x + op->dx : entity->x;
  next_y = source ? source->y + op->dy : entity->y;
  if (op->has_x) {
    next_x = op->random_x
      ? bounded_random_range(game, op->x_min, op->x_max)
      : op->x_min;
  }
  if (op->has_y) {
    next_y = op->random_y
      ? bounded_random_range(game, op->y_min, op->y_max)
      : op->y_min;
  }
  max_x = game->width - entity->width;
  max_y = game->height - entity->height;
  next_x = bounded_clamp(next_x, 0, max_x);
  next_y = bounded_clamp(next_y, 0, max_y);
  bool changed = !entity->active || entity->x != next_x || entity->y != next_y;
  entity->x = (int8_t) next_x;
  entity->y = (int8_t) next_y;
  entity->active = true;
  return changed;
}

static bool bounded_apply_op(
  pet_p4_game_engine_t *game,
  const pet_p4_game_op_t *op,
  uint64_t now_ms,
  bounded_event_t *events,
  uint8_t *event_count
) {
  pet_p4_game_entity_t *entity = NULL;
  if (!game || !op) return false;
  if (op->entity_index >= 0 && op->entity_index < game->bounded.entity_count) {
    entity = &game->bounded_entities[op->entity_index];
  }
  if (op->kind == PET_P4_GAME_OP_MOVE) {
    return bounded_move_entity(
      game,
      op->entity_index,
      op->dx,
      op->dy,
      events,
      event_count
    );
  }
  if (op->kind == PET_P4_GAME_OP_VELOCITY && entity) {
    bool changed = entity->vx != op->vx || entity->vy != op->vy;
    entity->vx = op->vx;
    entity->vy = op->vy;
    return changed;
  }
  if (op->kind == PET_P4_GAME_OP_ACCELERATE && entity) {
    int next_vx = bounded_clamp(entity->vx + op->vx, -4, 4);
    int next_vy = bounded_clamp(entity->vy + op->vy, -4, 4);
    bool changed = entity->vx != next_vx || entity->vy != next_vy;
    entity->vx = (int8_t) next_vx;
    entity->vy = (int8_t) next_vy;
    return changed;
  }
  if (op->kind == PET_P4_GAME_OP_PLACE) {
    return bounded_place_entity(game, op);
  }
  if (op->kind == PET_P4_GAME_OP_SHOW && entity) {
    bool changed = !entity->active;
    entity->active = true;
    return changed;
  }
  if (op->kind == PET_P4_GAME_OP_HIDE && entity) {
    bool changed = entity->active;
    entity->active = false;
    return changed;
  }
  if (op->kind == PET_P4_GAME_OP_SCORE_ADD) {
    int32_t next = bounded_score((int64_t) game->score + op->value);
    bool changed = next != game->score;
    game->score = next;
    return changed;
  }
  if (op->kind == PET_P4_GAME_OP_SCORE_SET) {
    int32_t next = bounded_score(op->value);
    bool changed = next != game->score;
    game->score = next;
    return changed;
  }
  if (op->kind == PET_P4_GAME_OP_RUN) {
    bool changed = !game->running || game->game_over;
    game->running = true;
    game->game_over = false;
    game->last_tick_ms = now_ms;
    return changed;
  }
  if (op->kind == PET_P4_GAME_OP_STOP) {
    bool changed = game->running || !game->game_over;
    game->running = false;
    game->game_over = true;
    return changed;
  }
  if (op->kind == PET_P4_GAME_OP_RESTART) {
    bounded_reset(game, now_ms, true);
    return true;
  }
  if (op->kind == PET_P4_GAME_OP_BOUNCE && entity) {
    int8_t old_vx = entity->vx;
    int8_t old_vy = entity->vy;
    if ((op->axis_mask & BOUNDED_AXIS_X) != 0) entity->vx = (int8_t) -entity->vx;
    if ((op->axis_mask & BOUNDED_AXIS_Y) != 0) entity->vy = (int8_t) -entity->vy;
    return old_vx != entity->vx || old_vy != entity->vy;
  }
  if (op->kind == PET_P4_GAME_OP_TONE && entity) {
    bool changed = entity->tone != op->tone;
    entity->tone = op->tone;
    return changed;
  }
  return false;
}

static bool bounded_rule_matches(
  const pet_p4_game_rule_t *rule,
  pet_p4_game_trigger_t trigger,
  const char *action,
  int entity_index,
  int with_index,
  pet_p4_game_edge_t edge
) {
  if (!rule || rule->trigger != trigger) return false;
  if (trigger == PET_P4_GAME_TRIGGER_ACTION) {
    return action && strcmp(rule->action, action) == 0;
  }
  if (trigger == PET_P4_GAME_TRIGGER_COLLISION) {
    return (rule->entity_index == entity_index && rule->with_index == with_index)
      || (rule->entity_index == with_index && rule->with_index == entity_index);
  }
  if (trigger == PET_P4_GAME_TRIGGER_EDGE || trigger == PET_P4_GAME_TRIGGER_BLOCKED) {
    return rule->entity_index == entity_index
      && (rule->edge == PET_P4_GAME_EDGE_NONE || rule->edge == edge);
  }
  return true;
}

static bool bounded_execute_rules(
  pet_p4_game_engine_t *game,
  pet_p4_game_trigger_t trigger,
  const char *action,
  int entity_index,
  int with_index,
  pet_p4_game_edge_t edge,
  uint64_t now_ms,
  bounded_event_t *events,
  uint8_t *event_count
) {
  bool changed = false;
  if (!game) return false;
  for (int index = 0; index < game->bounded.rule_count; index += 1) {
    const pet_p4_game_rule_t *rule = &game->bounded.rules[index];
    if (!bounded_rule_matches(
          rule,
          trigger,
          action,
          entity_index,
          with_index,
          edge
        )) {
      continue;
    }
    for (int op_index = 0; op_index < rule->op_count; op_index += 1) {
      changed |= bounded_apply_op(
        game,
        &rule->ops[op_index],
        now_ms,
        events,
        event_count
      );
    }
  }
  return changed;
}

static bool bounded_process_trigger(
  pet_p4_game_engine_t *game,
  pet_p4_game_trigger_t trigger,
  const char *action,
  uint64_t now_ms
) {
  bounded_event_t events[BOUNDED_EVENT_MAX];
  uint8_t event_count = 0;
  bool changed = bounded_execute_rules(
    game,
    trigger,
    action,
    -1,
    -1,
    PET_P4_GAME_EDGE_NONE,
    now_ms,
    events,
    &event_count
  );
  if (trigger == PET_P4_GAME_TRIGGER_TICK && game->running && !game->game_over) {
    for (int index = 0; index < game->bounded.entity_count; index += 1) {
      pet_p4_game_entity_t *entity = &game->bounded_entities[index];
      if (!entity->active || (entity->vx == 0 && entity->vy == 0)) continue;
      changed |= bounded_move_entity(
        game,
        index,
        entity->vx,
        entity->vy,
        events,
        &event_count
      );
    }
  }
  for (int index = 0; index < event_count; index += 1) {
    bounded_event_t *event = &events[index];
    changed |= bounded_execute_rules(
      game,
      event->trigger,
      NULL,
      event->entity_index,
      -1,
      event->edge,
      now_ms,
      NULL,
      NULL
    );
  }
  for (int left = 0; left < game->bounded.entity_count; left += 1) {
    for (int right = left + 1; right < game->bounded.entity_count; right += 1) {
      if (!bounded_entities_overlap(
            &game->bounded_entities[left],
            &game->bounded_entities[right]
          )) {
        continue;
      }
      changed |= bounded_execute_rules(
        game,
        PET_P4_GAME_TRIGGER_COLLISION,
        NULL,
        left,
        right,
        PET_P4_GAME_EDGE_NONE,
        now_ms,
        NULL,
        NULL
      );
    }
  }
  return changed;
}

static bool block_mask_cell(uint8_t piece, uint8_t rotation, int x, int y) {
  if (piece >= 7 || rotation >= 4 || x < 0 || x >= 4 || y < 0 || y >= 4) return false;
  return (BLOCK_MASKS[piece][rotation] & (uint16_t) (1U << (y * 4 + x))) != 0;
}

static bool blocks_can_place(
  const pet_p4_game_engine_t *game,
  uint8_t piece,
  uint8_t rotation,
  int origin_x,
  int origin_y
) {
  for (int py = 0; py < 4; py += 1) {
    for (int px = 0; px < 4; px += 1) {
      int x;
      int y;
      int index;
      if (!block_mask_cell(piece, rotation, px, py)) continue;
      x = origin_x + px;
      y = origin_y + py;
      index = game_cell_index(game, x, y);
      if (index < 0 || game->board[index] != 0) return false;
    }
  }
  return true;
}

static void game_refresh_frame(pet_p4_game_engine_t *game) {
  pet_p4_game_frame_t *frame;
  if (!game) return;
  frame = &game->frame;
  memset(frame->cells, 0, sizeof(frame->cells));
  frame->kind = game->kind;
  frame->width = game->width;
  frame->height = game->height;
  frame->score = game->score;
  frame->running = game->running;
  frame->game_over = game->game_over;
  frame->entity_count = 0;
  if (game->kind == PET_P4_GAME_BOUNDED) {
    memcpy(
      frame->cells,
      game->bounded.base_cells,
      (size_t) game->width * game->height
    );
    for (int entity_index = 0;
         entity_index < game->bounded.entity_count;
         entity_index += 1) {
      const pet_p4_game_entity_t *entity = &game->bounded_entities[entity_index];
      if (!entity->active) continue;
      frame->entities[frame->entity_count++] = *entity;
    }
  } else {
    memcpy(frame->cells, game->board, (size_t) game->width * game->height);
  }

  if (game->kind == PET_P4_GAME_BLOCKS && !game->game_over) {
    uint8_t color = (uint8_t) ((game->block_piece % 4U) + 1U);
    for (int py = 0; py < 4; py += 1) {
      for (int px = 0; px < 4; px += 1) {
        int index;
        if (!block_mask_cell(game->block_piece, game->block_rotation, px, py)) continue;
        index = game_cell_index(game, game->block_x + px, game->block_y + py);
        if (index >= 0) frame->cells[index] = color;
      }
    }
  } else if (game->kind == PET_P4_GAME_SNAKE) {
    memset(frame->cells, 0, sizeof(frame->cells));
    for (uint16_t i = 0; i < game->snake_length; i += 1) {
      int index = game_cell_index(game, game->snake_x[i], game->snake_y[i]);
      if (index >= 0) frame->cells[index] = i == 0 ? 3 : 1;
    }
    int food_index = game_cell_index(game, game->food_x, game->food_y);
    if (food_index >= 0) frame->cells[food_index] = 2;
  } else if (game->kind == PET_P4_GAME_FLAPPY) {
    memset(frame->cells, 0, sizeof(frame->cells));
    for (size_t pipe = 0; pipe < 2; pipe += 1) {
      int pipe_x = game->flappy_pipe_x[pipe];
      int gap_top = game->flappy_pipe_gap_top[pipe];
      for (int px = 0; px < FLAPPY_PIPE_WIDTH; px += 1) {
        int x = pipe_x + px;
        if (x < 0 || x >= game->width) continue;
        for (int y = 0; y < game->height; y += 1) {
          if (y >= gap_top && y < gap_top + FLAPPY_PIPE_GAP) continue;
          int index = game_cell_index(game, x, y);
          if (index >= 0) {
            bool cap = y == gap_top - 1 || y == gap_top + FLAPPY_PIPE_GAP;
            frame->cells[index] = cap ? 2 : 1;
          }
        }
      }
    }
    int bird_y = game->flappy_y_q8 >> 8;
    int bird_index = game_cell_index(game, FLAPPY_BIRD_X, bird_y);
    int wing_index = game_cell_index(game, FLAPPY_BIRD_X - 1, bird_y);
    if (wing_index >= 0) frame->cells[wing_index] = 4;
    if (bird_index >= 0) frame->cells[bird_index] = 3;
  }
  frame->revision += 1;
}

static bool blocks_spawn(pet_p4_game_engine_t *game) {
  game->block_piece = (uint8_t) (game_random(game) % 7U);
  game->block_rotation = 0;
  game->block_x = 3;
  game->block_y = 0;
  if (blocks_can_place(
        game,
        game->block_piece,
        game->block_rotation,
        game->block_x,
        game->block_y
      )) {
    return true;
  }
  game->running = false;
  game->game_over = true;
  return false;
}

static int blocks_clear_lines(pet_p4_game_engine_t *game) {
  int cleared = 0;
  for (int y = game->height - 1; y >= 0; y -= 1) {
    bool full = true;
    for (int x = 0; x < game->width; x += 1) {
      if (game->board[y * game->width + x] == 0) {
        full = false;
        break;
      }
    }
    if (!full) continue;
    for (int pull = y; pull > 0; pull -= 1) {
      memcpy(
        &game->board[pull * game->width],
        &game->board[(pull - 1) * game->width],
        game->width
      );
    }
    memset(game->board, 0, game->width);
    cleared += 1;
    y += 1;
  }
  return cleared;
}

static bool blocks_lock_piece(pet_p4_game_engine_t *game) {
  uint8_t color = (uint8_t) ((game->block_piece % 4U) + 1U);
  for (int py = 0; py < 4; py += 1) {
    for (int px = 0; px < 4; px += 1) {
      int index;
      if (!block_mask_cell(game->block_piece, game->block_rotation, px, py)) continue;
      index = game_cell_index(game, game->block_x + px, game->block_y + py);
      if (index >= 0) game->board[index] = color;
    }
  }
  int cleared = blocks_clear_lines(game);
  game->score += 10 + cleared * cleared * 100;
  return blocks_spawn(game);
}

static bool blocks_step_down(pet_p4_game_engine_t *game) {
  if (blocks_can_place(
        game,
        game->block_piece,
        game->block_rotation,
        game->block_x,
        game->block_y + 1
      )) {
    game->block_y += 1;
  } else {
    blocks_lock_piece(game);
  }
  return true;
}

static void blocks_reset(pet_p4_game_engine_t *game, uint64_t now_ms, bool running) {
  memset(game->board, 0, sizeof(game->board));
  game->score = 0;
  game->running = running;
  game->game_over = false;
  game->last_tick_ms = now_ms;
  blocks_spawn(game);
  game->running = running && !game->game_over;
}

static bool snake_contains(
  const pet_p4_game_engine_t *game,
  uint8_t x,
  uint8_t y,
  uint16_t count
) {
  for (uint16_t i = 0; i < count; i += 1) {
    if (game->snake_x[i] == x && game->snake_y[i] == y) return true;
  }
  return false;
}

static void snake_place_food(pet_p4_game_engine_t *game) {
  uint16_t capacity = (uint16_t) game->width * game->height;
  uint16_t start = (uint16_t) (game_random(game) % capacity);
  for (uint16_t offset = 0; offset < capacity; offset += 1) {
    uint16_t index = (uint16_t) ((start + offset) % capacity);
    uint8_t x = (uint8_t) (index % game->width);
    uint8_t y = (uint8_t) (index / game->width);
    if (!snake_contains(game, x, y, game->snake_length)) {
      game->food_x = x;
      game->food_y = y;
      return;
    }
  }
  game->running = false;
  game->game_over = true;
}

static void snake_reset(pet_p4_game_engine_t *game, uint64_t now_ms, bool running) {
  memset(game->board, 0, sizeof(game->board));
  game->score = 0;
  game->running = running;
  game->game_over = false;
  game->last_tick_ms = now_ms;
  game->snake_length = 4;
  game->snake_direction = 0;
  for (uint16_t i = 0; i < game->snake_length; i += 1) {
    game->snake_x[i] = (uint8_t) (game->width / 2 - i);
    game->snake_y[i] = (uint8_t) (game->height / 2);
  }
  game->food_x = (uint8_t) (game->width - 3);
  game->food_y = (uint8_t) (game->height / 2);
}

static bool snake_step(pet_p4_game_engine_t *game) {
  static const int8_t dx[] = {1, 0, -1, 0};
  static const int8_t dy[] = {0, 1, 0, -1};
  int next_x = (int) game->snake_x[0] + dx[game->snake_direction];
  int next_y = (int) game->snake_y[0] + dy[game->snake_direction];
  bool growing;
  uint16_t collision_count;
  if (next_x < 0 || next_y < 0 || next_x >= game->width || next_y >= game->height) {
    game->running = false;
    game->game_over = true;
    return true;
  }
  growing = next_x == game->food_x && next_y == game->food_y;
  collision_count = growing ? game->snake_length
                            : (game->snake_length > 0 ? game->snake_length - 1 : 0);
  if (snake_contains(game, (uint8_t) next_x, (uint8_t) next_y, collision_count)) {
    game->running = false;
    game->game_over = true;
    return true;
  }
  uint16_t next_length = game->snake_length;
  if (growing && next_length < PET_P4_GAME_GRID_MAX_CELLS) next_length += 1;
  for (uint16_t i = next_length - 1; i > 0; i -= 1) {
    game->snake_x[i] = game->snake_x[i - 1];
    game->snake_y[i] = game->snake_y[i - 1];
  }
  game->snake_x[0] = (uint8_t) next_x;
  game->snake_y[0] = (uint8_t) next_y;
  game->snake_length = next_length;
  if (growing) {
    game->score += 1;
    snake_place_food(game);
  }
  return true;
}

static void flappy_place_pipe(pet_p4_game_engine_t *game, size_t pipe, int x) {
  int gap_positions = FLAPPY_HEIGHT - FLAPPY_PIPE_GAP - 1;
  game->flappy_pipe_x[pipe] = (int8_t) x;
  game->flappy_pipe_gap_top[pipe] =
    (uint8_t) (1 + (game_random(game) % (uint32_t) gap_positions));
  game->flappy_pipe_scored[pipe] = false;
}

static void flappy_reset(
  pet_p4_game_engine_t *game,
  uint64_t now_ms,
  bool running,
  bool initial_flap
) {
  memset(game->board, 0, sizeof(game->board));
  game->score = 0;
  game->running = running;
  game->game_over = false;
  game->last_tick_ms = now_ms;
  game->flappy_y_q8 = (int16_t) ((FLAPPY_HEIGHT / 2) << 8);
  game->flappy_velocity_q8 = initial_flap ? FLAPPY_FLAP_VELOCITY_Q8 : 0;
  game->flappy_scroll_phase = 0;
  flappy_place_pipe(game, 0, FLAPPY_WIDTH - 5);
  flappy_place_pipe(game, 1, FLAPPY_WIDTH - 5 + FLAPPY_PIPE_SPACING);
}

static bool flappy_collides(const pet_p4_game_engine_t *game) {
  int bird_y = game->flappy_y_q8 >> 8;
  if (bird_y < 0 || bird_y >= game->height) return true;
  for (size_t pipe = 0; pipe < 2; pipe += 1) {
    int pipe_x = game->flappy_pipe_x[pipe];
    if (FLAPPY_BIRD_X < pipe_x || FLAPPY_BIRD_X >= pipe_x + FLAPPY_PIPE_WIDTH) continue;
    int gap_top = game->flappy_pipe_gap_top[pipe];
    if (bird_y < gap_top || bird_y >= gap_top + FLAPPY_PIPE_GAP) return true;
  }
  return false;
}

static bool flappy_step(pet_p4_game_engine_t *game) {
  game->flappy_y_q8 = (int16_t) (game->flappy_y_q8 + game->flappy_velocity_q8);
  game->flappy_velocity_q8 = (int16_t) (
    game->flappy_velocity_q8 + FLAPPY_GRAVITY_Q8
  );
  if (game->flappy_velocity_q8 > FLAPPY_MAX_FALL_VELOCITY_Q8) {
    game->flappy_velocity_q8 = FLAPPY_MAX_FALL_VELOCITY_Q8;
  }

  game->flappy_scroll_phase += 1;
  if (game->flappy_scroll_phase >= 2) {
    game->flappy_scroll_phase = 0;
    for (size_t pipe = 0; pipe < 2; pipe += 1) {
      game->flappy_pipe_x[pipe] -= 1;
      if (!game->flappy_pipe_scored[pipe]
          && game->flappy_pipe_x[pipe] + FLAPPY_PIPE_WIDTH <= FLAPPY_BIRD_X) {
        game->flappy_pipe_scored[pipe] = true;
        game->score += 1;
      }
      if (game->flappy_pipe_x[pipe] + FLAPPY_PIPE_WIDTH < 0) {
        size_t other = pipe == 0 ? 1 : 0;
        flappy_place_pipe(
          game,
          pipe,
          game->flappy_pipe_x[other] + FLAPPY_PIPE_SPACING
        );
      }
    }
  }

  if (flappy_collides(game)) {
    game->running = false;
    game->game_over = true;
  }
  return true;
}

const char *pet_p4_game_kind_name(pet_p4_game_kind_t kind) {
  if (kind == PET_P4_GAME_BOUNDED) return "p4-bounded-game-v1";
  if (kind == PET_P4_GAME_BLOCKS) return "blocks";
  if (kind == PET_P4_GAME_SNAKE) return "snake";
  if (kind == PET_P4_GAME_FLAPPY) return "flappy";
  return "";
}

pet_p4_game_kind_t pet_p4_game_kind_from_name(const char *name) {
  if (name && strcmp(name, "p4-bounded-game-v1") == 0) return PET_P4_GAME_BOUNDED;
  if (name && strcmp(name, "blocks") == 0) return PET_P4_GAME_BLOCKS;
  if (name && strcmp(name, "snake") == 0) return PET_P4_GAME_SNAKE;
  if (name && strcmp(name, "flappy") == 0) return PET_P4_GAME_FLAPPY;
  return PET_P4_GAME_NONE;
}

bool pet_p4_game_configure(
  pet_p4_game_engine_t *game,
  pet_p4_game_kind_t kind,
  uint32_t tick_ms,
  uint32_t seed
) {
  if (!game || kind == PET_P4_GAME_NONE || kind == PET_P4_GAME_BOUNDED
      || tick_ms < GAME_TICK_MIN_MS || tick_ms > GAME_TICK_MAX_MS) {
    return false;
  }
  memset(game, 0, sizeof(*game));
  game->kind = kind;
  game->tick_ms = tick_ms;
  game->rng = seed ? seed : 0x70657434U;
  if (kind == PET_P4_GAME_BLOCKS) {
    game->width = BLOCKS_WIDTH;
    game->height = BLOCKS_HEIGHT;
    blocks_reset(game, 0, false);
  } else if (kind == PET_P4_GAME_SNAKE) {
    game->width = SNAKE_WIDTH;
    game->height = SNAKE_HEIGHT;
    snake_reset(game, 0, false);
  } else {
    game->width = FLAPPY_WIDTH;
    game->height = FLAPPY_HEIGHT;
    flappy_reset(game, 0, false, false);
  }
  game_refresh_frame(game);
  return true;
}

bool pet_p4_game_configure_bounded(
  pet_p4_game_engine_t *game,
  const pet_p4_bounded_game_config_t *config,
  uint32_t seed
) {
  if (!game || !config
      || config->width < 4 || config->width > PET_P4_GAME_GRID_MAX_WIDTH
      || config->height < 4 || config->height > PET_P4_GAME_GRID_MAX_HEIGHT
      || config->tick_ms < GAME_TICK_MIN_MS || config->tick_ms > GAME_TICK_MAX_MS
      || config->entity_count == 0 || config->entity_count > PET_P4_GAME_MAX_ENTITIES
      || config->rule_count == 0 || config->rule_count > PET_P4_GAME_MAX_RULES) {
    return false;
  }
  for (int index = 0; index < config->width * config->height; index += 1) {
    if (config->base_cells[index] > 4) return false;
  }
  for (int index = 0; index < config->entity_count; index += 1) {
    const pet_p4_game_entity_t *entity = &config->entities[index];
    if (!entity->id[0] || entity->width == 0 || entity->height == 0
        || entity->width > 8 || entity->height > 8
        || entity->tone == 0 || entity->tone > 4
        || entity->x < 0 || entity->y < 0
        || entity->x + entity->width > config->width
        || entity->y + entity->height > config->height
        || entity->vx < -4 || entity->vx > 4
        || entity->vy < -4 || entity->vy > 4
        || entity->bounds < PET_P4_GAME_BOUNDS_CLAMP
        || entity->bounds > PET_P4_GAME_BOUNDS_STOP
        || entity->shape < PET_P4_GAME_SHAPE_RECT
        || entity->shape > PET_P4_GAME_SHAPE_CHARACTER
        || entity->sprite_index < -1) {
      return false;
    }
  }
  for (int index = 0; index < config->rule_count; index += 1) {
    const pet_p4_game_rule_t *rule = &config->rules[index];
    if (rule->op_count == 0 || rule->op_count > PET_P4_GAME_MAX_OPS_PER_RULE
        || rule->trigger < PET_P4_GAME_TRIGGER_ACTION
        || rule->trigger > PET_P4_GAME_TRIGGER_BLOCKED
        || (rule->trigger == PET_P4_GAME_TRIGGER_ACTION && !rule->action[0])) {
      return false;
    }
  }
  memset(game, 0, sizeof(*game));
  game->kind = PET_P4_GAME_BOUNDED;
  game->tick_ms = config->tick_ms;
  game->rng = seed ? seed : 0x70657434U;
  game->width = config->width;
  game->height = config->height;
  game->bounded = *config;
  bounded_reset(game, 0, false);
  game_refresh_frame(game);
  return true;
}

bool pet_p4_game_command(
  pet_p4_game_engine_t *game,
  pet_p4_game_command_t command,
  uint64_t now_ms
) {
  bool changed = false;
  if (!game || game->kind == PET_P4_GAME_NONE
      || game->kind == PET_P4_GAME_BOUNDED) return false;
  if (game->kind == PET_P4_GAME_FLAPPY && command == PET_P4_GAME_COMMAND_FLAP) {
    if (!game->running || game->game_over) {
      flappy_reset(game, now_ms, true, true);
    } else {
      game->flappy_velocity_q8 = FLAPPY_FLAP_VELOCITY_Q8;
    }
    changed = true;
  } else if (command == PET_P4_GAME_COMMAND_START) {
    if (game->kind == PET_P4_GAME_BLOCKS) blocks_reset(game, now_ms, true);
    else if (game->kind == PET_P4_GAME_SNAKE) snake_reset(game, now_ms, true);
    else flappy_reset(game, now_ms, true, true);
    changed = true;
  } else if (!game->running || game->game_over) {
    return false;
  } else if (game->kind == PET_P4_GAME_BLOCKS) {
    if (command == PET_P4_GAME_COMMAND_LEFT
        && blocks_can_place(game, game->block_piece, game->block_rotation,
                            game->block_x - 1, game->block_y)) {
      game->block_x -= 1;
      changed = true;
    } else if (command == PET_P4_GAME_COMMAND_RIGHT
        && blocks_can_place(game, game->block_piece, game->block_rotation,
                            game->block_x + 1, game->block_y)) {
      game->block_x += 1;
      changed = true;
    } else if (command == PET_P4_GAME_COMMAND_ROTATE) {
      uint8_t next_rotation = (uint8_t) ((game->block_rotation + 1U) % 4U);
      if (blocks_can_place(game, game->block_piece, next_rotation,
                           game->block_x, game->block_y)) {
        game->block_rotation = next_rotation;
        changed = true;
      }
    } else if (command == PET_P4_GAME_COMMAND_DROP) {
      int distance = 0;
      while (blocks_can_place(game, game->block_piece, game->block_rotation,
                              game->block_x, game->block_y + 1)) {
        game->block_y += 1;
        distance += 1;
      }
      game->score += distance;
      blocks_lock_piece(game);
      changed = true;
    }
  } else if (game->kind == PET_P4_GAME_SNAKE) {
    if (command == PET_P4_GAME_COMMAND_LEFT) {
      game->snake_direction = (uint8_t) ((game->snake_direction + 3U) % 4U);
      changed = true;
    } else if (command == PET_P4_GAME_COMMAND_RIGHT) {
      game->snake_direction = (uint8_t) ((game->snake_direction + 1U) % 4U);
      changed = true;
    }
  }
  if (changed) game_refresh_frame(game);
  return changed;
}

bool pet_p4_game_dispatch_action(
  pet_p4_game_engine_t *game,
  const char *action,
  uint64_t now_ms
) {
  if (!game || game->kind != PET_P4_GAME_BOUNDED || !action || !action[0]) {
    return false;
  }
  bool changed = bounded_process_trigger(
    game,
    PET_P4_GAME_TRIGGER_ACTION,
    action,
    now_ms
  );
  if (changed) game_refresh_frame(game);
  return changed;
}

bool pet_p4_game_process(pet_p4_game_engine_t *game, uint64_t now_ms) {
  bool changed = false;
  if (!game || !game->running || game->game_over) return false;
  if (game->last_tick_ms == 0) {
    game->last_tick_ms = now_ms;
    return false;
  }
  if (now_ms < game->last_tick_ms) {
    game->last_tick_ms = now_ms;
    return false;
  }
  uint64_t elapsed = now_ms - game->last_tick_ms;
  uint32_t steps = (uint32_t) (elapsed / game->tick_ms);
  if (steps == 0) return false;
  if (steps > 8U) steps = 8U;
  for (uint32_t step = 0; step < steps && game->running; step += 1) {
    if (game->kind == PET_P4_GAME_BOUNDED) {
      changed |= bounded_process_trigger(
        game,
        PET_P4_GAME_TRIGGER_TICK,
        NULL,
        now_ms
      );
    } else if (game->kind == PET_P4_GAME_BLOCKS) changed |= blocks_step_down(game);
    else if (game->kind == PET_P4_GAME_SNAKE) changed |= snake_step(game);
    else changed |= flappy_step(game);
  }
  game->last_tick_ms += (uint64_t) steps * game->tick_ms;
  if (changed) game_refresh_frame(game);
  return changed;
}

void pet_p4_game_get_frame(
  const pet_p4_game_engine_t *game,
  pet_p4_game_frame_t *out
) {
  if (!out) return;
  if (!game) {
    memset(out, 0, sizeof(*out));
    return;
  }
  *out = game->frame;
}
