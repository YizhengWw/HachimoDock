#include "runtime_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "runtime_common.h"
#include "runtime_json.h"

#define BR_STATS_MAX_SOURCES 16
#define BR_STATS_MAX_SESSIONS 64
#define BR_STATS_DATE_LEN 16

typedef struct {
  long long total_tokens;
  long long input_tokens;
  long long output_tokens;
  long long cached_input_tokens;
  long long cache_creation_input_tokens;
  long long reasoning_output_tokens;
  double estimated_cost_usd;
} br_stats_totals;

typedef struct {
  char source[64];
  br_stats_totals totals;
  /* last_update_ms tracks when this bucket most recently received any token
     update — used by br_stats_active_source() to pick the agent the user is
     CURRENTLY using rather than the all-time-tokens leader (which would
     otherwise pin the dashboard to whichever agent has historically chewed
     more tokens, even when the user is now mid-session in a different one). */
  long long last_update_ms;
} br_stats_source_bucket;

typedef struct {
  char source[64];
  char session_key[160];
  long long prev_total;
  long long prev_input;
  long long prev_output;
  long long prev_cached;
  long long prev_cache_create;
  long long prev_reasoning;
  double prev_cost;
} br_stats_session_entry;

typedef struct {
  bool initialized;
  char root_dir[BR_MAX_PATH];
  char stats_dir[BR_MAX_PATH];
  char today_path[BR_MAX_PATH];
  char sessions_path[BR_MAX_PATH];
  char display_path[BR_MAX_PATH];
  long long tokens_per_coffee;
  int tz_offset_sec;

  char date_stamp[BR_STATS_DATE_LEN];
  long long updated_at_ms;
  br_stats_totals totals;
  br_stats_source_bucket sources[BR_STATS_MAX_SOURCES];
  size_t source_count;

  br_stats_session_entry sessions[BR_STATS_MAX_SESSIONS];
  size_t session_count;

  bool dirty;
} br_stats_ctx;

static br_stats_ctx g_ctx;

