#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main br_overlay_program_main
#include "../src/fb_speech_overlay.c"
#undef main

static int count_bgra(
  const br_framebuffer *fb,
  int x0,
  int y0,
  int x1,
  int y1,
  unsigned char blue,
  unsigned char green,
  unsigned char red,
  unsigned char alpha
) {
  int count = 0;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > fb->width) x1 = fb->width;
  if (y1 > fb->height) y1 = fb->height;
  for (int y = y0; y < y1; y += 1) {
    const unsigned char *row = fb->data + (size_t) y * (size_t) fb->stride;
    for (int x = x0; x < x1; x += 1) {
      const unsigned char *px = row + (size_t) x * 4U;
      if (px[0] == blue && px[1] == green && px[2] == red && px[3] == alpha) {
        count += 1;
      }
    }
  }
  return count;
}

static int test_stats_dashboard_renders_in_480(void) {
  const char *payload =
    "STATS_DASHBOARD_V1\n"
    "agent=Codex\n"
    "eyebrow=等价于购买了\n"
    "lunch=3.7\n"
    "headline=约 3.7 顿工作午餐\n"
    "metricTitle=今日累计 Token\n"
    "metricValue=1.30M\n"
    "metricUnit=TOKEN\n"
    "alerts=1\n"
    "completed=1\n";
  br_overlay_config config;
  br_stats_dashboard_model model;
  br_glyph_cache cache;
  br_framebuffer fb;
  char combined[4096];

  br_overlay_load_config(&config, ".");
  if (!br_stats_dashboard_parse(payload, &model)) {
    fprintf(stderr, "failed to parse dashboard payload\n");
    return 1;
  }
  br_stats_dashboard_combined_text(&model, "", combined, sizeof(combined));
  if (!strstr(combined, "!")) {
    fprintf(stderr, "dashboard glyph preload text must include alert icon glyph\n");
    return 1;
  }
  if (!br_overlay_load_glyphs("./unifont-17.0.04.hex.gz", combined, &cache)) {
    fprintf(stderr, "failed to load test font\n");
    return 1;
  }

  memset(&fb, 0, sizeof(fb));
  fb.width = 800;
  fb.height = 480;
  fb.stride = fb.width * 4;
  fb.data = (unsigned char *) calloc((size_t) fb.stride * (size_t) fb.height, 1);
  if (!fb.data) {
    fprintf(stderr, "failed to allocate framebuffer\n");
    return 1;
  }

  if (!br_overlay_render_stats_dashboard(&config, &fb, &cache, &model)) {
    fprintf(stderr, "failed to render dashboard\n");
    free(fb.data);
    return 1;
  }

  int panel_pixels = count_bgra(&fb, 40, 300, 760, 462, 13, 14, 11, 255);
  int token_text_pixels = count_bgra(&fb, 40, 300, 760, 462, 204, 241, 250, 255);
  free(fb.data);

  if (panel_pixels < 8000) {
    fprintf(stderr, "token panel is not visible in 800x480 layout: panel_pixels=%d\n", panel_pixels);
    return 1;
  }
  if (token_text_pixels < 100) {
    fprintf(stderr, "token value text is not visible in 800x480 layout: token_text_pixels=%d\n", token_text_pixels);
    return 1;
  }

  return 0;
}

static int test_component_dashboard_parses_nine_slots(void) {
  const char *payload =
    "COMPONENT_DASHBOARD_V1\n"
    "title=摸鱼倒计时\n"
    "eyebrow=距离今天下班\n"
    "headline=还有 2 小时 13 分\n"
    "metricLabel=下班时间\n"
    "metricValue=18:00\n"
    "metricUnit=\n"
    "badge=5\n"
    "note=本周已坚持 5 天\n"
    "footer=自动倒计时 · 点击切显示 · 长按重置\n";
  br_component_dashboard_model model;
  if (!br_component_dashboard_parse(payload, &model)) {
    fprintf(stderr, "B1: failed to parse COMPONENT_DASHBOARD_V1 payload\n");
    return 1;
  }
  if (strcmp(model.title, "摸鱼倒计时") != 0) {
    fprintf(stderr, "B1: title mismatch: %s\n", model.title);
    return 1;
  }
  if (strcmp(model.note, "本周已坚持 5 天") != 0) {
    fprintf(stderr, "B1: note mismatch: %s\n", model.note);
    return 1;
  }
  if (strcmp(model.badge, "5") != 0) {
    fprintf(stderr, "B1: badge mismatch: %s\n", model.badge);
    return 1;
  }
  return 0;
}

