#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "../main/pet_p4_game.h"

static int occupied(const pet_p4_game_frame_t *frame) {
  int count = 0;
  for (int i = 0; i < frame->width * frame->height; i += 1) {
    if (frame->cells[i] != 0) count += 1;
  }
  return count;
}

static void test_blocks_move_drop_and_game_over(void) {
  pet_p4_game_engine_t game;
  pet_p4_game_frame_t before;
  pet_p4_game_frame_t after;
  assert(pet_p4_game_configure(&game, PET_P4_GAME_BLOCKS, 480, 7));
  pet_p4_game_get_frame(&game, &before);
  assert(before.width == 10);
  assert(before.height == 16);
  assert(occupied(&before) == 4);
  assert(!before.running);

  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_START, 10));
  pet_p4_game_get_frame(&game, &before);
  assert(before.running);
  assert(pet_p4_game_process(&game, 500));
  pet_p4_game_get_frame(&game, &after);
  assert(memcmp(before.cells, after.cells, sizeof(before.cells)) != 0);

  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_DROP, 510));
  pet_p4_game_get_frame(&game, &after);
  assert(after.score > 0);
  assert(occupied(&after) >= 8);

  uint64_t now = 510;
  for (int i = 0; i < 1000 && !game.game_over; i += 1) {
    now += game.tick_ms;
    (void) pet_p4_game_process(&game, now);
  }
  assert(game.game_over);
  assert(!game.running);
}

static void test_snake_moves_eats_and_turns(void) {
  pet_p4_game_engine_t game;
  pet_p4_game_frame_t before;
  pet_p4_game_frame_t after;
  assert(pet_p4_game_configure(&game, PET_P4_GAME_SNAKE, 220, 11));
  pet_p4_game_get_frame(&game, &before);
  assert(before.width == 16);
  assert(before.height == 10);
  assert(occupied(&before) == 5);

  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_START, 10));
  pet_p4_game_get_frame(&game, &before);
  for (int i = 1; i <= 5; i += 1) {
    assert(pet_p4_game_process(&game, 10 + (uint64_t) i * game.tick_ms));
  }
  pet_p4_game_get_frame(&game, &after);
  assert(after.score == 1);
  assert(occupied(&after) == 6);
  assert(memcmp(before.cells, after.cells, sizeof(before.cells)) != 0);

  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_RIGHT, 1200));
  assert(pet_p4_game_process(&game, 1420));
  assert(game.snake_direction == 1);
}

static void test_flappy_flaps_scrolls_scores_and_restarts(void) {
  pet_p4_game_engine_t game;
  pet_p4_game_frame_t before;
  pet_p4_game_frame_t after;
  assert(pet_p4_game_configure(&game, PET_P4_GAME_FLAPPY, 100, 13));
  pet_p4_game_get_frame(&game, &before);
  assert(before.width == 16);
  assert(before.height == 10);
  assert(!before.running);
  assert(occupied(&before) > 2);

  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_FLAP, 10));
  pet_p4_game_get_frame(&game, &before);
  assert(before.running);
  int16_t flap_start_y_q8 = game.flappy_y_q8;
  assert(pet_p4_game_process(&game, 10 + game.tick_ms));
  assert(flap_start_y_q8 - game.flappy_y_q8 == (1 << 8));

  uint64_t now = 10 + game.tick_ms;
  for (int step = 0; step < 24; step += 1) {
    game.flappy_y_q8 = (int16_t) (4 << 8);
    game.flappy_velocity_q8 = 0;
    game.flappy_pipe_gap_top[0] = 2;
    game.flappy_pipe_gap_top[1] = 2;
    now += game.tick_ms;
    assert(pet_p4_game_process(&game, now));
    assert(!game.game_over);
  }
  pet_p4_game_get_frame(&game, &after);
  assert(after.score >= 1);
  assert(memcmp(before.cells, after.cells, sizeof(before.cells)) != 0);

  game.flappy_y_q8 = -256;
  game.flappy_velocity_q8 = 0;
  now += game.tick_ms;
  assert(pet_p4_game_process(&game, now));
  assert(game.game_over);
  assert(!game.running);

  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_FLAP, now + 1));
  assert(game.running);
  assert(!game.game_over);
  assert(game.score == 0);
}

