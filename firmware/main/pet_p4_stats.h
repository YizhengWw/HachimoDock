#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;

#define PET_P4_STATS_SOURCE_MAX 32
#define PET_P4_STATS_STATE_MAX 24
#define PET_P4_STATS_TITLE_MAX 256

enum {
  PET_P4_STATS_HAS_TOTAL_TOKENS = 1U << 0,
  PET_P4_STATS_HAS_INPUT_TOKENS = 1U << 1,
  PET_P4_STATS_HAS_OUTPUT_TOKENS = 1U << 2,
  PET_P4_STATS_HAS_CACHED_TOKENS = 1U << 3,
  PET_P4_STATS_HAS_REASONING_TOKENS = 1U << 4,
  PET_P4_STATS_HAS_CONTEXT_WINDOW = 1U << 5,
  PET_P4_STATS_HAS_CONTEXT_PERCENT = 1U << 6,
  PET_P4_STATS_HAS_COST = 1U << 7,
  PET_P4_STATS_HAS_TURN_MS = 1U << 8,
  PET_P4_STATS_HAS_FIRST_TOKEN_MS = 1U << 9,
  PET_P4_STATS_HAS_WAITING_USER_MS = 1U << 10,
  PET_P4_STATS_HAS_TOOL_CALLS = 1U << 11,
  PET_P4_STATS_HAS_TOOL_ERRORS = 1U << 12,
};

typedef struct {
  char source[PET_P4_STATS_SOURCE_MAX];
  char state[PET_P4_STATS_STATE_MAX];
  char session_title[PET_P4_STATS_TITLE_MAX];
  uint64_t total_tokens;
  uint64_t input_tokens;
  uint64_t output_tokens;
  uint64_t cached_tokens;
  uint64_t reasoning_tokens;
  uint64_t context_window;
  uint64_t cost_microusd;
  uint64_t turn_ms;
  uint64_t first_token_ms;
  uint64_t waiting_user_ms;
  uint64_t updated_at_ms;
  uint32_t context_percent_x100;
  uint32_t tool_calls;
  uint32_t tool_errors;
  uint32_t valid;
} pet_p4_stats_model_t;

void pet_p4_stats_init(pet_p4_stats_model_t *model);
void pet_p4_stats_update(
  pet_p4_stats_model_t *model,
  const cJSON *payload,
  const char *fallback_source
);

#ifdef __cplusplus
}
#endif