static int test_component_dashboard_rejects_non_magic(void) {
  br_component_dashboard_model model;
  if (br_component_dashboard_parse("STATS_DASHBOARD_V1\nagent=Codex\n", &model)) {
    fprintf(stderr, "B1: component parser should reject STATS_DASHBOARD_V1 payload\n");
    return 1;
  }
  if (br_component_dashboard_parse(NULL, &model)) {
    fprintf(stderr, "B1: component parser should reject NULL\n");
    return 1;
  }
  return 0;
}

static int test_component_dashboard_renders_note_and_footer_in_480(void) {
  const char *payload =
    "COMPONENT_DASHBOARD_V1\n"
    "title=摸鱼倒计时\n"
    "eyebrow=距离今天下班\n"
    "headline=还有 2 小时 13 分\n"
    "metricLabel=下班时间\n"
    "metricValue=18:00\n"
    "metricUnit=\n"
    "badge=5\n"
    "note=本周已坚持 5 天\n"
    "footer=自动倒计时 · 点击切显示 · 长按重置\n";
  br_overlay_config config;
  br_component_dashboard_model model;
  br_glyph_cache cache;
  br_framebuffer fb;
  char combined[4096];

  br_overlay_load_config(&config, ".");
  if (!br_component_dashboard_parse(payload, &model)) {
    fprintf(stderr, "B2: parse failed\n");
    return 1;
  }
  snprintf(combined, sizeof(combined), "%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s",
           model.title, model.eyebrow, model.headline,
           model.metric_label, model.metric_value, model.metric_unit,
           model.badge, model.note, model.footer);
  if (!br_overlay_load_glyphs("./unifont-17.0.04.hex.gz", combined, &cache)) {
    fprintf(stderr, "B2: failed to load test font\n");
    return 1;
  }

  memset(&fb, 0, sizeof(fb));
  fb.width = 800; fb.height = 480;
  fb.stride = fb.width * 4;
  fb.data = (unsigned char *) calloc((size_t) fb.stride * (size_t) fb.height, 1);
  if (!fb.data) { fprintf(stderr, "B2: alloc fail\n"); return 1; }

  if (!br_overlay_render_component_dashboard(&config, &fb, &cache, &model)) {
    fprintf(stderr, "B2: render failed\n");
    free(fb.data);
    return 1;
  }

  /* note region inside the metric panel and footer region (bottom 60 px) must
     have visible pixels. The STATS clone wraps these in !compact and skips them
     in 480; the COMPONENT render MUST keep them visible. */
  int note_region_pixels = count_bgra(&fb, 40, 285, 760, 325, 160, 169, 164, 255);
  int footer_region_pixels = 0;
  for (int y = 420; y < 470; y++) {
    const unsigned char *row = fb.data + (size_t) y * (size_t) fb.stride;
    for (int x = 40; x < 760; x++) {
      const unsigned char *px = row + (size_t) x * 4U;
      if (!(px[0] == 5 && px[1] == 7 && px[2] == 6)) {
        footer_region_pixels++;
      }
    }
  }
  free(fb.data);

  if (note_region_pixels < 100) {
    fprintf(stderr, "B2: note region not rendered in 480 compact mode (pixels=%d)\n", note_region_pixels);
    return 1;
  }
  if (footer_region_pixels < 100) {
    fprintf(stderr, "B2: footer region not rendered in 480 compact mode (pixels=%d)\n", footer_region_pixels);
    return 1;
  }
  return 0;
}

static int test_component_dashboard_truncates_oversized_slots(void) {
  /* badge buffer is 16 bytes total -> br_normalize_text must truncate; the
     91-char ASCII input is much larger than the destination buffer. */
  const char *payload =
    "COMPONENT_DASHBOARD_V1\n"
    "badge=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
  br_component_dashboard_model model;
  if (!br_component_dashboard_parse(payload, &model)) {
    fprintf(stderr, "B4: parse failed\n");
    return 1;
  }
  size_t badge_len = strlen(model.badge);
  if (badge_len >= sizeof(model.badge)) {
    fprintf(stderr, "B4: badge buffer overflow: len=%zu cap=%zu\n", badge_len, sizeof(model.badge));
    return 1;
  }
  /* Truncated value should be a non-empty prefix (i.e. br_normalize_text actually copied something). */
  if (badge_len == 0) {
    fprintf(stderr, "B4: badge fully dropped on truncation\n");
    return 1;
  }
  return 0;
}

