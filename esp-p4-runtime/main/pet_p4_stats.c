#include "pet_p4_stats.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static void copy_text(char *dest, size_t dest_size, const char *src) {
  if (!dest || dest_size == 0) return;
  if (!src) src = "";
  size_t limit = dest_size - 1;
  size_t copied = 0;
  while (src[copied] && copied < limit) {
    unsigned char lead = (unsigned char) src[copied];
    size_t width = lead < 0x80 ? 1 : ((lead & 0xE0) == 0xC0 ? 2 : ((lead & 0xF0) == 0xE0 ? 3 : 4));
    if (copied + width > limit) break;
    bool valid = true;
    for (size_t i = 1; i < width; i += 1) {
      if (!src[copied + i] || (((unsigned char) src[copied + i] & 0xC0) != 0x80)) {
        valid = false;
        break;
      }
    }
    copied += valid ? width : 1;
  }
  memcpy(dest, src, copied);
  dest[copied] = '\0';
}

static const cJSON *object_item(const cJSON *object, const char *key) {
  if (!cJSON_IsObject(object)) return NULL;
  return cJSON_GetObjectItemCaseSensitive(object, key);
}

static const cJSON *first_object(const cJSON *root, const char *const *keys, size_t count) {
  for (size_t i = 0; i < count; i += 1) {
    const cJSON *value = object_item(root, keys[i]);
    if (cJSON_IsObject(value)) return value;
  }
  return NULL;
}

static const char *first_string(const cJSON *root, const char *const *keys, size_t count) {
  for (size_t i = 0; i < count; i += 1) {
    const cJSON *value = object_item(root, keys[i]);
    if (cJSON_IsString(value) && value->valuestring && value->valuestring[0]) {
      return value->valuestring;
    }
  }
  return "";
}

static bool first_number(
  const cJSON *primary,
  const cJSON *fallback,
  const char *const *keys,
  size_t count,
  double *out
) {
  const cJSON *objects[] = {primary, fallback};
  for (size_t object_index = 0; object_index < 2; object_index += 1) {
    const cJSON *object = objects[object_index];
    if (!cJSON_IsObject(object)) continue;
    for (size_t i = 0; i < count; i += 1) {
      const cJSON *value = object_item(object, keys[i]);
      if (cJSON_IsNumber(value) && value->valuedouble >= 0) {
        *out = value->valuedouble;
        return true;
      }
    }
  }
  return false;
}

static void update_u64(
  uint64_t *target,
  uint32_t *valid,
  uint32_t flag,
  const cJSON *primary,
  const cJSON *fallback,
  const char *const *keys,
  size_t key_count
) {
  double value = 0;
  if (first_number(primary, fallback, keys, key_count, &value)) {
    *target = (uint64_t) value;
    *valid |= flag;
  }
}

static void update_u32(
  uint32_t *target,
  uint32_t *valid,
  uint32_t flag,
  const cJSON *primary,
  const cJSON *fallback,
  const char *const *keys,
  size_t key_count
) {
  double value = 0;
  if (first_number(primary, fallback, keys, key_count, &value)) {
    *target = value > 4294967295.0 ? UINT32_MAX : (uint32_t) value;
    *valid |= flag;
  }
}

void pet_p4_stats_init(pet_p4_stats_model_t *model) {
  if (!model) return;
  memset(model, 0, sizeof(*model));
  copy_text(model->state, sizeof(model->state), "idle");
}