static void test_flappy_half_vertical_speed_keeps_pipe_cadence(void) {
  pet_p4_game_engine_t game;
  assert(pet_p4_game_configure(&game, PET_P4_GAME_FLAPPY, 100, 13));
  assert(game.flappy_vertical_speed_percent == 100);
  assert(!pet_p4_game_set_vertical_speed(&game, 0));
  assert(!pet_p4_game_set_vertical_speed(&game, 101));
  assert(pet_p4_game_set_vertical_speed(&game, 50));
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_FLAP, 10));
  const int pipe_start = game.flappy_pipe_x[0];
  int old_velocity = -256;
  for (int step = 1; step <= 12; ++step) {
    /* Reset only position to avoid a floor collision while checking the
     * entire old/new velocity curve through terminal fall speed. */
    game.flappy_y_q8 = 4 << 8;
    game.flappy_pipe_gap_top[0] = 2;
    game.flappy_pipe_gap_top[1] = 2;
    const int y = game.flappy_y_q8;
    assert(game.flappy_velocity_q8 == old_velocity);
    assert(pet_p4_game_process(&game, 10 + step * 100));
    assert(!game.game_over);
    assert(2 * (game.flappy_y_q8 - y) == old_velocity);
    old_velocity += 80;
    if (old_velocity > 512) old_velocity = 512;
    assert(game.flappy_velocity_q8 == old_velocity);
    assert(game.flappy_pipe_x[0] == pipe_start - step / 2);
  }
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_FLAP, 1211));
  assert(game.flappy_velocity_q8 == -256);
  assert(game.flappy_vertical_speed_percent == 50);
}

