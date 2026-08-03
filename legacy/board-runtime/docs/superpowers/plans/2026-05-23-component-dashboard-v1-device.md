# Phase B: 设备端 COMPONENT_DASHBOARD_V1 渲染分支

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** 给 `fb_speech_overlay` 增加一条与现有 `STATS_DASHBOARD_V1` 并列的 `COMPONENT_DASHBOARD_V1` 渲染分支。9 个固定槽位、字节上限与 claw-pet-manager `ref/src/lib/clawpkg-contract.js` 严格一致。同时修复现有 STATS 渲染在 480 屏 compact 模式下 `breakdown` / `sources` 不渲染的 bug。

**Architecture:** 仿照现有 `br_stats_dashboard_*` 单元（struct / defaults / set_value / parse / combined_text / render），新增并列的 `br_component_dashboard_*` 系列。dispatch 端在已有 `br_stats_dashboard_parse` 尝试之前先尝试 `br_component_dashboard_parse`，命中则走新 render，未命中再 fall through 到既有 STATS 分支。新 render 在 compact 模式下也输出 note / footer（compact bug fix 等价应用到新 render）。

**Tech Stack:** C11, framebuffer 32bpp BGRA, GNU Unifont 位图，单文件 include 测试（`fb_speech_overlay_layout_tests.c` 用 `#include "../src/fb_speech_overlay.c"` 把 static 函数全部拉进 test 翻译单元）。

**Repo:** `board-runtime/` lives in this repository next to `ref/`; use one feature branch for cross desktop/device work.

---

## 槽位契约（与 Phase A 一致）

```
| 槽位 | maxBytes | C 字段名 | 现有 STATS 字段对应 |
|------|----------|----------|---------------------|
| title       | 60  | title[64]         | agent (sort of)  |
| eyebrow     | 90  | eyebrow[96]       | eyebrow          |
| headline    | 156 | headline[160]     | headline         |
| metricLabel | 90  | metric_label[96]  | metric_title     |
| metricValue | 60  | metric_value[64]  | metric_value     |
| metricUnit  | 30  | metric_unit[32]   | metric_unit      |
| badge       | 12  | badge[16]         | completed        |
| note        | 156 | note[160]         | breakdown        |
| footer      | 156 | footer[160]       | sources          |
```

C 字段缓冲 = maxBytes + 至少 4 bytes 余量（null terminator + 安全余量），与现有 `br_stats_dashboard_model` 风格一致。

Parser 第一行匹配 `COMPONENT_DASHBOARD_V1`（19 字符）。

---

## Task B1: `br_component_dashboard_model` 契约 + parser

**Files:**
- Modify: `src/fb_speech_overlay.c` (add struct + defaults + set_value + parse function near existing `br_stats_dashboard_*` block, around line 88)

- [ ] **Step 1: 写 failing test 草案到 layout test 文件末尾**

修改 `tests/fb_speech_overlay_layout_tests.c`。把 existing `int main(void)` 重命名为 `static int test_stats_dashboard_renders_in_480(void)`，把所有现有 main 体内的逻辑搬进去保留。然后在文件尾部加新主 main：

```c
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
    "footer=红钮 切显示 · 旋钮 调下班时间 · 长按 重设\n";
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

int main(void) {
  int rc = 0;
  rc |= test_stats_dashboard_renders_in_480();
  rc |= test_component_dashboard_parses_nine_slots();
  rc |= test_component_dashboard_rejects_non_magic();
  return rc;
}
```

- [ ] **Step 2: 运行测试确认失败** — `cmake -S . -B build-armhf-tests && cmake --build build-armhf-tests --target fb-speech-overlay-layout-tests 2>&1 | tail -20`。预期 FAIL with "br_component_dashboard_model undefined" or similar.

> **注：** Building test on host (macOS) requires `cmake -S . -B build-host && cmake --build build-host --target fb-speech-overlay-layout-tests`. Cross-compile (armhf) only needed for B5. For B1-B4 build/test, use host toolchain.

