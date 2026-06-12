#ifndef BOARD_RUNTIME_STATS_H
#define BOARD_RUNTIME_STATS_H

#include <stdbool.h>
#include <stddef.h>

#include "runtime_protocol.h"

/* runtime_stats: device-side daily token aggregation.
 *
 * 数据流:
 *   1. board_server 收到 desk/<id>/state/<source> 后调用 br_bridge_state_from_message
 *      解出 br_bridge_state_update。如果 update.has_token_usage 为真，再调
 *      runtime_stats_ingest 把 tokenUsage 累加进当日总量。
 *   2. tokenUsage.totalTokens 在 pet-claw 端是 session 内的单调累加值；本模块对每个
 *      (source, sessionKey 或 sessionId) 维持上次记录的 prev，只把 cur > prev 的差量
 *      加进 today。session 重启 / cur < prev 就把它当成新会话起点，不产生 delta。
 *   3. ingest 内部维护 dirty 标志；调用 runtime_stats_flush 把 today.json + sessions.json
 *      + .stats-display 三个文件 atomic-write 到 root_dir。
 *   4. 跨天检测在 ingest 时进行：如果 today.dateStamp != localDate(now_ms) 就把 today.json
 *      归档到 stats/YYYY-MM-DD.json，重置 today。
 *
 * 时区与午餐换算:
 *   - tz_offset_sec：本地时区相对 UTC 的偏移秒数，默认 +28800（北京时间）。
 *   - tokens_per_coffee：每顿工作午餐的 token 数；历史命名沿用，默认 350,000。
 *
 * 文件协议:
 *   <root>/stats/today.json     当日聚合（机器可读）
 *   <root>/stats/sessions.json  每个 (source,session) 的 prev 值，重启可恢复
 *   <root>/stats/YYYY-MM-DD.json 历史归档
 *   <root>/.stats-display        runtime_stats 写入的 STATS_DASHBOARD_V1 payload，由 fb-speech-overlay 渲染
 */
int runtime_stats_init(
  const char *root_dir,
  long long tokens_per_coffee,
  int tz_offset_sec,
  long long now_ms
);
void runtime_stats_ingest(const br_bridge_state_update *update, long long now_ms);
size_t runtime_stats_render_display(char *out, size_t out_size);
bool runtime_stats_flush(void);
void runtime_stats_shutdown(void);

#endif