static void br_stats_local_date_stamp(long long ms, int tz_offset_sec, char *out, size_t out_size) {
  if (!out || out_size < 11) {
    return;
  }
  time_t local = (time_t) (ms / 1000) + (time_t) tz_offset_sec;
  struct tm tm;
  gmtime_r(&local, &tm);
  snprintf(out, out_size, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static bool br_stats_ensure_dir(const char *path) {
  if (!path || !*path) return false;
  struct stat st;
  if (stat(path, &st) == 0) {
    return (st.st_mode & S_IFDIR) != 0;
  }
  if (mkdir(path, 0755) == 0) {
    return true;
  }
  return false;
}

static void br_stats_compose_paths(br_stats_ctx *ctx) {
  snprintf(ctx->stats_dir, sizeof(ctx->stats_dir), "%s/stats", ctx->root_dir);
  snprintf(ctx->today_path, sizeof(ctx->today_path), "%s/today.json", ctx->stats_dir);
  snprintf(ctx->sessions_path, sizeof(ctx->sessions_path), "%s/sessions.json", ctx->stats_dir);
  snprintf(ctx->display_path, sizeof(ctx->display_path), "%s/.stats-display", ctx->root_dir);
}

static br_stats_source_bucket *br_stats_find_or_create_source(br_stats_ctx *ctx, const char *source) {
  if (!source || !*source) {
    source = "unknown";
  }
  for (size_t i = 0; i < ctx->source_count; i += 1) {
    if (strcmp(ctx->sources[i].source, source) == 0) {
      return &ctx->sources[i];
    }
  }
  if (ctx->source_count >= BR_STATS_MAX_SOURCES) {
    return NULL;
  }
  br_stats_source_bucket *bucket = &ctx->sources[ctx->source_count];
  memset(bucket, 0, sizeof(*bucket));
  snprintf(bucket->source, sizeof(bucket->source), "%s", source);
  ctx->source_count += 1;
  return bucket;
}

static void br_stats_session_compose_key(
  const br_bridge_state_update *update,
  char *out,
  size_t out_size
) {
  const char *source = update->source[0] ? update->source : "unknown";
  const char *key = NULL;
  if (update->session_key[0] != '\0') {
    key = update->session_key;
  } else if (update->session_id[0] != '\0') {
    key = update->session_id;
  } else if (update->run_id[0] != '\0') {
    key = update->run_id;
  } else {
    key = "_default";
  }
  snprintf(out, out_size, "%s|%s", source, key);
}

static br_stats_session_entry *br_stats_find_or_create_session(
  br_stats_ctx *ctx,
  const char *source,
  const char *session_key
) {
  for (size_t i = 0; i < ctx->session_count; i += 1) {
    if (strcmp(ctx->sessions[i].source, source) == 0
        && strcmp(ctx->sessions[i].session_key, session_key) == 0) {
      return &ctx->sessions[i];
    }
  }
  if (ctx->session_count >= BR_STATS_MAX_SESSIONS) {
    /* 简单 LRU：覆盖最早的一项。生产端 session 数量不多，64 槽足够。 */
    br_stats_session_entry *victim = &ctx->sessions[0];
    memset(victim, 0, sizeof(*victim));
    snprintf(victim->source, sizeof(victim->source), "%s", source);
    snprintf(victim->session_key, sizeof(victim->session_key), "%s", session_key);
    return victim;
  }
  br_stats_session_entry *entry = &ctx->sessions[ctx->session_count];
  memset(entry, 0, sizeof(*entry));
  snprintf(entry->source, sizeof(entry->source), "%s", source);
  snprintf(entry->session_key, sizeof(entry->session_key), "%s", session_key);
  ctx->session_count += 1;
  return entry;
}

static void br_stats_reset_today(br_stats_ctx *ctx, const char *date_stamp) {
  memset(&ctx->totals, 0, sizeof(ctx->totals));
  ctx->source_count = 0;
  memset(ctx->sources, 0, sizeof(ctx->sources));
  snprintf(ctx->date_stamp, sizeof(ctx->date_stamp), "%s", date_stamp);
}

static void br_stats_archive_today(br_stats_ctx *ctx) {
  if (ctx->date_stamp[0] == '\0') return;
  char archive_path[BR_MAX_PATH];
  snprintf(archive_path, sizeof(archive_path), "%s/%s.json", ctx->stats_dir, ctx->date_stamp);
  /* 仅在 today.json 存在时归档：把当前 today.json 内容写到 YYYY-MM-DD.json。
   * 若文件读失败就静默跳过，下一次 flush 仍会落新日期的 today。 */
  char buffer[BR_MAX_JSON];
  if (br_read_text_file(ctx->today_path, buffer, sizeof(buffer))) {
    br_atomic_write_text(archive_path, buffer);
  }
}

static void br_stats_check_rollover(br_stats_ctx *ctx, long long now_ms) {
  char today[BR_STATS_DATE_LEN];
  br_stats_local_date_stamp(now_ms, ctx->tz_offset_sec, today, sizeof(today));
  if (ctx->date_stamp[0] == '\0') {
    snprintf(ctx->date_stamp, sizeof(ctx->date_stamp), "%s", today);
    return;
  }
  if (strcmp(today, ctx->date_stamp) != 0) {
    /* 把昨日聚合归档；session prev 值保留——下一次帧来时 totalTokens 是单调累加，
     * delta 不会因为跨天而被错误计成全量。 */
    br_stats_archive_today(ctx);
    br_stats_reset_today(ctx, today);
    ctx->dirty = true;
  }
}

static long long br_stats_long_long_field(
  const br_json_token *tokens,
  int count,
  int object_index,
  const char *json,
  const char *key,
  long long fallback
) {
  int idx = br_json_find_key(json, tokens, count, object_index, key);
  if (idx < 0) return fallback;
  double v = 0;
  if (br_json_token_to_double(json, &tokens[idx], &v)) {
    return (long long) v;
  }
  return fallback;
}

static double br_stats_double_field(
  const br_json_token *tokens,
  int count,
  int object_index,
  const char *json,
  const char *key,
  double fallback
) {
  int idx = br_json_find_key(json, tokens, count, object_index, key);
  if (idx < 0) return fallback;
  double v = 0;
  if (br_json_token_to_double(json, &tokens[idx], &v)) {
    return v;
  }
  return fallback;
}

static bool br_stats_string_field(
  const br_json_token *tokens,
  int count,
  int object_index,
  const char *json,
  const char *key,
  char *out,
  size_t out_size
) {
  int idx = br_json_find_key(json, tokens, count, object_index, key);
  if (idx < 0) return false;
  if (tokens[idx].type != BR_JSON_STRING) return false;
  return br_json_token_to_string(json, &tokens[idx], out, out_size);
}

static void br_stats_load_today(br_stats_ctx *ctx) {
  char buffer[BR_MAX_JSON];
  if (!br_read_text_file(ctx->today_path, buffer, sizeof(buffer))) {
    return;
  }
  br_json_token tokens[256];
  int count = br_json_parse(buffer, strlen(buffer), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) return;

  char date[BR_STATS_DATE_LEN];
  if (br_stats_string_field(tokens, count, 0, buffer, "dateStamp", date, sizeof(date))) {
    snprintf(ctx->date_stamp, sizeof(ctx->date_stamp), "%s", date);
  }
  ctx->updated_at_ms = br_stats_long_long_field(tokens, count, 0, buffer, "updatedAtMs", 0);

  int totals_idx = br_json_find_key(buffer, tokens, count, 0, "totals");
  if (totals_idx >= 0 && tokens[totals_idx].type == BR_JSON_OBJECT) {
    ctx->totals.total_tokens = br_stats_long_long_field(tokens, count, totals_idx, buffer, "totalTokens", 0);
    ctx->totals.input_tokens = br_stats_long_long_field(tokens, count, totals_idx, buffer, "inputTokens", 0);
    ctx->totals.output_tokens = br_stats_long_long_field(tokens, count, totals_idx, buffer, "outputTokens", 0);
    ctx->totals.cached_input_tokens =
      br_stats_long_long_field(tokens, count, totals_idx, buffer, "cachedInputTokens", 0);
    ctx->totals.cache_creation_input_tokens =
      br_stats_long_long_field(tokens, count, totals_idx, buffer, "cacheCreationInputTokens", 0);
    ctx->totals.reasoning_output_tokens =
      br_stats_long_long_field(tokens, count, totals_idx, buffer, "reasoningOutputTokens", 0);
    ctx->totals.estimated_cost_usd =
      br_stats_double_field(tokens, count, totals_idx, buffer, "estimatedCostUsd", 0.0);
  }

  int by_source_idx = br_json_find_key(buffer, tokens, count, 0, "bySource");
  if (by_source_idx >= 0 && tokens[by_source_idx].type == BR_JSON_ARRAY) {
    int parent = by_source_idx;
    /* 遍历 tokens 数组：jsmn 用 parent 引用 + size 表示元素，按线性扫描。 */
    int remaining = tokens[by_source_idx].size;
    for (int i = by_source_idx + 1; i < count && remaining > 0; i += 1) {
      if (tokens[i].parent != parent) continue;
      remaining -= 1;
      if (tokens[i].type != BR_JSON_OBJECT) continue;
      char source_name[64];
      if (!br_stats_string_field(tokens, count, i, buffer, "source", source_name, sizeof(source_name))) {
        continue;
      }
      br_stats_source_bucket *bucket = br_stats_find_or_create_source(ctx, source_name);
      if (!bucket) continue;
      bucket->totals.total_tokens = br_stats_long_long_field(tokens, count, i, buffer, "totalTokens", 0);
      bucket->totals.input_tokens = br_stats_long_long_field(tokens, count, i, buffer, "inputTokens", 0);
      bucket->totals.output_tokens = br_stats_long_long_field(tokens, count, i, buffer, "outputTokens", 0);
      bucket->totals.cached_input_tokens =
        br_stats_long_long_field(tokens, count, i, buffer, "cachedInputTokens", 0);
      bucket->totals.cache_creation_input_tokens =
        br_stats_long_long_field(tokens, count, i, buffer, "cacheCreationInputTokens", 0);
      bucket->totals.reasoning_output_tokens =
        br_stats_long_long_field(tokens, count, i, buffer, "reasoningOutputTokens", 0);
      bucket->totals.estimated_cost_usd =
        br_stats_double_field(tokens, count, i, buffer, "estimatedCostUsd", 0.0);
    }
  }
}

static void br_stats_load_sessions(br_stats_ctx *ctx) {
  char buffer[BR_MAX_JSON];
  if (!br_read_text_file(ctx->sessions_path, buffer, sizeof(buffer))) {
    return;
  }
  br_json_token tokens[512];
  int count = br_json_parse(buffer, strlen(buffer), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) return;

  int sessions_idx = br_json_find_key(buffer, tokens, count, 0, "sessions");
  if (sessions_idx < 0 || tokens[sessions_idx].type != BR_JSON_ARRAY) return;

  int parent = sessions_idx;
  int remaining = tokens[sessions_idx].size;
  for (int i = sessions_idx + 1; i < count && remaining > 0; i += 1) {
    if (tokens[i].parent != parent) continue;
    remaining -= 1;
    if (tokens[i].type != BR_JSON_OBJECT) continue;
    if (ctx->session_count >= BR_STATS_MAX_SESSIONS) break;
    br_stats_session_entry *entry = &ctx->sessions[ctx->session_count];
    memset(entry, 0, sizeof(*entry));
    if (!br_stats_string_field(tokens, count, i, buffer, "source", entry->source, sizeof(entry->source))) {
      continue;
    }
    if (!br_stats_string_field(tokens, count, i, buffer, "sessionKey", entry->session_key, sizeof(entry->session_key))) {
      continue;
    }
    entry->prev_total = br_stats_long_long_field(tokens, count, i, buffer, "prevTotal", 0);
    entry->prev_input = br_stats_long_long_field(tokens, count, i, buffer, "prevInput", 0);
    entry->prev_output = br_stats_long_long_field(tokens, count, i, buffer, "prevOutput", 0);
    entry->prev_cached = br_stats_long_long_field(tokens, count, i, buffer, "prevCached", 0);
    entry->prev_cache_create = br_stats_long_long_field(tokens, count, i, buffer, "prevCacheCreate", 0);
    entry->prev_reasoning = br_stats_long_long_field(tokens, count, i, buffer, "prevReasoning", 0);
    entry->prev_cost = br_stats_double_field(tokens, count, i, buffer, "prevCost", 0.0);
    ctx->session_count += 1;
  }
}

int runtime_stats_init(
  const char *root_dir,
  long long tokens_per_coffee,
  int tz_offset_sec,
  long long now_ms
) {
  if (!root_dir || !*root_dir) return -1;
  memset(&g_ctx, 0, sizeof(g_ctx));
  snprintf(g_ctx.root_dir, sizeof(g_ctx.root_dir), "%s", root_dir);
  g_ctx.tokens_per_coffee = tokens_per_coffee > 0 ? tokens_per_coffee : 350000LL;
  g_ctx.tz_offset_sec = tz_offset_sec;
  br_stats_compose_paths(&g_ctx);
  if (!br_stats_ensure_dir(g_ctx.root_dir)) {
    return -1;
  }
  if (!br_stats_ensure_dir(g_ctx.stats_dir)) {
    return -1;
  }
  br_stats_local_date_stamp(now_ms, g_ctx.tz_offset_sec, g_ctx.date_stamp, sizeof(g_ctx.date_stamp));
  br_stats_load_today(&g_ctx);
  br_stats_load_sessions(&g_ctx);
  br_stats_check_rollover(&g_ctx, now_ms);
  g_ctx.initialized = true;
  g_ctx.dirty = true;
  return 0;
}

static void br_stats_apply_token_diff(
  br_stats_totals *target,
  long long delta_total,
  long long delta_input,
  long long delta_output,
  long long delta_cached,
  long long delta_cache_create,
  long long delta_reasoning,
  double delta_cost
) {
  target->total_tokens += delta_total;
  target->input_tokens += delta_input;
  target->output_tokens += delta_output;
  target->cached_input_tokens += delta_cached;
  target->cache_creation_input_tokens += delta_cache_create;
  target->reasoning_output_tokens += delta_reasoning;
  target->estimated_cost_usd += delta_cost;
}

static long long br_stats_positive_delta(long long cur, long long prev) {
  if (cur > prev) return cur - prev;
  return 0;
}

static long long br_stats_delta_with_first_sample(
  long long cur,
  long long prev,
  bool has_last,
  long long last
) {
  if (prev == 0 && cur > 0 && has_last && last >= 0 && last <= cur) {
    return last;
  }
  return br_stats_positive_delta(cur, prev);
}

static double br_stats_positive_delta_double(double cur, double prev) {
  if (cur > prev) return cur - prev;
  return 0.0;
}

void runtime_stats_ingest(const br_bridge_state_update *update, long long now_ms) {
  if (!g_ctx.initialized || !update || !update->has_token_usage) {
    return;
  }
  br_stats_check_rollover(&g_ctx, now_ms);

  char composed_key[224];
  br_stats_session_compose_key(update, composed_key, sizeof(composed_key));
  const char *source = update->source[0] ? update->source : "unknown";
  br_stats_session_entry *entry = br_stats_find_or_create_session(&g_ctx, source, composed_key);
  if (!entry) return;

  const br_token_usage *u = &update->token_usage;
  long long cur_total = u->has_total_tokens ? u->total_tokens : 0;
  long long cur_input = u->has_input_tokens ? u->input_tokens : 0;
  long long cur_output = u->has_output_tokens ? u->output_tokens : 0;
  long long cur_cached = u->has_cached_input_tokens ? u->cached_input_tokens : 0;
  long long cur_cache_create =
    u->has_cache_creation_input_tokens ? u->cache_creation_input_tokens : 0;
  long long cur_reasoning = u->has_reasoning_output_tokens ? u->reasoning_output_tokens : 0;
  double cur_cost = u->has_estimated_cost_usd ? u->estimated_cost_usd : 0.0;

  /* 如果 session 重启 (cur < prev) 或 prev 全 0 但 cur 已经有量，
   * 把 prev 直接对齐 cur，避免 historical retained 把今日 today 拉爆。 */
  bool reset = false;
  if (cur_total > 0 && cur_total < entry->prev_total) {
    reset = true;
  }

  long long delta_total = 0, delta_input = 0, delta_output = 0;
  long long delta_cached = 0, delta_cache_create = 0, delta_reasoning = 0;
  double delta_cost = 0.0;

  if (!reset) {
    delta_total = br_stats_delta_with_first_sample(
      cur_total, entry->prev_total, u->has_last_total_tokens, u->last_total_tokens);
    delta_input = br_stats_delta_with_first_sample(
      cur_input, entry->prev_input, u->has_last_input_tokens, u->last_input_tokens);
    delta_output = br_stats_delta_with_first_sample(
      cur_output, entry->prev_output, u->has_last_output_tokens, u->last_output_tokens);
    delta_cached = br_stats_delta_with_first_sample(
      cur_cached, entry->prev_cached, u->has_last_cached_input_tokens, u->last_cached_input_tokens);
    delta_cache_create = br_stats_delta_with_first_sample(
      cur_cache_create, entry->prev_cache_create,
      u->has_last_cache_creation_input_tokens, u->last_cache_creation_input_tokens);
    delta_reasoning = br_stats_delta_with_first_sample(
      cur_reasoning, entry->prev_reasoning,
      u->has_last_reasoning_output_tokens, u->last_reasoning_output_tokens);
    delta_cost = br_stats_positive_delta_double(cur_cost, entry->prev_cost);
  }

  entry->prev_total = cur_total;
  entry->prev_input = cur_input;
  entry->prev_output = cur_output;
  entry->prev_cached = cur_cached;
  entry->prev_cache_create = cur_cache_create;
  entry->prev_reasoning = cur_reasoning;
  entry->prev_cost = cur_cost;

  if (delta_total == 0 && delta_input == 0 && delta_output == 0
      && delta_cached == 0 && delta_cache_create == 0 && delta_reasoning == 0
      && delta_cost == 0.0) {
    g_ctx.dirty = true; /* 即使没有 delta，session prev 也变了，需要 flush */
    g_ctx.updated_at_ms = now_ms;
    return;
  }

  br_stats_apply_token_diff(&g_ctx.totals, delta_total, delta_input, delta_output,
                            delta_cached, delta_cache_create, delta_reasoning, delta_cost);
  br_stats_source_bucket *bucket = br_stats_find_or_create_source(&g_ctx, source);
  if (bucket) {
    br_stats_apply_token_diff(&bucket->totals, delta_total, delta_input, delta_output,
                              delta_cached, delta_cache_create, delta_reasoning, delta_cost);
    /* stamp activity so the dashboard can follow the agent the user is
       actually using right now (vs the all-time-token leader). */
    bucket->last_update_ms = now_ms;
  }
  g_ctx.updated_at_ms = now_ms;
  g_ctx.dirty = true;
}

static void br_stats_format_compact_count(long long n, char *out, size_t out_size) {
  if (n < 1000) {
    snprintf(out, out_size, "%lld", n);
    return;
  }
  if (n < 1000000) {
    snprintf(out, out_size, "%.1fK", n / 1000.0);
    return;
  }
  if (n < 1000000000LL) {
    snprintf(out, out_size, "%.2fM", n / 1000000.0);
    return;
  }
  snprintf(out, out_size, "%.2fB", n / 1000000000.0);
}

static long long br_stats_clamp_nonneg(long long v) {
  return v < 0 ? 0 : v;
}

static const char *br_stats_source_display_label(const char *source) {
  if (!source || !*source) return "Codex";
  if (strcmp(source, "codex") == 0) return "Codex";
  if (strcmp(source, "claude-code") == 0) return "Claude";
  if (strcmp(source, "claude") == 0) return "Claude";
  if (strcmp(source, "cursor") == 0) return "Cursor";
  if (strcmp(source, "chatgpt") == 0) return "ChatGPT";
  if (strcmp(source, "openclaw") == 0) return "OpenClaw";
  return source;
}

static const br_stats_source_bucket *br_stats_top_source(void) {
  const br_stats_source_bucket *best = NULL;
  for (size_t i = 0; i < g_ctx.source_count; i += 1) {
    const br_stats_source_bucket *candidate = &g_ctx.sources[i];
    if (candidate->totals.total_tokens <= 0) continue;
    if (!best || candidate->totals.total_tokens > best->totals.total_tokens) {
      best = candidate;
    }
  }
  return best;
}

/* Pick the source the user is most likely watching right now:
   bucket with the most recent last_update_ms within a freshness window
   (5 minutes). Falls back to the all-time-tokens leader if no recent
   activity. This is what makes the dashboard "follow Claude Code" when
   the user switches agents mid-day instead of staying pinned to whoever
   has historically chewed more tokens. */
static const br_stats_source_bucket *br_stats_active_source(long long now_ms) {
  const long long ACTIVE_WINDOW_MS = 5LL * 60LL * 1000LL;
  const br_stats_source_bucket *most_recent = NULL;
  for (size_t i = 0; i < g_ctx.source_count; i += 1) {
    const br_stats_source_bucket *c = &g_ctx.sources[i];
    if (c->last_update_ms <= 0) continue;
    if (now_ms > 0 && (now_ms - c->last_update_ms) > ACTIVE_WINDOW_MS) continue;
    if (!most_recent || c->last_update_ms > most_recent->last_update_ms) {
      most_recent = c;
    }
  }
  return most_recent ? most_recent : br_stats_top_source();
}

size_t runtime_stats_render_display(char *out, size_t out_size) {
  if (!out || out_size == 0) return 0;
  size_t used = 0;
  out[0] = '\0';

  long long total = br_stats_clamp_nonneg(g_ctx.totals.total_tokens);
  long long per_lunch = g_ctx.tokens_per_coffee > 0 ? g_ctx.tokens_per_coffee : 350000LL;
  double lunch_count = per_lunch > 0 ? (double) total / (double) per_lunch : 0.0;
  char total_str[32];
  char lunch_str[32];
  /* Prefer the currently-active agent (most recent token update within 5m)
     over the all-time-tokens leader. Falls back gracefully if no activity. */
  const br_stats_source_bucket *top = br_stats_active_source(g_ctx.updated_at_ms);
  const char *agent = br_stats_source_display_label(top ? top->source : "codex");

  br_stats_format_compact_count(total, total_str, sizeof(total_str));
  snprintf(lunch_str, sizeof(lunch_str), "%.1f", lunch_count);

  br_snprintf_append(out, out_size, &used, "STATS_DASHBOARD_V1");
  br_snprintf_append(out, out_size, &used, "\nagent=%s", agent);
  br_snprintf_append(out, out_size, &used, "\neyebrow=等价于购买了");
  br_snprintf_append(out, out_size, &used, "\nlunch=%s", lunch_str);
  br_snprintf_append(out, out_size, &used, "\nheadline=约 %s 顿工作午餐", lunch_str);
  br_snprintf_append(out, out_size, &used, "\nmetricTitle=今日累计 Token");
  br_snprintf_append(out, out_size, &used, "\nmetricValue=%s", total_str);
  br_snprintf_append(out, out_size, &used, "\nmetricUnit=TOKEN");
  br_snprintf_append(out, out_size, &used, "\nalerts=%lld", total > 0 ? 1LL : 0LL);
  br_snprintf_append(out, out_size, &used, "\ncompleted=%lld", total > 0 ? 1LL : 0LL);

  if (g_ctx.totals.input_tokens > 0
      || g_ctx.totals.output_tokens > 0
      || g_ctx.totals.cached_input_tokens > 0) {
    char in_str[32], out_str[32], cache_str[32];
    br_stats_format_compact_count(br_stats_clamp_nonneg(g_ctx.totals.input_tokens), in_str, sizeof(in_str));
    br_stats_format_compact_count(br_stats_clamp_nonneg(g_ctx.totals.output_tokens), out_str, sizeof(out_str));
    br_stats_format_compact_count(br_stats_clamp_nonneg(g_ctx.totals.cached_input_tokens), cache_str, sizeof(cache_str));
    br_snprintf_append(out, out_size, &used, "\nbreakdown=输入 %s · 输出 %s · 缓存 %s",
                       in_str, out_str, cache_str);
  }

  size_t n = g_ctx.source_count;
  size_t order[BR_STATS_MAX_SOURCES];
  for (size_t i = 0; i < n; i += 1) order[i] = i;
  for (size_t i = 0; i + 1 < n; i += 1) {
    size_t best = i;
    for (size_t j = i + 1; j < n; j += 1) {
      if (g_ctx.sources[order[j]].totals.total_tokens
          > g_ctx.sources[order[best]].totals.total_tokens) {
        best = j;
      }
    }
    if (best != i) {
      size_t tmp = order[i];
      order[i] = order[best];
      order[best] = tmp;
    }
  }
  size_t shown = 0;
  for (size_t i = 0; i < n && shown < 3; i += 1) {
    long long src_total = g_ctx.sources[order[i]].totals.total_tokens;
    if (src_total <= 0) continue;
    char src_str[32];
    br_stats_format_compact_count(src_total, src_str, sizeof(src_str));
    if (shown == 0) {
      br_snprintf_append(out, out_size, &used, "\nsources=%s %s",
                         g_ctx.sources[order[i]].source, src_str);
    } else {
      br_snprintf_append(out, out_size, &used, " · %s %s",
                         g_ctx.sources[order[i]].source, src_str);
    }
    shown += 1;
  }

  return used;
}

static bool br_stats_write_today_json(br_stats_ctx *ctx) {
  char buffer[BR_MAX_JSON];
  size_t used = 0;
  buffer[0] = '\0';
  br_snprintf_append(buffer, sizeof(buffer), &used, "{\"dateStamp\":\"%s\",", ctx->date_stamp);
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"updatedAtMs\":%lld,", ctx->updated_at_ms);
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"tzOffsetSec\":%d,", ctx->tz_offset_sec);
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"totals\":{");
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"totalTokens\":%lld,", ctx->totals.total_tokens);
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"inputTokens\":%lld,", ctx->totals.input_tokens);
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"outputTokens\":%lld,", ctx->totals.output_tokens);
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"cachedInputTokens\":%lld,", ctx->totals.cached_input_tokens);
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"cacheCreationInputTokens\":%lld,",
                     ctx->totals.cache_creation_input_tokens);
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"reasoningOutputTokens\":%lld,",
                     ctx->totals.reasoning_output_tokens);
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"estimatedCostUsd\":%.4f},",
                     ctx->totals.estimated_cost_usd);
  br_snprintf_append(buffer, sizeof(buffer), &used, "\"bySource\":[");
  bool first = true;
  for (size_t i = 0; i < ctx->source_count; i += 1) {
    const br_stats_source_bucket *b = &ctx->sources[i];
    if (b->totals.total_tokens <= 0) continue;
    if (!first) {
      br_snprintf_append(buffer, sizeof(buffer), &used, ",");
    }
    first = false;
    br_snprintf_append(buffer, sizeof(buffer), &used, "{\"source\":\"");
    br_json_escape_append(buffer, sizeof(buffer), &used, b->source);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\",\"totalTokens\":%lld,", b->totals.total_tokens);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"inputTokens\":%lld,", b->totals.input_tokens);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"outputTokens\":%lld,", b->totals.output_tokens);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"cachedInputTokens\":%lld,",
                       b->totals.cached_input_tokens);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"cacheCreationInputTokens\":%lld,",
                       b->totals.cache_creation_input_tokens);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"reasoningOutputTokens\":%lld,",
                       b->totals.reasoning_output_tokens);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"estimatedCostUsd\":%.4f}",
                       b->totals.estimated_cost_usd);
  }
  br_snprintf_append(buffer, sizeof(buffer), &used, "]}");
  return br_atomic_write_text(ctx->today_path, buffer);
}