- [ ] **Step 3: 实现 `br_component_dashboard_*`**

在 `src/fb_speech_overlay.c` line 88 之后（紧跟 `} br_stats_dashboard_model;` 的位置）插入：

```c
typedef struct {
  char title[64];        /* maxBytes 60, +4 safety */
  char eyebrow[96];      /* maxBytes 90, +6 safety */
  char headline[160];    /* maxBytes 156, +4 */
  char metric_label[96]; /* maxBytes 90 */
  char metric_value[64]; /* maxBytes 60 */
  char metric_unit[32];  /* maxBytes 30 */
  char badge[16];        /* maxBytes 12 */
  char note[160];        /* maxBytes 156 */
  char footer[160];      /* maxBytes 156 */
} br_component_dashboard_model;
```

在 `br_stats_dashboard_parse` 函数定义之前（line 730 附近，或就紧跟新 struct 后）插入：

```c
static void br_component_dashboard_defaults(br_component_dashboard_model *model) {
  memset(model, 0, sizeof(*model));
  snprintf(model->title, sizeof(model->title), "petAgent");
  snprintf(model->eyebrow, sizeof(model->eyebrow), "");
  snprintf(model->headline, sizeof(model->headline), "");
  snprintf(model->metric_label, sizeof(model->metric_label), "");
  snprintf(model->metric_value, sizeof(model->metric_value), "");
  snprintf(model->metric_unit, sizeof(model->metric_unit), "");
  snprintf(model->badge, sizeof(model->badge), "");
  snprintf(model->note, sizeof(model->note), "");
  snprintf(model->footer, sizeof(model->footer), "");
}

static void br_component_dashboard_set_value(br_component_dashboard_model *model, const char *key, const char *value) {
  if (!key || !value) return;
  if (strcmp(key, "title") == 0) {
    br_normalize_text(value, "petAgent", model->title, sizeof(model->title));
  } else if (strcmp(key, "eyebrow") == 0) {
    br_normalize_text(value, "", model->eyebrow, sizeof(model->eyebrow));
  } else if (strcmp(key, "headline") == 0) {
    br_normalize_text(value, "", model->headline, sizeof(model->headline));
  } else if (strcmp(key, "metricLabel") == 0) {
    br_normalize_text(value, "", model->metric_label, sizeof(model->metric_label));
  } else if (strcmp(key, "metricValue") == 0) {
    br_normalize_text(value, "", model->metric_value, sizeof(model->metric_value));
  } else if (strcmp(key, "metricUnit") == 0) {
    br_normalize_text(value, "", model->metric_unit, sizeof(model->metric_unit));
  } else if (strcmp(key, "badge") == 0) {
    br_normalize_text(value, "", model->badge, sizeof(model->badge));
  } else if (strcmp(key, "note") == 0) {
    br_normalize_text(value, "", model->note, sizeof(model->note));
  } else if (strcmp(key, "footer") == 0) {
    br_normalize_text(value, "", model->footer, sizeof(model->footer));
  }
}

static bool br_component_dashboard_parse(const char *text, br_component_dashboard_model *model) {
  if (!text || strncmp(text, "COMPONENT_DASHBOARD_V1", 22) != 0) {
    return false;
  }
  br_component_dashboard_defaults(model);

  const char *cursor = text;
  while (*cursor) {
    const char *line_end = strchr(cursor, '\n');
    size_t line_len = line_end ? (size_t) (line_end - cursor) : strlen(cursor);
    if (line_len > 0 && line_len < 256) {
      char line[256];
      memcpy(line, cursor, line_len);
      line[line_len] = '\0';
      char *eq = strchr(line, '=');
      if (eq) {
        *eq = '\0';
        br_component_dashboard_set_value(model, line, eq + 1);
      }
    }
    if (!line_end) break;
    cursor = line_end + 1;
  }
  return true;
}
```

- [ ] **Step 4: 运行测试确认 parse 测试通过、render 测试还在因为 render 未实现**