static void test_bounded_runtime_uses_data_rules_for_a_new_game(void) {
  pet_p4_bounded_game_config_t config = {0};
  pet_p4_game_engine_t game;
  pet_p4_game_frame_t frame;
  config.width = 8;
  config.height = 6;
  config.tick_ms = 100;
  config.entity_count = 2;
  strcpy(config.entities[0].id, "player");
  config.entities[0].x = 3;
  config.entities[0].y = 5;
  config.entities[0].width = 1;
  config.entities[0].height = 1;
  config.entities[0].tone = 3;
  config.entities[0].shape = PET_P4_GAME_SHAPE_PADDLE;
  config.entities[0].active = true;
  config.entities[0].collidable = true;
  config.entities[0].bounds = PET_P4_GAME_BOUNDS_CLAMP;
  strcpy(config.entities[1].id, "drop");
  config.entities[1].x = 3;
  config.entities[1].y = 0;
  config.entities[1].width = 1;
  config.entities[1].height = 1;
  config.entities[1].tone = 2;
  config.entities[1].shape = PET_P4_GAME_SHAPE_STAR;
  config.entities[1].vy = 1;
  config.entities[1].active = true;
  config.entities[1].collidable = true;
  config.entities[1].bounds = PET_P4_GAME_BOUNDS_HIDE;

  config.rule_count = 4;
  config.rules[0].trigger = PET_P4_GAME_TRIGGER_ACTION;
  strcpy(config.rules[0].action, "catch.start");
  config.rules[0].op_count = 1;
  config.rules[0].ops[0].kind = PET_P4_GAME_OP_RESTART;
  config.rules[0].ops[0].entity_index = -1;
  config.rules[0].ops[0].source_index = -1;

  config.rules[1].trigger = PET_P4_GAME_TRIGGER_ACTION;
  strcpy(config.rules[1].action, "catch.left");
  config.rules[1].op_count = 1;
  config.rules[1].ops[0].kind = PET_P4_GAME_OP_MOVE;
  config.rules[1].ops[0].entity_index = 0;
  config.rules[1].ops[0].source_index = -1;
  config.rules[1].ops[0].dx = -1;

  config.rules[2].trigger = PET_P4_GAME_TRIGGER_COLLISION;
  config.rules[2].entity_index = 0;
  config.rules[2].with_index = 1;
  config.rules[2].op_count = 2;
  config.rules[2].ops[0].kind = PET_P4_GAME_OP_SCORE_ADD;
  config.rules[2].ops[0].entity_index = -1;
  config.rules[2].ops[0].source_index = -1;
  config.rules[2].ops[0].value = 1;
  config.rules[2].ops[1].kind = PET_P4_GAME_OP_PLACE;
  config.rules[2].ops[1].entity_index = 1;
  config.rules[2].ops[1].source_index = -1;
  config.rules[2].ops[1].has_x = true;
  config.rules[2].ops[1].random_x = true;
  config.rules[2].ops[1].x_min = 0;
  config.rules[2].ops[1].x_max = 7;
  config.rules[2].ops[1].has_y = true;
  config.rules[2].ops[1].y_min = 0;
  config.rules[2].ops[1].y_max = 0;

  config.rules[3].trigger = PET_P4_GAME_TRIGGER_EDGE;
  config.rules[3].entity_index = 1;
  config.rules[3].with_index = -1;
  config.rules[3].edge = PET_P4_GAME_EDGE_BOTTOM;
  config.rules[3].op_count = 1;
  config.rules[3].ops[0] = config.rules[2].ops[1];

  assert(pet_p4_game_configure_bounded(&game, &config, 17));
  assert(game.kind == PET_P4_GAME_BOUNDED);
  assert(pet_p4_game_dispatch_action(&game, "catch.start", 10));
  assert(game.running);
  for (int step = 1; step <= 5; step += 1) {
    assert(pet_p4_game_process(&game, 10 + (uint64_t) step * 100));
  }
  assert(game.score == 1);
  assert(game.bounded_entities[1].active);
  assert(game.bounded_entities[1].y == 0);
  pet_p4_game_get_frame(&game, &frame);
  assert(frame.kind == PET_P4_GAME_BOUNDED);
  assert(frame.width == 8 && frame.height == 6);
  assert(frame.score == 1);
  assert(frame.entity_count == 2);
  assert(frame.entities[0].shape == PET_P4_GAME_SHAPE_PADDLE);
  assert(frame.entities[1].shape == PET_P4_GAME_SHAPE_STAR);
  assert(frame.cells[5 * frame.width + 3] == 0);

  assert(pet_p4_game_dispatch_action(&game, "catch.left", 520));
  assert(game.bounded_entities[0].x == 2);

  config.entities[0].shape = (pet_p4_game_shape_t) (PET_P4_GAME_SHAPE_CHARACTER + 1);
  assert(!pet_p4_game_configure_bounded(&game, &config, 17));
}

/* ---- gomoku ------------------------------------------------------------- */

#define G PET_P4_GAME_GOMOKU_SIZE

static int gomoku_at(int x, int y) { return y * G + x; }

static void gomoku_start(pet_p4_game_engine_t *game) {
  assert(pet_p4_game_configure(game, PET_P4_GAME_GOMOKU, 300, 5));
  assert(game->width == G && game->height == G);
  assert(!game->running);
  assert(pet_p4_game_command(game, PET_P4_GAME_COMMAND_PLACE, 10));
  assert(game->running && !game->game_over && game->score == 0);
  assert(game->gomoku_cursor_x == 7 && game->gomoku_cursor_y == 7);
}

