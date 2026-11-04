"""Compile real renderer timing paths with deterministic clocks; no device I/O."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/pet_p4_renderer.c"


def function(source, name):
    """Extract original source while merging brace depth of every #if branch.

    Both capability paths remain visible to the original transaction assertions.
    An inconsistent branch fails rather than swallowing subsequent functions.
    """
    import re
    lexical = re.sub(r'''//[^\n]*|/\*[\s\S]*?\*/|"(?:\\[\s\S]|[^"\\])*"|'(?:\\[\s\S]|[^'\\])*' ''',
                     lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), source,
                     flags=re.VERBOSE)
    match = re.search(r'(?:static )?[\w *]+\b' + re.escape(name) + r'\s*\([^;]*?\)\s*\{', lexical)
    assert match, name
    brace = lexical.index('{', match.start())
    depth, branches = 0, []
    for token in re.finditer(r'^[ \t]*#[^\n]*|[{}]', lexical[brace:], re.MULTILINE):
        value = token[0]
        if value.lstrip().startswith('#'):
            directive = re.match(r'\s*#\s*(\w+)', value)
            assert directive, (name, value)
            kind = directive[1]
            if kind in ('if', 'ifdef', 'ifndef'):
                branches.append([depth, [], False])
            elif kind in ('elif', 'else'):
                assert branches and not branches[-1][2], (name, kind)
                branch = branches[-1]
                branch[1].append(depth)
                depth = branch[0]
                branch[2] = kind == 'else'
            elif kind == 'endif':
                assert branches, (name, kind)
                base, ends, has_else = branches.pop()
                ends.append(depth)
                if not has_else:
                    ends.append(base)
                assert len(set(ends)) == 1, (name, 'conditional brace mismatch', ends)
                depth = ends[0]
        else:
            depth += 1 if value == '{' else -1
            assert depth >= 0, (name, 'negative brace depth')
            if depth == 0 and not branches:
                return source[match.start():brace + token.end()]
    raise AssertionError((name, 'unterminated function or conditional'))


def test_conditional_extraction_keeps_all_transaction_branches_and_exact_boundary():
    import pytest
    transaction = r'''bool transaction(void) {
#if FEATURE
#if INNER
  if (first) {
#else
  if (inner_fallback) {
#endif
#elif SECOND
  if (second) {
#else
  if (fallback) {
#endif
#if NESTED
    const char *literal = "} // not a closing brace";
    const char open = '{';
    const char close = '}';
    /* } neither is this comment { */
    write_resource();
#else
    preserve_resource();
#endif
  }
  return true;
}'''
    source = transaction + '\nvoid next_function(void) { destroy_live_state(); }\n'
    extracted = function(source, 'transaction')
    assert extracted == transaction
    assert all(word in extracted for word in ('write_resource();', 'preserve_resource();', '#elif SECOND'))
    assert 'destroy_live_state' not in extracted
    with pytest.raises(AssertionError, match='conditional brace mismatch'):
        function(transaction.replace('if (fallback) {', 'fallback();'), 'transaction')



def run_c(code):
    with tempfile.TemporaryDirectory() as tmp:
        src, binary = Path(tmp) / "trace.c", Path(tmp) / "trace"
        src.write_text(code)
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
            str(src), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


COMMON = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "pet_p4_render_trace.h"
'''


def test_numeric_trace_reset_saturation_and_reason_flags():
    run_c(COMMON + r'''
int main(void) {
  pet_p4_render_trace_t trace;
  memset(&trace, 0xff, sizeof(trace));
  pet_p4_render_trace_begin(&trace, 4, "app");
  assert(trace.frame == 4 && trace.page == PET_RENDER_PAGE_APP);
  assert(!trace.cache_hit && !trace.reset_us && !trace.backend && !trace.fallback);
  pet_p4_render_trace_add(&trace.reset_us, UINT32_MAX - 7);
  pet_p4_render_trace_add(&trace.reset_us, 8);
  assert(trace.reset_us == UINT32_MAX);
  pet_p4_render_trace_add(&trace.reset_us, 1);
  assert(trace.reset_us == UINT32_MAX);
  pet_p4_render_trace_begin(&trace, 0, NULL);
  assert(trace.frame == 0 && trace.page == PET_RENDER_PAGE_MAIN && !trace.reset_us);
  pet_p4_render_trace_begin(&trace, 5, "other");
  assert(trace.page == PET_RENDER_PAGE_OTHER);
  pet_p4_render_trace_begin(&trace, 6, "components");
  assert(trace.page == PET_RENDER_PAGE_COMPONENTS);
  pet_p4_render_trace_begin(&trace, 7, "touch_test");
  assert(trace.page == PET_RENDER_PAGE_TOUCH_TEST);
  assert(pet_p4_render_restart_reason(false, false, true) == PET_RENDER_RESTART_COLD);
  assert(pet_p4_render_restart_reason(true, true, false) == 0);
  assert(pet_p4_render_restart_reason(true, false, false) == PET_RENDER_RESTART_PATH);
  assert(pet_p4_render_restart_reason(true, true, true) == PET_RENDER_RESTART_REWIND);
  assert(pet_p4_render_restart_reason(true, false, true)
    == (PET_RENDER_RESTART_PATH | PET_RENDER_RESTART_REWIND));
  char output[768];
  memset(&trace, 0xff, sizeof(trace));
  int size = snprintf(output, sizeof(output), PET_RENDER_TRACE_FORMAT, PET_RENDER_TRACE_VALUES(trace));
  assert(size > 0 && size < 400);
  assert(strstr(output, "frame=4294967295") && strstr(output, "fallback=4294967295"));
  return 0;
}
''')


def test_context_keeps_coherent_frame_bounds_and_two_numeric_lanes():
    run_c(COMMON + r'''
int main(void) {
  pet_p4_render_trace_t trace;
  pet_p4_render_trace_begin(&trace, 8, "main");
  trace.start_us = 100;
  pet_widget_trace_snapshot_t snapshot = {0};
  snapshot.lanes[0].seq = 123;
  snapshot.lanes[0].kind = PET_WTRACE_DELETE;
  snapshot.lanes[0].current_stage = PET_WTRACE_FS_REMOVE;
  snapshot.lanes[0].active = 1;
  snapshot.lanes[1].seq = 321;
  snapshot.lanes[1].kind = PET_WTRACE_SAVE_FLUSH;
  snapshot.lanes[1].current_stage = PET_WTRACE_FS_CLOSE;
  snapshot.lanes[1].active = 1;
  pet_p4_render_trace_observe(&trace, &snapshot, false);
  snapshot.lanes[0].active = 0;
  snapshot.lanes[0].last_completed_seq = 123;
  snapshot.lanes[1].current_stage = PET_WTRACE_PUBLISH;
  pet_p4_render_trace_observe(&trace, &snapshot, true);
  trace.end_us = 220;
  assert(trace.operation_before[0].phase == (65536U | (PET_WTRACE_DELETE << 8) | PET_WTRACE_FS_REMOVE));
  assert(trace.operation_after[0].phase == ((PET_WTRACE_DELETE << 8) | PET_WTRACE_FS_REMOVE));
  assert(trace.completed_after[0] == 123 && trace.completed_after[1] == 0);
  assert(trace.operation_before[1].seq == 321 && trace.operation_after[1].seq == 321);
  pet_p4_render_peak_t peak = {0};
  pet_p4_render_peak_record(&peak, &trace, 120, 0, 0, 0, 0, 0, 0, 0);
  pet_p4_render_trace_begin(&trace, 9, "app");
  assert(!trace.start_us && !trace.end_us && !trace.operation_before[0].seq);
  assert(peak.trace.start_us == 100 && peak.trace.end_us == 220);
  assert(peak.trace.operation_before[0].seq == 123 && peak.trace.completed_after[0] == 123);
  char line[768];
  memset(&trace, 0xff, sizeof(trace));
  int length = snprintf(line, sizeof(line), PET_RENDER_CONTEXT_FORMAT, PET_RENDER_CONTEXT_VALUES(trace));
  assert(length > 0 && length < 600);
  assert(strstr(line, "wb_seq=18446744073709551615") && strstr(line, "s_done=18446744073709551615"));
  length = snprintf(line, sizeof(line), PET_RENDER_PPA_FORMAT, PET_RENDER_PPA_VALUES(trace));
  assert(length > 0 && length < 400);
  assert(strstr(line, "convert_api=4294967295") && strstr(line, "rotate_flags=4294967295"));
  return 0;
}
''')


def test_actual_perf_window_logs_same_frame_peak_and_preserves_throttling():
    source = SOURCE.read_text()
    start = source.index("typedef struct {\n  uint32_t samples;")
    end = source.index("} pet_p4_render_perf_t;", start) + len("} pet_p4_render_perf_t;")
    code = COMMON + r'''
#define PET_P4_RENDER_PERF_WINDOW 100U
#define PET_P4_RENDER_PERF_GAP_RESET_US 2000000LL
#define PET_P4_RENDER_FRAME_BUDGET_US 66667U
#include "pet_p4_render_lifetime.h"
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define ESP_LOGW(tag, ...) capture(__VA_ARGS__)
#define ESP_LOGI(tag, ...) capture(__VA_ARGS__)
#define ESP_LOGD(tag, ...) capture(__VA_ARGS__)
static char logs[12][1024];
static unsigned log_count;
static void capture(const char *format, ...) {
  assert(log_count < 12);
  va_list args; va_start(args, format);
  int length = vsnprintf(logs[log_count++], 1024, format, args);
  va_end(args);
  /* Leave space for the transport's normal log prefix as well. */
  assert(length > 0 && length < 850);
}
static struct {
  uint32_t last_us, p95_us, max_us, last_decode_us;
  uint32_t window_samples, submitted_frames, unchanged_frames;
  pet_render_lifetime_t lifetime;
} g_metrics;
static uint32_t g_last_decode_us, g_last_h264_pack_us, g_last_h264_convert_us;
static uint32_t g_lcd_submitted, g_lcd_skipped;
static int64_t g_last_slow_render_log_us;
static int64_t g_last_frame_gap_log_us;
static pet_p4_render_trace_t g_render_trace;
''' + source[start:end] + r'''
static pet_p4_render_perf_t g_render_perf;
''' + "\n".join(function(source, name) for name in (
        "elapsed_us_clamped", "perf_average_tenths_ms", "record_render_performance",
    )) + r'''
int main(void) {
  assert(elapsed_us_clamped(INT64_MIN,INT64_MAX)==0);
  assert(elapsed_us_clamped(0,INT64_MAX)==UINT32_MAX);
  assert(elapsed_us_clamped(10,9)==0);
  int64_t started = 1000000;
  for (unsigned i = 1; i <= 100; i++) {
    uint32_t total = i == 1 ? 110000 : i == 2 ? 216900 : 10000;
    pet_p4_render_trace_begin(&g_render_trace, i, i <= 2 ? "main" : "app");
    g_render_trace.reset_us = i == 2 ? 100011 : 0;
    g_render_trace.cache_hit = i <= 2 ? PET_RENDER_CACHE_HIT : 0;
    g_render_trace.restart_reason = i == 2 ? PET_RENDER_RESTART_REWIND : 0;
    g_last_decode_us = i == 2 ? 44200 : 0;
    g_last_h264_pack_us = i == 2 ? 4600 : 0;
    g_last_h264_convert_us = i == 2 ? 400 : 0;
    record_render_performance(started, started + total, i == 2 ? 197000 : 0,
      i == 2 ? 18000 : 9000, 1000, 900);
    started += total + 1000;
  }
  assert(log_count == 3); /* One throttled slow log, existing summary, peak. */
  assert(strstr(logs[0], "P4 slow render total=110.0ms") && strstr(logs[0], "frame=1 "));
  assert(strstr(logs[1], "P4 render perf ") && strstr(logs[1], "max=216.9ms"));
  assert(strstr(logs[2], "P4 render peak total_us=216900 asset_us=197000 decode_us=44200"));
  assert(strstr(logs[2], "frame=2 page=1") && strstr(logs[2], "reset_us=100011"));
  assert(strstr(logs[2], "cacheHit=2 restartReason=4"));
  assert(g_metrics.max_us == 216900 && g_metrics.window_samples == 100);
  assert(g_metrics.lifetime.samples==100&&g_metrics.lifetime.max_us==216900);
  assert(g_metrics.lifetime.peak_frame==2&&g_metrics.lifetime.over_budget==2);
#if PET_P4_RETAIN_PEAK_PHASES
  assert(g_metrics.lifetime.asset_us==197000&&g_metrics.lifetime.decode_us==44200);
  assert(g_metrics.lifetime.pack_us==4600&&g_metrics.lifetime.convert_us==400);
  assert(g_metrics.lifetime.overlay_us==18000&&g_metrics.lifetime.rotate_us==1000&&g_metrics.lifetime.lcd_us==900);
#endif
  assert(g_metrics.lifetime.completed_windows==1&&g_metrics.lifetime.window_end_frame==100);
  assert(g_render_perf.samples == 0 && !g_render_perf.peak.valid);
  /* A later app frame cannot inherit the main frame's stages or backend. */
  pet_p4_render_trace_begin(&g_render_trace, 101, "app");
  g_last_decode_us = g_last_h264_pack_us = g_last_h264_convert_us = 0;
  record_render_performance(4000000, 4150000, 0, 149000, 1000, 0);
  assert(log_count == 4 && strstr(logs[3], "page=2") && strstr(logs[3], "reset_us=0"));
  assert(g_render_perf.peak.trace.frame == 101 && g_render_perf.peak.asset_us == 0);
  /* An idle gap starts a fresh window; >2s samples still follow legacy exclusion. */
  pet_p4_render_trace_begin(&g_render_trace, 102, "components");
  record_render_performance(8000000, 8010000, 0, 9000, 1000, 0);
  assert(g_render_perf.samples == 1 && g_render_perf.peak.trace.frame == 102);
  record_render_performance(9000000, 11000001, 0, 0, 0, 0);
  assert(!g_render_perf.peak.valid && g_render_perf.samples == 0);
  assert(g_metrics.max_us==216900); /* legacy window alone cannot prove no stall */
  assert(g_metrics.lifetime.max_us==2000001&&g_metrics.lifetime.peak_frame==102);
  assert(g_metrics.lifetime.peak_start_us==9000000&&g_metrics.lifetime.peak_end_us==11000001);
#if PET_P4_RETAIN_PEAK_PHASES
  assert(g_metrics.lifetime.asset_us==0&&g_metrics.lifetime.decode_us==0);
  assert(g_metrics.lifetime.overlay_us==0&&g_metrics.lifetime.rotate_us==0&&g_metrics.lifetime.lcd_us==0);
#endif
  assert(g_metrics.lifetime.window_resets==2&&g_metrics.lifetime.over_budget==4);
  /* Equal totals retain one coherent earliest frame rather than mixing stages. */
  pet_p4_render_peak_t peak = {0};
  pet_p4_render_peak_record(&peak, &g_render_trace, 5, 1, 2, 3, 4, 5, 6, 7);
  pet_p4_render_trace_begin(&g_render_trace, 103, "main");
  pet_p4_render_peak_record(&peak, &g_render_trace, 5, 9, 9, 9, 9, 9, 9, 9);
  assert(peak.trace.frame == 102 && peak.asset_us == 1 && peak.lcd_us == 7);
  /* A long inter-frame stall remains visible even when the next frame itself
   * takes only 10 ms; separate throttling must not change the timing metric. */
  pet_p4_render_trace_begin(&g_render_trace, 104, "main");
  g_render_trace.previous_end_us=12000000;
  g_render_trace.start_us=12500000;g_render_trace.end_us=12510000;
  g_render_trace.gap_us=500000;
  unsigned before_gap=log_count;
  record_render_performance(12500000,12510000,0,9000,1000,0);
  assert(log_count==before_gap+2 && g_metrics.last_us==10000);
  assert(strstr(logs[before_gap],"P4 frame gap frame=104 gap_us=500000"));
  assert(strstr(logs[before_gap+1],"prev_end_us=12000000 gap_us=500000"));
  pet_p4_render_trace_begin(&g_render_trace,105,"main");
  g_render_trace.gap_us=600000;
  record_render_performance(12600000,12610000,0,9000,1000,0);
  assert(log_count==before_gap+2); /* bounded diagnostic rate */
  assert(g_metrics.lifetime.samples==105&&g_metrics.lifetime.gap_over_budget==2);
  assert(g_metrics.lifetime.max_gap_us==600000&&g_metrics.lifetime.gap_peak_frame==105);
  assert(g_metrics.lifetime.max_us==2000001); /* newer fast window never clears peak */
  g_metrics.lifetime.samples=UINT32_MAX;g_metrics.lifetime.over_budget=UINT32_MAX;
  pet_render_lifetime_record(&g_metrics.lifetime,100000,0,106,0,-1,0,66667,100000);
  assert(g_metrics.lifetime.samples==UINT32_MAX&&g_metrics.lifetime.over_budget==UINT32_MAX&&g_metrics.lifetime.invalid_clock==1);
  return 0;
}
'''
    run_c(code)
    run_c('#define PET_P4_RETAIN_PEAK_PHASES 1\n'+code)


def test_actual_h264_reset_copy_init_timing_includes_failures_not_external_reset():
    source = SOURCE.read_text()
    declarations = source[source.index("static esp_h264_dec_handle_t g_h264_decoder;"):source.index("static uint8_t *g_h264_ppa_input;")]
    functions = "\n".join(function(source, name) for name in (
        "reset_h264_decoder", "reset_h264_decoder_traced", "reset_h264_decoder_measured", "start_h264_decoder_role", "decode_next_h264_frame_role", "start_h264_decoder", "decode_next_h264_frame",
    ))
    harness = (ROOT / "tests/p4_h264_replay_test.c").read_text()
    harness = harness[:harness.index("int main(void)")]
    harness = harness.replace("static unsigned allocations;", "static unsigned allocations;\nstatic int64_t clock_us;")
    harness = harness.replace("allocations++;", "allocations++; clock_us += 31;")
    harness = harness.replace("(void)dec; return 0; }", "(void)dec; clock_us += 11; return 0; }", 1)
    harness = harness.replace("static int esp_h264_dec_del(void *dec) { (void)dec; return 0; }", "static int esp_h264_dec_del(void *dec) { (void)dec; clock_us += 100000; return 0; }")
    harness = harness.replace("(void)cfg; *dec", "(void)cfg; clock_us += 17; *dec")
    harness = harness.replace("(void)dec; return fail_open", "(void)dec; clock_us += 19; return fail_open")
    harness = harness.replace("  (void)dec;\n", "  (void)dec; clock_us += 44200;\n")
    harness = harness.replace("static int64_t esp_timer_get_time(void) { return 1; }", "static int64_t esp_timer_get_time(void) { return clock_us; }")
    harness = harness.replace("static uint32_t elapsed_us_clamped(int64_t start, int64_t end) { (void)start; (void)end; return 1; }", function(source, "elapsed_us_clamped"))
    harness = harness.replace("/* RENDERER_IMPLEMENTATION */", declarations + "\n" + functions)
    run_c(harness + r'''
int main(void) {
  static const uint8_t stream[32] = {0,0,1,9,0,0,1,5};
  pet_p4_render_trace_begin(&g_render_trace, 1, "main");
  assert(start_h264_decoder("idle", stream, sizeof(stream)));
  assert(g_render_trace.reset_us == 0 && g_render_trace.copy_us == 31 && g_render_trace.init_us == 36);
  assert(decode_next_h264_frame(2, 2) && g_last_decode_us == 44200);
  pet_p4_render_trace_begin(&g_render_trace, 2, "main");
  assert(start_h264_decoder("idle", stream, sizeof(stream)));
  assert(g_render_trace.reset_us == 100011 && g_render_trace.copy_us == 0 && g_render_trace.init_us == 36);
  reset_h264_decoder(); /* Out-of-frame reset must leave the last trace untouched. */
  assert(g_render_trace.reset_us == 100011);
  pet_p4_render_trace_begin(&g_render_trace, 3, "main");
  fail_open = true;
  assert(!start_h264_decoder("failed", stream, sizeof(stream)));
  assert(g_render_trace.init_us == 36 && g_render_trace.reset_us == 100000);
  fail_open = false; fail_new = true;
  pet_p4_render_trace_begin(&g_render_trace, 4, "main");
  assert(!start_h264_decoder("failed", stream, sizeof(stream)));
  assert(g_render_trace.init_us == 17 && !g_render_trace.reset_us);
  fail_new = false;
  free(g_h264_work_stream); g_h264_work_stream = NULL; g_h264_work_capacity = 0;
  fail_alloc = true;
  pet_p4_render_trace_begin(&g_render_trace, 5, "main");
  assert(!start_h264_decoder("oom", stream, sizeof(stream)));
  assert(g_render_trace.copy_us == 31 && !g_render_trace.init_us && !g_render_trace.reset_us);
  reset_h264_decoder_measured();assert(!g_render_trace.reset_us);
  return 0;
}
''')


def test_actual_h264_render_warm_restart_and_failed_stage_boundaries():
    source = SOURCE.read_text()
    run_c(COMMON + r'''
#define PET_P4_H264_MAX_DECODE_FRAMES_PER_RENDER 1U
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
typedef struct { int frames, width, height, duration_ms; } pet_p4_asset_selection_t;
typedef struct { const uint8_t *bytes; size_t size; bool single_slice_valid; } pet_p4_asset_cache_entry_t;
static pet_p4_render_trace_t g_render_trace;
static char g_h264_fs_path[80];
static void *g_h264_decoder;
static bool g_logged_h264_decoder;
static int g_h264_decoded_frame = -1;
static uint8_t output[6];
static uint8_t *g_h264_output;
static uint32_t g_h264_output_size, g_last_decode_us, g_last_h264_pack_us, g_last_h264_convert_us;
static int64_t clock_us;
static bool fail_decode, ppa_supported = true, fail_pack, fail_ppa, fail_layout;
static unsigned starts, decodes;
static int64_t esp_timer_get_time(void) { return clock_us; }
static void vTaskDelay(unsigned ticks) { assert(ticks == 1); }
static bool start_h264_decoder(const char *path, const uint8_t *bytes, size_t size) {
  assert(bytes && size); starts++;
  g_h264_decoder = output; g_h264_decoded_frame = -1;
  snprintf(g_h264_fs_path, sizeof(g_h264_fs_path), "%s", path);
  return true;
}
static bool decode_next_h264_frame(int width, int height) {
  assert(width == 2 && height == 2); decodes++; clock_us += 43; g_last_decode_us += 40;
  if (fail_decode) return false;
  g_h264_decoded_frame++; g_h264_output = output; g_h264_output_size = sizeof(output);
  return true;
}
static bool resolve_h264_i420_layout(int width, int height, uint32_t size, int *stride, int *storage) {
  assert(width == 2 && height == 2 && size == 6); clock_us += 2;
  *stride = width; *storage = height; return !fail_layout;
}
static bool h264_frame_supports_ppa_yuv420(int width, int storage) {
  assert(width == 2 && storage == 2); return ppa_supported;
}
static bool pack_h264_i420_for_ppa(const uint8_t *bytes, int w, int h, int stride, int storage, const uint8_t **packed) {
  assert(bytes && w == 2 && h == 2 && stride == 2 && storage == 2);
  clock_us += 3; *packed = output; return !fail_pack;
}
static bool scale_h264_frame_ppa(const uint8_t *bytes, int w, int h, int storage) {
  assert(bytes && w == 2 && h == 2 && storage == 2);
  clock_us += 5; g_last_h264_convert_us += 5; return !fail_ppa;
}
static void scale_h264_frame_cpu(const uint8_t *bytes, int w, int h, int stride, int storage) {
  assert(bytes && w == 2 && h == 2 && stride == 2 && storage == 2); clock_us += 7;
}
''' + function(source, "elapsed_us_clamped") + "\n" + function(source, "render_h264_asset_frame") + r'''
static void begin(unsigned frame) {
  pet_p4_render_trace_begin(&g_render_trace, frame, "main");
  g_last_decode_us = g_last_h264_pack_us = g_last_h264_convert_us = 0;
}
int main(void) {
  pet_p4_asset_selection_t asset = {3, 2, 2, 300};
  pet_p4_asset_cache_entry_t stream = {output, sizeof(output), true};
  begin(1);
  assert(render_h264_asset_frame(&asset, "idle", &stream, 0));
  assert(g_render_trace.validate_us == 0 && g_render_trace.restart_reason == PET_RENDER_RESTART_COLD);
  assert(g_render_trace.decode_outer_us == 43 && g_last_decode_us == 40);
  assert(g_render_trace.scale_us == 10 && g_last_h264_pack_us == 3 && g_last_h264_convert_us == 5);
  assert(!g_render_trace.fallback && starts == 1 && decodes == 1);
  begin(2);
  assert(render_h264_asset_frame(&asset, "idle", &stream, 0));
  assert(!g_render_trace.validate_us && !g_render_trace.restart_reason && !g_render_trace.decode_outer_us);
  assert(g_render_trace.scale_us == 10 && starts == 1 && decodes == 1);
  begin(3);
  assert(render_h264_asset_frame(&asset, "idle", &stream, 150));
  assert(!g_render_trace.restart_reason && g_render_trace.decode_outer_us == 43);
  begin(4);
  assert(render_h264_asset_frame(&asset, "idle", &stream, 0));
  assert(g_render_trace.restart_reason == PET_RENDER_RESTART_REWIND && starts == 2);
  begin(5);
  assert(render_h264_asset_frame(&asset, "other", &stream, 250));
  assert(g_render_trace.restart_reason == PET_RENDER_RESTART_PATH && g_render_trace.validate_us == 0);
  assert(g_h264_decoded_frame == 0); /* Original one-frame work bound is intact. */
  begin(6); fail_decode = true;
  assert(!render_h264_asset_frame(&asset, "other", &stream, 150));
  assert(g_render_trace.decode_outer_us == 43 && !g_render_trace.scale_us);
  begin(7); fail_decode = false; stream.single_slice_valid = false;
  assert(!render_h264_asset_frame(&asset, "unsafe", &stream, 0));
  assert(g_render_trace.validate_us == 0 && !g_render_trace.decode_outer_us && !g_render_trace.restart_reason);
  begin(8);
  assert(!render_h264_asset_frame(&asset, "unsafe", &stream, 0));
  assert(!g_render_trace.validate_us && !g_render_trace.scale_us);
  begin(9); stream.single_slice_valid = true; ppa_supported = false;
  assert(render_h264_asset_frame(&asset, "idle", &stream, 0));
  assert(g_render_trace.scale_us == 9 && !g_last_h264_pack_us && !g_last_h264_convert_us);
  assert(g_render_trace.fallback == PET_RENDER_FALLBACK_H264_CPU);
  begin(10); ppa_supported = true; fail_pack = true;
  assert(render_h264_asset_frame(&asset, "idle", &stream, 0));
  assert(g_render_trace.scale_us == 12 && g_last_h264_pack_us == 3 && !g_last_h264_convert_us);
  assert(g_render_trace.fallback == PET_RENDER_FALLBACK_H264_CPU);
  begin(11); fail_pack = false; fail_ppa = true;
  assert(render_h264_asset_frame(&asset, "idle", &stream, 0));
  assert(g_render_trace.scale_us == 17 && g_last_h264_convert_us == 5);
  assert(g_render_trace.fallback == PET_RENDER_FALLBACK_H264_CPU);
  begin(12); fail_layout = true;
  assert(!render_h264_asset_frame(&asset, "idle", &stream, 0));
  assert(g_render_trace.scale_us == 2 && !g_render_trace.fallback && !g_last_h264_pack_us);
  return 0;
}
''')


def test_frame_entry_resets_trace_before_all_page_branches():
    source = SOURCE.read_text()
    render = function(source, "pet_p4_renderer_render")
    assert render.index("pet_p4_render_trace_begin") < render.index('if(strcmp(page,"components"))')
    assert "pet_p4_render_trace_begin" not in function(source, "pet_p4_renderer_reset_playback")
    assert "reset_h264_decoder_measured" not in function(source, "pet_p4_renderer_reset_playback")
    # Every restart remains the existing bounded one-frame decoder policy.
    assert "#define PET_P4_H264_MAX_DECODE_FRAMES_PER_RENDER 1U" in source
    assert "PET_RENDER_FALLBACK_H264_CPU" in function(source, "render_h264_asset_frame")
    asset = function(source, "render_asset_pet_frame")
    for field in ("resolve_us", "cache_us", "copy_us", "decode_outer_us", "scale_us", "stream_size", "backend"):
        assert "g_render_trace." + field in asset
    assert "PET_RENDER_FALLBACK_JPEG_TJPGD" in asset
    assert "g_render_trace.decode_outer_us = g_last_decode_us" in asset