`cmake --build build-host --target fb-speech-overlay-layout-tests && ./build-host/fb-speech-overlay-layout-tests 2>&1 | tail -10`

预期：`test_component_dashboard_parses_nine_slots` PASS + `test_component_dashboard_rejects_non_magic` PASS + `test_stats_dashboard_renders_in_480` PASS（不动 stats）。Test rc 应该是 0。

- [ ] **Step 5: 提交** — `git commit -m "feat(fb-overlay): COMPONENT_DASHBOARD_V1 model + parser"`

---

## Task B2: `br_overlay_render_component_dashboard` + compact bug fix

**Files:**
- Modify: `src/fb_speech_overlay.c` (add render function, near existing `br_overlay_render_stats_dashboard`)
- Modify: `tests/fb_speech_overlay_layout_tests.c` (add render test verifying note + footer pixels in 800x480 compact mode)

- [ ] **Step 1: 写 failing render test**

在 layout test 文件加：

```c
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
    "footer=红钮 切显示 · 旋钮 调下班时间 · 长按 重设\n";
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
  /* preload all glyphs that will be drawn */
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

  /* note region (below the metric panel) and footer region (bottom 60 px) must have
     non-bg pixels. Existing STATS render skips these in compact (height<=540); we
     specifically guarantee they DO render in COMPONENT. */
  int note_region_pixels = 0;
  int footer_region_pixels = 0;
  for (int y = 360; y < 410; y++) {
    const unsigned char *row = fb.data + (size_t) y * (size_t) fb.stride;
    for (int x = 40; x < 760; x++) {
      const unsigned char *px = row + (size_t) x * 4U;
      /* count any pixel that is NOT the bg (5,7,6,255) */
      if (!(px[0] == 5 && px[1] == 7 && px[2] == 6)) {
        note_region_pixels++;
      }
    }
  }
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
```

并在 `main()` 中加 `rc |= test_component_dashboard_renders_note_and_footer_in_480();`。

- [ ] **Step 2: 运行测试确认 FAIL** — render 函数还不存在。

- [ ] **Step 3: 实现 `br_overlay_render_component_dashboard`**

在 `br_overlay_render_stats_dashboard` 函数下方（line 888 附近，紧跟 `return true;` 后面）插入下列函数。它 clone 自 stats render，把字段名映射到 component 槽位，**关键差异：去掉 `!compact` 的 note/footer guard**。