static void gomoku_move_cursor(pet_p4_game_engine_t *game, int x, int y, uint64_t now) {
  while (game->gomoku_cursor_x > x) assert(pet_p4_game_command(game, PET_P4_GAME_COMMAND_LEFT, now));
  while (game->gomoku_cursor_x < x) assert(pet_p4_game_command(game, PET_P4_GAME_COMMAND_RIGHT, now));
  while (game->gomoku_cursor_y > y) assert(pet_p4_game_command(game, PET_P4_GAME_COMMAND_NORTH, now));
  while (game->gomoku_cursor_y < y) assert(pet_p4_game_command(game, PET_P4_GAME_COMMAND_SOUTH, now));
}

/* Lay a position directly (both colours), then let black play the final
 * stone through the real cursor/place path so win detection runs. */
static void gomoku_set(pet_p4_game_engine_t *game, int x, int y, uint8_t color) {
  game->board[gomoku_at(x, y)] = color;
  game->gomoku_stones += 1;
  if (color == 1) game->gomoku_rounds += 1;
}

static void test_gomoku_player_wins_in_all_four_directions(void) {
  static const int dirs[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
  for (int d = 0; d < 4; d += 1) {
    pet_p4_game_engine_t game;
    pet_p4_game_frame_t frame;
    gomoku_start(&game);
    int sx = 3, sy = d == 3 ? 9 : 3;
    for (int i = 0; i < 4; i += 1) gomoku_set(&game, sx + i * dirs[d][0], sy + i * dirs[d][1], 1);
    /* white stones elsewhere keep the board honest but irrelevant */
    gomoku_set(&game, 12, 12, 2);
    gomoku_set(&game, 13, 12, 2);
    gomoku_set(&game, 12, 13, 2);
    int fx = sx + 4 * dirs[d][0], fy = sy + 4 * dirs[d][1];
    gomoku_move_cursor(&game, fx, fy, 20);
    assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 30));
    assert(game.game_over && !game.running);
    assert(game.gomoku_outcome == 1);
    assert(game.gomoku_win_len == 5);
    assert(game.score == 1000 + 8); /* five black + three white stones */
    pet_p4_game_get_frame(&game, &frame);
    int highlighted = 0;
    for (int i = 0; i < G * G; i += 1) {
      if (frame.cells[i] & PET_P4_GAME_GOMOKU_WIN) {
        highlighted += 1;
        assert((frame.cells[i] & PET_P4_GAME_GOMOKU_STONE_MASK) == 1);
      }
      assert(!(frame.cells[i] & PET_P4_GAME_GOMOKU_CURSOR));
    }
    assert(highlighted == 5);
    /* nothing moves after the result; place restarts an empty board */
    assert(!pet_p4_game_command(&game, PET_P4_GAME_COMMAND_LEFT, 40));
    assert(!pet_p4_game_process(&game, 2000));
    assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 50));
    assert(game.running && !game.game_over && game.score == 0);
    assert(occupied(&game.frame) == 1); /* only the cursor overlay */
  }
}