void pet_p4_stats_update(
  pet_p4_stats_model_t *model,
  const cJSON *payload,
  const char *fallback_source
) {
  static const char *const usage_keys[] = {"tokenUsage", "token_usage", "usage"};
  static const char *const source_keys[] = {"source", "agent", "agentId", "agent_id"};
  static const char *const state_keys[] = {"state", "status"};
  static const char *const title_keys[] = {"sessionTitle", "session_title", "title", "displayTitle"};
  static const char *const total_keys[] = {"totalTokens", "total_tokens"};
  static const char *const input_keys[] = {"inputTokens", "input_tokens"};
  static const char *const output_keys[] = {"outputTokens", "output_tokens"};
  static const char *const cached_keys[] = {"cachedInputTokens", "cached_input_tokens", "cacheReadInputTokens"};
  static const char *const reasoning_keys[] = {"reasoningOutputTokens", "reasoning_output_tokens", "reasoningTokens"};
  static const char *const context_keys[] = {"modelContextWindow", "model_context_window", "contextTokens", "context_tokens"};
  static const char *const cost_keys[] = {"estimatedCostUsd", "estimated_cost_usd", "costUsd", "cost_usd"};
  static const char *const updated_keys[] = {"tsMs", "updatedAtMs", "updated_at_ms"};
  static const char *const context_percent_keys[] = {"contextUsagePct", "context_usage_pct"};
  static const char *const tool_calls_keys[] = {"toolCalls", "tool_calls"};
  static const char *const tool_errors_keys[] = {"toolErrors", "tool_errors"};
  static const char *const waiting_keys[] = {"waitingUserMs", "waiting_user_ms"};
  static const char *const turn_keys[] = {"turnMs", "turn_ms"};
  static const char *const first_token_keys[] = {"firstTokenMs", "first_token_ms"};
  const cJSON *usage;
  const cJSON *metrics;
  const cJSON *latency;
  const char *text;
  double number = 0;

  if (!model || !cJSON_IsObject(payload)) return;
  usage = first_object(payload, usage_keys, sizeof(usage_keys) / sizeof(usage_keys[0]));
  metrics = object_item(payload, "metrics");
  latency = object_item(metrics, "latency");

  text = first_string(payload, source_keys, sizeof(source_keys) / sizeof(source_keys[0]));
  if (!text[0]) text = fallback_source ? fallback_source : "";
  if (text[0]) copy_text(model->source, sizeof(model->source), text);
  text = first_string(payload, state_keys, sizeof(state_keys) / sizeof(state_keys[0]));
  if (text[0]) copy_text(model->state, sizeof(model->state), text);
  text = first_string(payload, title_keys, sizeof(title_keys) / sizeof(title_keys[0]));
  if (text[0]) copy_text(model->session_title, sizeof(model->session_title), text);

  update_u64(&model->total_tokens, &model->valid, PET_P4_STATS_HAS_TOTAL_TOKENS,
             usage, payload, total_keys, sizeof(total_keys) / sizeof(total_keys[0]));
  update_u64(&model->input_tokens, &model->valid, PET_P4_STATS_HAS_INPUT_TOKENS,
             usage, payload, input_keys, sizeof(input_keys) / sizeof(input_keys[0]));
  update_u64(&model->output_tokens, &model->valid, PET_P4_STATS_HAS_OUTPUT_TOKENS,
             usage, payload, output_keys, sizeof(output_keys) / sizeof(output_keys[0]));
  update_u64(&model->cached_tokens, &model->valid, PET_P4_STATS_HAS_CACHED_TOKENS,
             usage, payload, cached_keys, sizeof(cached_keys) / sizeof(cached_keys[0]));
  update_u64(&model->reasoning_tokens, &model->valid, PET_P4_STATS_HAS_REASONING_TOKENS,
             usage, payload, reasoning_keys, sizeof(reasoning_keys) / sizeof(reasoning_keys[0]));
  update_u64(&model->context_window, &model->valid, PET_P4_STATS_HAS_CONTEXT_WINDOW,
             usage, payload, context_keys, sizeof(context_keys) / sizeof(context_keys[0]));
  update_u64(&model->updated_at_ms, &model->valid, 0,
             payload, NULL, updated_keys, sizeof(updated_keys) / sizeof(updated_keys[0]));

  if (first_number(usage, payload, cost_keys, sizeof(cost_keys) / sizeof(cost_keys[0]), &number)) {
    model->cost_microusd = (uint64_t) (number * 1000000.0 + 0.5);
    model->valid |= PET_P4_STATS_HAS_COST;
  }
  if (first_number(metrics, payload, context_percent_keys,
                   sizeof(context_percent_keys) / sizeof(context_percent_keys[0]), &number)) {
    if (number > 100.0) number = 100.0;
    model->context_percent_x100 = (uint32_t) (number * 100.0 + 0.5);
    model->valid |= PET_P4_STATS_HAS_CONTEXT_PERCENT;
  }
  update_u32(&model->tool_calls, &model->valid, PET_P4_STATS_HAS_TOOL_CALLS,
             metrics, payload, tool_calls_keys, sizeof(tool_calls_keys) / sizeof(tool_calls_keys[0]));
  update_u32(&model->tool_errors, &model->valid, PET_P4_STATS_HAS_TOOL_ERRORS,
             metrics, payload, tool_errors_keys, sizeof(tool_errors_keys) / sizeof(tool_errors_keys[0]));
  update_u64(&model->waiting_user_ms, &model->valid, PET_P4_STATS_HAS_WAITING_USER_MS,
             metrics, payload, waiting_keys, sizeof(waiting_keys) / sizeof(waiting_keys[0]));
  update_u64(&model->turn_ms, &model->valid, PET_P4_STATS_HAS_TURN_MS,
             latency, metrics, turn_keys, sizeof(turn_keys) / sizeof(turn_keys[0]));
  update_u64(&model->first_token_ms, &model->valid, PET_P4_STATS_HAS_FIRST_TOKEN_MS,
             latency, metrics, first_token_keys, sizeof(first_token_keys) / sizeof(first_token_keys[0]));

  if (!(model->valid & PET_P4_STATS_HAS_CONTEXT_PERCENT)
      && (model->valid & PET_P4_STATS_HAS_TOTAL_TOKENS)
      && (model->valid & PET_P4_STATS_HAS_CONTEXT_WINDOW)
      && model->context_window > 0) {
    uint64_t scaled = model->total_tokens <= UINT64_MAX / 10000ULL
      ? model->total_tokens * 10000ULL
      : UINT64_MAX;
    uint64_t percent = scaled / model->context_window;
    model->context_percent_x100 = percent > 10000ULL ? 10000U : (uint32_t) percent;
    model->valid |= PET_P4_STATS_HAS_CONTEXT_PERCENT;
  }
}