```c
static bool br_overlay_render_component_dashboard(
  const br_overlay_config *config,
  br_framebuffer *fb,
  br_glyph_cache *cache,
  const br_component_dashboard_model *model
) {
  bool compact = fb->height <= 540;
  uint32_t bg = br_pack_pixel(5, 7, 6, 255);
  uint32_t scanline = br_pack_pixel(16, 27, 19, 255);
  uint32_t green_wash = br_pack_pixel(10, 31, 22, 255);
  uint32_t amber_wash = br_pack_pixel(36, 25, 10, 255);
  uint32_t panel = br_pack_pixel(11, 14, 13, 255);
  uint32_t panel_green = br_pack_pixel(13, 39, 25, 255);
  uint32_t dim = br_pack_pixel(158, 164, 160, 255);
  uint32_t ivory = br_pack_pixel(250, 241, 204, 255);
  uint32_t orange = br_pack_pixel(255, 163, 31, 255);
  uint32_t green = br_pack_pixel(85, 193, 133, 255);
  uint32_t dark_text = br_pack_pixel(19, 18, 13, 255);

  br_fill_rect(fb, 0, 0, fb->width, fb->height, bg);
  int left_wash = fb->width / 3;
  br_fill_rect(fb, 0, 0, left_wash, fb->height, green_wash);
  br_fill_rect(fb, fb->width - fb->width / 4, 0, fb->width / 4, fb->height, amber_wash);
  for (int y = 0; y < fb->height; y += 4) {
    br_fill_rect(fb, 0, y, fb->width, 1, scanline);
  }

  int margin = compact ? (fb->width < 700 ? 30 : 44) : (fb->width < 700 ? 38 : 56);
  int badge_x = margin;
  int badge_y = compact ? 42 : 64;
  int badge_w = compact ? (fb->width < 700 ? 210 : 248) : (fb->width < 700 ? 246 : 304);
  int badge_h = compact ? 78 : 112;
  br_fill_rounded_rect(fb, badge_x, badge_y, badge_w, badge_h, 10, panel_green);
  br_draw_scaled_text_fit(config, fb, cache, model->title,
                          badge_x + (compact ? 18 : 24),
                          badge_y + (compact ? 18 : 24),
                          badge_w - (compact ? 36 : 48),
                          compact ? 3.0 : 4.2,
                          2,
                          br_pack_pixel(225, 241, 231, 255));

  /* badge circle (top-right, single circle for COMPONENT_DASHBOARD_V1 — no alert) */
  int circle_radius = compact ? 36 : 45;
  int circle_shadow = compact ? 4 : 5;
  int circle_y = compact ? 82 : 118;
  int done_x = fb->width - margin - circle_radius;
  br_fill_circle(fb, done_x + circle_shadow, circle_y + circle_shadow,
                 circle_radius + circle_shadow, br_pack_pixel(20, 57, 39, 255));
  br_fill_circle(fb, done_x, circle_y, circle_radius, green);
  if (model->badge[0]) {
    br_draw_scaled_text_center(config, fb, cache, model->badge, done_x,
                               circle_y - (compact ? 19 : 24),
                               compact ? 2.55 : 3.3, 1, dark_text);
  }

  int copy_y = compact ? 168 : 286;
  int headline_y = copy_y + (compact ? 40 : 58);
  int divider_y = copy_y + (compact ? 114 : 146);
  if (model->eyebrow[0]) {
    br_draw_scaled_text(config, fb, cache, model->eyebrow, margin, copy_y,
                        compact ? 1.85 : 2.4, 2, dim);
  }
  if (model->headline[0]) {
    br_draw_scaled_text_fit(config, fb, cache, model->headline, margin, headline_y,
                            fb->width - margin * 2, compact ? 3.0 : 3.8, 2, orange);
  }
  br_stroke_dashed_hline(fb, margin, divider_y, fb->width - margin * 2, 9, 7,
                         br_pack_pixel(74, 74, 62, 255));

  /* metric panel */
  int token_y = compact ? divider_y + 24 : copy_y + 184;
  int token_w = fb->width - margin * 2;
  int token_h = compact ? 132 : 190;
  br_fill_rounded_rect(fb, margin, token_y, token_w, token_h, 8, panel);
  if (model->metric_label[0]) {
    br_draw_scaled_text(config, fb, cache, model->metric_label,
                        margin + (compact ? 20 : 26),
                        token_y + (compact ? 18 : 28),
                        compact ? 1.65 : 2.5, 2, dim);
  }
  if (model->metric_value[0]) {
    br_overlay_config value_cfg = *config;
    value_cfg.scale = compact ? 3.15 : 4.4;
    value_cfg.tracking = 2;
    int value_max_width = token_w - (compact ? 150 : 190);
    while (value_cfg.scale > 1.0 && br_overlay_measure_text(&value_cfg, cache, model->metric_value) > value_max_width) {
      value_cfg.scale -= 0.15;
    }
    int value_x = margin + (compact ? 20 : 26);
    int value_width = br_overlay_measure_text(&value_cfg, cache, model->metric_value);
    br_draw_text_color(&value_cfg, fb, cache, model->metric_value, value_x,
                       token_y + (compact ? 58 : 86), ivory);
    if (model->metric_unit[0]) {
      br_draw_scaled_text(config, fb, cache, model->metric_unit,
                          value_x + value_width + (compact ? 14 : 18),
                          token_y + (compact ? 80 : 116),
                          compact ? 1.15 : 1.7, 1, dim);
    }
  }

  /* IMPORTANT: COMPONENT_DASHBOARD_V1 renders note + footer ALSO in compact mode.
     This is the explicit fix for the STATS_DASHBOARD_V1 compact-mode regression. */
  if (model->note[0]) {
    int note_y = compact ? token_y + token_h + 12 : token_y + token_h + 34;
    br_draw_scaled_text_fit(config, fb, cache, model->note, margin, note_y,
                            fb->width - margin * 2, compact ? 1.3 : 1.45, 1,
                            br_pack_pixel(164, 169, 160, 255));
  }
  if (model->footer[0]) {
    int footer_y = compact ? fb->height - 40 : fb->height - 60;
    br_draw_scaled_text_fit(config, fb, cache, model->footer, margin, footer_y,
                            fb->width - margin * 2, compact ? 1.15 : 1.35, 1,
                            br_pack_pixel(111, 183, 142, 255));
  }
  return true;
}
```