static void test_gomoku_ai_replies_on_tick_and_blocks_an_open_four(void) {
  pet_p4_game_engine_t game;
  pet_p4_game_frame_t frame;
  gomoku_start(&game);
  /* black: three in a row at (5..7, 7); white: two harmless stones */
  gomoku_set(&game, 5, 7, 1);
  gomoku_set(&game, 6, 7, 1);
  gomoku_set(&game, 7, 7, 1);
  gomoku_set(&game, 2, 2, 2);
  gomoku_set(&game, 2, 12, 2);
  gomoku_move_cursor(&game, 8, 7, 20);
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 30)); /* open four 5..8 */
  assert(game.gomoku_ai_pending && game.score == 6); /* 4 black + 2 white, white to answer */
  pet_p4_game_get_frame(&game, &frame);
  assert(frame.cells[gomoku_at(8, 7)] == (1 | PET_P4_GAME_GOMOKU_LAST | PET_P4_GAME_GOMOKU_CURSOR));
  /* place is ignored while white is thinking; cursor keeps moving */
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_RIGHT, 31));
  assert(!pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 32));
  assert(!pet_p4_game_process(&game, 309)); /* tick clock runs from start (10) */
  assert(game.gomoku_ai_pending);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  assert(pet_p4_game_process(&game, 310));
  clock_gettime(CLOCK_MONOTONIC, &t1);
  assert(!game.gomoku_ai_pending && !game.game_over);
  assert(game.score == 7); /* the reply is visible to the HUD as the odd->even step */
  assert(game.board[gomoku_at(4, 7)] == 2 || game.board[gomoku_at(9, 7)] == 2);
  assert(game.gomoku_ai_reads > 0 && game.gomoku_ai_reads <= PET_P4_GAME_GOMOKU_AI_MAX_READS);
  long long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000LL + (t1.tv_nsec - t0.tv_nsec) / 1000;
  assert(elapsed_us < 20000); /* host sanity bound; the device budget is the read count */
  pet_p4_game_get_frame(&game, &frame);
  int ai = game.gomoku_last_ai;
  assert((frame.cells[ai] & PET_P4_GAME_GOMOKU_STONE_MASK) == 2);
  assert(frame.cells[ai] & PET_P4_GAME_GOMOKU_LAST);
  assert(!(frame.cells[gomoku_at(8, 7)] & PET_P4_GAME_GOMOKU_LAST));
  /* a second tick with nothing pending changes nothing */
  assert(!pet_p4_game_process(&game, 610));
}

static void test_gomoku_ai_completes_its_own_five_over_blocking(void) {
  pet_p4_game_engine_t game;
  gomoku_start(&game);
  /* white has an open diagonal four; black also threatens a five */
  for (int i = 0; i < 4; i += 1) gomoku_set(&game, 3 + i, 3 + i, 2);
  for (int i = 0; i < 4; i += 1) gomoku_set(&game, 1 + i, 12, 1);
  gomoku_move_cursor(&game, 10, 12, 20); /* black plays away from both threats */
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 30));
  assert(pet_p4_game_process(&game, 330));
  assert(game.game_over && game.gomoku_outcome == 2);
  assert(game.board[gomoku_at(7, 7)] == 2 || game.board[gomoku_at(2, 2)] == 2);
  assert(game.gomoku_win_len == 5 && game.gomoku_win_dir == 2);
  assert(game.score == 2000 + game.gomoku_stones && game.gomoku_stones == 10);
}

static void test_gomoku_undo_takes_back_the_last_pair(void) {
  pet_p4_game_engine_t game;
  gomoku_start(&game);
  assert(!pet_p4_game_command(&game, PET_P4_GAME_COMMAND_UNDO, 11)); /* nothing yet */
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 12)); /* centre */
  assert(game.score == 1);
  /* undo before the reply removes only the player's stone */
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_UNDO, 13));
  assert(game.score == 0 && !game.gomoku_ai_pending && occupied(&game.frame) == 1);
  assert(!pet_p4_game_process(&game, 400)); /* no pending reply left */
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 500));
  assert(pet_p4_game_process(&game, 800));
  int ai = game.gomoku_last_ai;
  assert(game.board[ai] == 2 && game.board[gomoku_at(7, 7)] == 1 && game.score == 2);
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_RIGHT, 810));
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_UNDO, 820));
  assert(game.board[ai] == 0 && game.board[gomoku_at(7, 7)] == 0 && game.score == 0);
  assert(game.gomoku_cursor_x == 7 && game.gomoku_cursor_y == 7); /* cursor returns to the undone stone */
  assert(!pet_p4_game_command(&game, PET_P4_GAME_COMMAND_UNDO, 830)); /* single level */
}

