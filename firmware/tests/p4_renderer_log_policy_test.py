"""Normal render logging must not join a serialized, paced hello at INFO."""
from pathlib import Path
import re

import pytest

from p4_h264_prefetch_test import compile_run
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def test_production_renderer_routine_logs_are_disabled_without_suppressing_faults():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    assert not re.search(r"\bESP_LOGI\s*\(", source)
    assert 'ESP_LOGW(' in source
    for name in ("sdkconfig.esp32_p4_program_public_candidate",
                 "sdkconfig.esp32_p4_program_public_candidate_v3"):
        config = (ROOT / name).read_text().splitlines()
        assert "CONFIG_LOG_DEFAULT_LEVEL=3" in config
        assert "CONFIG_LOG_MAXIMUM_LEVEL=3" in config
    # The complete frame and subsequent gap still include their original work.
    frame = function(source, "pet_p4_renderer_render")
    assert frame.index("render_asset_pet_frame(") < frame.index("render_ended_us =")
    assert frame.index("render_ended_us =") < frame.index("record_render_performance(")
    assert frame.index("record_render_performance(") < frame.index("h264_frame_offer_end(lcd_err==ESP_OK)")
    record = function(source, "record_render_performance")
    assert "pet_render_lifetime_record(" in record
    assert "g_metrics.lifetime" in record and "PET_P4_RENDER_FRAME_BUDGET_US" in record


@pytest.mark.parametrize("level,legacy_info", [(3, False), (3, True), (4, False)])
def test_actual_family_success_blocks_do_not_wait_on_busy_tx_at_info(tmp_path, level, legacy_info):
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    body = function(source, "render_asset_pet_frame")
    blocks = re.findall(
        r'if \(!g_logged_asset \|\| strcmp\(g_rendered_family, asset.family\) != 0\) \{[^{}]*\}',
        body,
    )
    assert len(blocks) == 2  # actual H264 and JPEG success paths
    if legacy_info:
        blocks = [block.replace("ESP_LOGD(", "ESP_LOGI(") for block in blocks]
    blocked = legacy_info or level >= 4
    code = r'''
#include "pet_p4_render_lifetime.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static const char *TAG = "render";
static bool g_logged_asset;
static char g_rendered_family[80];
static struct {const char *family,*path;} asset = {"idle.changed","p4/frame.h264"};
static uint32_t clock_us, sink_calls;
/* A deterministic busy transport seam: the old INFO call waits for the
 * remaining hello TX. It models the observed residual, not device timing. */
static void log_transport(const char *tag,const char *format,...) {
  assert(tag && format); sink_calls++; clock_us += 54004;
}
#define ESP_LOGI(...) log_transport(__VA_ARGS__)
#define ESP_LOGD(...) do { if (TEST_LOG_LEVEL >= 4) log_transport(__VA_ARGS__); } while (0)
static void h264_success(void) {
''' + blocks[0] + r'''
}
static void jpeg_success(void) {
''' + blocks[1] + r'''
}
int main(void) {
  void (*paths[])(void) = {h264_success,jpeg_success};
  for (unsigned i=0;i<2;i++) {
    g_logged_asset=false; g_rendered_family[0]=0; sink_calls=0;
    clock_us=34113; /* 88117 - 54004; deterministic regression fixture */
    paths[i]();
    assert(g_logged_asset && !strcmp(g_rendered_family,asset.family));
    pet_render_lifetime_t lifetime={0};
    pet_render_lifetime_record(&lifetime,clock_us,0,1,1,1,clock_us+1,66667,66667);
    assert(clock_us == (EXPECT_BLOCKED ? 88117U : 34113U));
    assert(sink_calls == EXPECT_BLOCKED && lifetime.over_budget == EXPECT_BLOCKED);
    /* Same-family retention still suppresses repeated announcements. */
    paths[i](); assert(sink_calls == EXPECT_BLOCKED);
  }
  return 0;
}
'''
    compile_run(tmp_path, f"#define TEST_LOG_LEVEL {level}\n#define EXPECT_BLOCKED {int(blocked)}\n" + code, "logging")