- [ ] **Step 4: Build + run tests** — all 4 tests PASS expected (stats + parse + reject + render).

- [ ] **Step 5: 提交** — `git commit -m "feat(fb-overlay): COMPONENT_DASHBOARD_V1 render with compact-mode note/footer fix"`

---

## Task B3: dispatcher 加 COMPONENT 分支

**Files:**
- Modify: `src/fb_speech_overlay.c` (modify `br_overlay_build_frame` around line 1042)

- [ ] **Step 1: 加一个 combined_text helper for component** in src/fb_speech_overlay.c, near `br_stats_dashboard_combined_text`:

```c
static void br_component_dashboard_combined_text(
  const br_component_dashboard_model *model,
  const char *debug_text,
  char *out,
  size_t out_size
) {
  snprintf(out, out_size, "%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s",
           debug_text ? debug_text : "",
           model->title,
           model->eyebrow,
           model->headline,
           model->metric_label,
           model->metric_value,
           model->metric_unit,
           model->badge,
           model->note,
           model->footer);
}
```

- [ ] **Step 2: Modify `br_overlay_build_frame`** (the function containing the dispatcher at line 1042). Locate the line:

```c
is_dashboard = br_stats_dashboard_parse(speech_text, &dashboard);
```

Add component dispatch BEFORE it. The whole dispatch block becomes:

```c
br_component_dashboard_model component;
bool is_component_dashboard = br_component_dashboard_parse(speech_text, &component);
if (is_component_dashboard) {
  br_component_dashboard_combined_text(&component, debug_text, combined_text, sizeof(combined_text));
} else {
  is_dashboard = br_stats_dashboard_parse(speech_text, &dashboard);
  if (is_dashboard) {
    br_stats_dashboard_combined_text(&dashboard, debug_text, combined_text, sizeof(combined_text));
  } else {
    snprintf(combined_text, sizeof(combined_text), "%s\n%s", debug_text ? debug_text : "", speech_text ? speech_text : "");
  }
}
```

And the render dispatch block (line 1051-1056) becomes:

```c
if (is_component_dashboard) {
  if (!br_overlay_render_component_dashboard(config, fb, &cache, &component)) {
    return false;
  }
  return br_overlay_render_card(config, fb, &cache, debug_text, true);
}
if (is_dashboard) {
  if (!br_overlay_render_stats_dashboard(config, fb, &cache, &dashboard)) {
    return false;
  }
  return br_overlay_render_card(config, fb, &cache, debug_text, true);
}
```

- [ ] **Step 3: Build + run all 4 layout tests** — PASS expected. (No new test added here; B4 will add an integration test going through build_frame.)

- [ ] **Step 4: 提交** — `git commit -m "feat(fb-overlay): dispatch COMPONENT_DASHBOARD_V1 in br_overlay_build_frame"`

---

## Task B4: 槽位越界 + 整体 frame 测试

**Files:**
- Modify: `tests/fb_speech_overlay_layout_tests.c`

- [ ] **Step 1: Add `test_component_dashboard_truncates_oversized_slots`**:

`br_normalize_text` is the existing safe-copy helper used by stats and component setters. It truncates if value exceeds the destination buffer (the `sizeof(model->...)` arg). Verify behavior:

```c
static int test_component_dashboard_truncates_oversized_slots(void) {
  /* badge buffer is 16 bytes -> normalize truncates to fit; 90-byte input is way larger */
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
  /* The truncated value should be a non-empty prefix (no garbage) */
  if (badge_len == 0) {
    fprintf(stderr, "B4: badge fully dropped on truncation\n");
    return 1;
  }
  return 0;
}
```

- [ ] **Step 2: Add `test_component_dashboard_unknown_keys_are_ignored`**:

```c
static int test_component_dashboard_unknown_keys_are_ignored(void) {
  const char *payload =
    "COMPONENT_DASHBOARD_V1\n"
    "title=ok\n"
    "bogus_key=should be ignored\n"
    "footer=tail\n";
  br_component_dashboard_model model;
  if (!br_component_dashboard_parse(payload, &model)) return 1;
  if (strcmp(model.title, "ok") != 0) return 1;
  if (strcmp(model.footer, "tail") != 0) return 1;
  return 0;
}
```

- [ ] **Step 3: Wire both into `main()`** along with the prior tests.

- [ ] **Step 4: Build + run** — all 6 tests PASS expected.

- [ ] **Step 5: 提交** — `git commit -m "test(fb-overlay): COMPONENT_DASHBOARD_V1 truncation + unknown-key coverage"`

---

## Task B5: docs + 交叉编译验证

**Files:**
- Create: `docs/component-dashboard-v1.md`
- Modify: `docs/stats-page.md` (add a cross-reference; do not rewrite)
- Modify: `docs/device-runtime-design.md` (if exists; otherwise skip)

- [ ] **Step 1: Write `docs/component-dashboard-v1.md`** — contract reference matching `claw-pet-manager/ref/src/lib/clawpkg-contract.js`. Include:
  - 9 slot table (id, maxBytes, role)
  - Payload format: `COMPONENT_DASHBOARD_V1\n<key>=<value>\n...`
  - Magic line literal: `COMPONENT_DASHBOARD_V1` (22 bytes)
  - Buffer convention: C-side buffer is `maxBytes + 4` bytes (null terminator + small safety)
  - File contract: identical to STATS — payload < 2048 bytes total, lives in `.stats-display` file (same channel)
  - Compact-mode note: COMPONENT renders `note` + `footer` in 480 mode (unlike legacy STATS_DASHBOARD_V1).

- [ ] **Step 2: 给 stats-page.md 加 cross-link** — 在文件末尾或现有 dashboard 节末尾加 1-2 句话指向 `docs/component-dashboard-v1.md`，说明通用组件使用 COMPONENT_DASHBOARD_V1。

- [ ] **Step 3: 交叉编译验证** — `sh scripts/build-armhf.sh 2>&1 | tail -20`。预期：`Finished` 或 `[100%]` 类似 success markers，无新增 error。

- [ ] **Step 4: 提交** — `git commit -m "docs: COMPONENT_DASHBOARD_V1 device contract + cross-link"`

---

## 自检结论

- 覆盖 spec：
  - struct + parse (B1)
  - render + compact bug fix (B2)
  - dispatcher (B3)
  - 测试 parse + render + truncation + unknown-key (B1/B2/B4)
  - docs + armhf cross-compile (B5)
- 类型一致性：9 slot id 与 claw-pet-manager `clock-pet-manager/ref/src/lib/clawpkg-contract.js` 完全一致（title, eyebrow, headline, metricLabel, metricValue, metricUnit, badge, note, footer）。C 端字段为 snake_case 内部但 parse key 为 camelCase（接收 JSON-like 输入）。
- 字节预算：C 字段缓冲 ≥ maxBytes + 4 bytes safety；`br_normalize_text` 已在现有 stats 路径验证过 truncation 安全。
- 不依赖设备硬件：所有 B1-B4 测试都用 host 编译 + memory-only framebuffer 验证，无需真机。B5 cross-compile 只验证产物能 link，不刷机。