static void test_gomoku_cursor_clamps_and_rejects_occupied_cells(void) {
  pet_p4_game_engine_t game;
  gomoku_start(&game);
  gomoku_move_cursor(&game, 0, 0, 20);
  assert(!pet_p4_game_command(&game, PET_P4_GAME_COMMAND_LEFT, 21));
  assert(!pet_p4_game_command(&game, PET_P4_GAME_COMMAND_NORTH, 22));
  gomoku_move_cursor(&game, G - 1, G - 1, 23);
  assert(!pet_p4_game_command(&game, PET_P4_GAME_COMMAND_RIGHT, 24));
  assert(!pet_p4_game_command(&game, PET_P4_GAME_COMMAND_SOUTH, 25));
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 26));
  assert(pet_p4_game_process(&game, 326));
  assert(!pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 327)); /* occupied */
  gomoku_move_cursor(&game, game.gomoku_last_ai % G, game.gomoku_last_ai / G, 328);
  assert(!pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 329)); /* on a white stone */
}

static void test_gomoku_full_board_is_a_draw(void) {
  pet_p4_game_engine_t game;
  gomoku_start(&game);
  /* colour(x, y) = ((x + 2y) mod 4 < 2) never connects five in any direction
   * and gives black 113 cells; leave the black centre for the final move. */
  for (int y = 0; y < G; y += 1) {
    for (int x = 0; x < G; x += 1) {
      if (x == 7 && y == 7) continue;
      gomoku_set(&game, x, y, (uint8_t) ((x + 2 * y) % 4 < 2 ? 1 : 2));
    }
  }
  assert(game.gomoku_rounds == 112 && game.gomoku_stones == 224);
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 30));
  assert(game.game_over && game.gomoku_outcome == 3 && game.gomoku_win_len == 0);
  assert(game.score == 3000 + 225);
  assert(!pet_p4_game_process(&game, 1000));
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 40));
  assert(game.running && game.score == 0);
}

static void test_gomoku_ai_work_stays_bounded_from_an_empty_board(void) {
  pet_p4_game_engine_t game;
  gomoku_start(&game);
  assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, 12));
  assert(pet_p4_game_process(&game, 312));
  /* the very first reply scans the maximum number of empty cells */
  assert(game.gomoku_ai_reads == (G * G - 1) * 4 * 9);
  assert(game.gomoku_ai_reads <= PET_P4_GAME_GOMOKU_AI_MAX_READS);
  /* first reply sits next to the opening stone, not in a corner */
  int ax = game.gomoku_last_ai % G, ay = game.gomoku_last_ai / G;
  assert(ax >= 6 && ax <= 8 && ay >= 6 && ay <= 8);
  /* a naive player who always places at the lowest free cell loses to the AI */
  uint64_t now = 312;
  for (int round = 0; round < 120 && !game.game_over; round += 1) {
    int target = -1;
    for (int i = 0; i < G * G; i += 1) if (game.board[i] == 0) { target = i; break; }
    assert(target >= 0);
    gomoku_move_cursor(&game, target % G, target / G, now + 1);
    assert(pet_p4_game_command(&game, PET_P4_GAME_COMMAND_PLACE, now + 2));
    now += 300;
    if (!game.game_over) assert(pet_p4_game_process(&game, now));
  }
  assert(game.game_over && game.gomoku_outcome == 2);
}

int main(void) {
  test_gomoku_player_wins_in_all_four_directions();
  test_gomoku_ai_replies_on_tick_and_blocks_an_open_four();
  test_gomoku_ai_completes_its_own_five_over_blocking();
  test_gomoku_undo_takes_back_the_last_pair();
  test_gomoku_cursor_clamps_and_rejects_occupied_cells();
  test_gomoku_full_board_is_a_draw();
  test_gomoku_ai_work_stays_bounded_from_an_empty_board();
  test_blocks_move_drop_and_game_over();
  test_snake_moves_eats_and_turns();
  test_flappy_flaps_scrolls_scores_and_restarts();
  test_flappy_half_vertical_speed_keeps_pipe_cadence();
  test_bounded_runtime_uses_data_rules_for_a_new_game();
  return 0;
}