static bool br_stats_write_sessions_json(br_stats_ctx *ctx) {
  char buffer[BR_MAX_JSON];
  size_t used = 0;
  buffer[0] = '\0';
  br_snprintf_append(buffer, sizeof(buffer), &used, "{\"sessions\":[");
  for (size_t i = 0; i < ctx->session_count; i += 1) {
    const br_stats_session_entry *e = &ctx->sessions[i];
    if (i > 0) {
      br_snprintf_append(buffer, sizeof(buffer), &used, ",");
    }
    br_snprintf_append(buffer, sizeof(buffer), &used, "{\"source\":\"");
    br_json_escape_append(buffer, sizeof(buffer), &used, e->source);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\",\"sessionKey\":\"");
    br_json_escape_append(buffer, sizeof(buffer), &used, e->session_key);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\",\"prevTotal\":%lld,", e->prev_total);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"prevInput\":%lld,", e->prev_input);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"prevOutput\":%lld,", e->prev_output);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"prevCached\":%lld,", e->prev_cached);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"prevCacheCreate\":%lld,", e->prev_cache_create);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"prevReasoning\":%lld,", e->prev_reasoning);
    br_snprintf_append(buffer, sizeof(buffer), &used, "\"prevCost\":%.4f}", e->prev_cost);
  }
  br_snprintf_append(buffer, sizeof(buffer), &used, "]}");
  return br_atomic_write_text(ctx->sessions_path, buffer);
}

bool runtime_stats_flush(void) {
  if (!g_ctx.initialized) return false;
  if (!g_ctx.dirty) return true;
  bool ok = br_stats_write_today_json(&g_ctx);
  ok = br_stats_write_sessions_json(&g_ctx) && ok;
  char display[1024];
  runtime_stats_render_display(display, sizeof(display));
  ok = br_atomic_write_text(g_ctx.display_path, display) && ok;
  if (ok) {
    g_ctx.dirty = false;
  }
  return ok;
}

void runtime_stats_shutdown(void) {
  if (g_ctx.initialized && g_ctx.dirty) {
    runtime_stats_flush();
  }
  memset(&g_ctx, 0, sizeof(g_ctx));
}
