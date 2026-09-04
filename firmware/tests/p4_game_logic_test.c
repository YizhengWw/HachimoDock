#include <assert.h>
#include <stdint.h>
#include <string.h>

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

  config.entities[0].shape = (pet_p4_game_shape_t) (PET_P4_GAME_SHAPE_BALL + 1);
  assert(!pet_p4_game_configure_bounded(&game, &config, 17));
}

int main(void) {
  test_blocks_move_drop_and_game_over();
  test_snake_moves_eats_and_turns();
  test_flappy_flaps_scrolls_scores_and_restarts();
  test_bounded_runtime_uses_data_rules_for_a_new_game();
  return 0;
}