static int test_component_dashboard_unknown_keys_are_ignored(void) {
  const char *payload =
    "COMPONENT_DASHBOARD_V1\n"
    "title=ok\n"
    "bogus_key=should be ignored\n"
    "footer=tail\n";
  br_component_dashboard_model model;
  if (!br_component_dashboard_parse(payload, &model)) return 1;
  if (strcmp(model.title, "ok") != 0) {
    fprintf(stderr, "B4: title not set after unknown key: %s\n", model.title);
    return 1;
  }
  if (strcmp(model.footer, "tail") != 0) {
    fprintf(stderr, "B4: footer not set after unknown key: %s\n", model.footer);
    return 1;
  }
  return 0;
}

static int test_component_dashboard_parses_progress_slot(void) {
  const char *payload =
    "COMPONENT_DASHBOARD_V1\n"
    "title=🍅 番茄钟\n"
    "headline=🔴 专注中\n"
    "progress=75:本轮进度\n";
  br_component_dashboard_model model;
  if (!br_component_dashboard_parse(payload, &model)) {
    fprintf(stderr, "progress: parse failed\n");
    return 1;
  }
  if (strcmp(model.progress, "75:本轮进度") != 0) {
    fprintf(stderr, "progress: value mismatch: '%s'\n", model.progress);
    return 1;
  }
  /* emoji codepoint should survive in title slot (utf-8 4-byte) */
  if (strstr(model.title, "番茄钟") == NULL) {
    fprintf(stderr, "progress: title CJK lost after emoji: '%s'\n", model.title);
    return 1;
  }
  return 0;
}

static int test_component_dashboard_renders_progress_bar(void) {
  const char *payload =
    "COMPONENT_DASHBOARD_V1\n"
    "title=🍅 番茄钟\n"
    "metricLabel=剩余时间\n"
    "metricValue=24:59\n"
    "note=🎯 本轮目标\n"
    "progress=80:本轮进度\n";
  br_overlay_config config;
  br_component_dashboard_model model;
  br_glyph_cache cache;
  br_framebuffer fb;
  char combined[4096];

  br_overlay_load_config(&config, ".");
  if (!br_component_dashboard_parse(payload, &model)) {
    fprintf(stderr, "renders_progress: parse failed\n");
    return 1;
  }
  snprintf(combined, sizeof(combined), "%s\n%s\n%s\n%s\n%s\n%s",
           model.title, model.metric_label, model.metric_value, model.note, "本轮进度", "80%");
  if (!br_overlay_load_glyphs("./unifont-17.0.04.hex.gz", combined, &cache)) {
    fprintf(stderr, "renders_progress: failed to load font\n");
    return 1;
  }

  memset(&fb, 0, sizeof(fb));
  fb.width = 800; fb.height = 480;
  fb.stride = fb.width * 4;
  fb.data = (unsigned char *) calloc((size_t) fb.stride * (size_t) fb.height, 1);
  if (!fb.data) { fprintf(stderr, "alloc fail\n"); return 1; }

  if (!br_overlay_render_component_dashboard(&config, &fb, &cache, &model)) {
    fprintf(stderr, "renders_progress: render failed\n");
    free(fb.data);
    return 1;
  }

  /* count orange (255,163,31) pixels — the progress bar fill lives inside the
     metric panel near its bottom edge. With a top-right headline it sits higher;
     without one it falls back lower, so this range covers both layouts. */
  int orange_pixels = count_bgra(&fb, 40, 332, 760, 410, 31, 163, 255, 255);
  free(fb.data);

  if (orange_pixels < 200) {
    fprintf(stderr, "renders_progress: progress bar fill not visible (orange=%d)\n", orange_pixels);
    return 1;
  }
  return 0;
}

int main(void) {
  int rc = 0;
  rc |= test_stats_dashboard_renders_in_480();
  rc |= test_component_dashboard_parses_nine_slots();
  rc |= test_component_dashboard_rejects_non_magic();
  rc |= test_component_dashboard_renders_note_and_footer_in_480();
  rc |= test_component_dashboard_truncates_oversized_slots();
  rc |= test_component_dashboard_unknown_keys_are_ignored();
  rc |= test_component_dashboard_parses_progress_slot();
  rc |= test_component_dashboard_renders_progress_bar();
  return rc;
}
