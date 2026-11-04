"""Compile the actual ST7701 entry/header with counted constructor adapters."""
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/esp_lcd_st7701"


def prepare(tmp_path):
    headers = {
        "soc/soc_caps.h": "#pragma once\n",
        "hal/lcd_types.h": "#pragma once\n",
        "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG -2\n#define ESP_ERR_NOT_SUPPORTED -3\n",
        "esp_lcd_types.h": "#pragma once\n#include <stddef.h>\n#include <stdbool.h>\ntypedef void *esp_lcd_panel_io_handle_t;\ntypedef void *esp_lcd_panel_handle_t;\n",
        "esp_lcd_panel_vendor.h": "#pragma once\n#include \"esp_err.h\"\n#include \"esp_lcd_types.h\"\ntypedef struct {void *vendor_config;} esp_lcd_panel_dev_config_t;\n",
        "esp_lcd_panel_rgb.h": "#pragma once\ntypedef struct {int unused;} esp_lcd_rgb_panel_config_t;\n",
        "esp_lcd_mipi_dsi.h": "#pragma once\ntypedef void *esp_lcd_dsi_bus_handle_t;\ntypedef struct {int unused;} esp_lcd_dpi_panel_config_t;\n",
        "esp_idf_version.h": "#pragma once\n#define ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))\n#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,4)\n",
        "esp_check.h": r'''#pragma once
#include "esp_err.h"
void test_log(const char *, const char *, int, int, int);
#define ESP_LOGI(tag, fmt, a, b, c) test_log(tag, fmt, a, b, c)
#define ESP_RETURN_ON_FALSE(condition, error, tag, message) do {if (!(condition)) return (error);} while (0)
''',
    }
    for name, text in headers.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    return ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
            "-DESP_LCD_ST7701_VER_MAJOR=1", "-DESP_LCD_ST7701_VER_MINOR=2", "-DESP_LCD_ST7701_VER_PATCH=3",
            "-I", str(tmp_path), "-I", str(COMPONENT / "include"), "-I", str(COMPONENT / "priv_include")]


HARNESS = r'''
#include <assert.h>
#include <string.h>
#include "esp_lcd_st7701.h"
static unsigned logs, rgb_calls, mipi_calls;
static void *expected_io;
static const esp_lcd_panel_dev_config_t *expected_config;
static esp_lcd_panel_handle_t *expected_output;
static int driver_result;
void test_log(const char *tag, const char *fmt, int a, int b, int c) {
    assert(!strcmp(tag,"st7701") && !strcmp(fmt,"version: %d.%d.%d"));
    assert(a==1 && b==2 && c==3); logs++;
}
#if SOC_LCD_RGB_SUPPORTED && !defined(MIPI_LINK_ONLY)
esp_err_t esp_lcd_new_panel_st7701_rgb(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *cfg, esp_lcd_panel_handle_t *out) {
    assert(io==expected_io && cfg==expected_config && out==expected_output);
    rgb_calls++; *out=(void*)17; return driver_result;
}
#endif
#if SOC_MIPI_DSI_SUPPORTED
esp_err_t esp_lcd_new_panel_st7701_mipi(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *cfg, esp_lcd_panel_handle_t *out) {
    assert(io==expected_io && cfg==expected_config && out==expected_output);
    mipi_calls++; *out=(void*)19; return driver_result;
}
#endif
int main(void) {
    st7701_vendor_config_t vendor={0};
    esp_lcd_panel_dev_config_t cfg={.vendor_config=&vendor};
    esp_lcd_panel_handle_t output=(void*)23;
    expected_io=(void*)11; expected_config=&cfg; expected_output=&output;
#ifndef MIPI_LINK_ONLY
    assert(esp_lcd_new_panel_st7701(expected_io,NULL,&output)==ESP_ERR_INVALID_ARG);
    assert(esp_lcd_new_panel_st7701(expected_io,&cfg,NULL)==ESP_ERR_INVALID_ARG);
    cfg.vendor_config=NULL;
    assert(esp_lcd_new_panel_st7701(expected_io,&cfg,&output)==ESP_ERR_INVALID_ARG);
    cfg.vendor_config=&vendor;
    driver_result=-37;
    unsigned before=logs;
#if SOC_LCD_RGB_SUPPORTED
    assert(esp_lcd_new_panel_st7701(expected_io,&cfg,&output)==-37);
    assert(rgb_calls==1 && output==(void*)17);
#else
    assert(esp_lcd_new_panel_st7701(expected_io,&cfg,&output)==ESP_ERR_NOT_SUPPORTED);
#endif
    assert(logs==before+1);
    vendor.flags.use_mipi_interface=1;
#if SOC_MIPI_DSI_SUPPORTED
    assert(esp_lcd_new_panel_st7701(expected_io,&cfg,&output)==-37);
    assert(mipi_calls==1 && output==(void*)19);
#else
    assert(esp_lcd_new_panel_st7701(expected_io,&cfg,&output)==ESP_ERR_NOT_SUPPORTED);
#endif
#endif
#if SOC_MIPI_DSI_SUPPORTED
    unsigned calls=mipi_calls, before_log=logs;
    output=(void*)23;
    assert(esp_lcd_new_panel_st7701_mipi_only(expected_io,NULL,&output)==ESP_ERR_INVALID_ARG);
    assert(esp_lcd_new_panel_st7701_mipi_only(expected_io,&cfg,NULL)==ESP_ERR_INVALID_ARG);
    cfg.vendor_config=NULL;
    assert(esp_lcd_new_panel_st7701_mipi_only(expected_io,&cfg,&output)==ESP_ERR_INVALID_ARG);
    cfg.vendor_config=&vendor; vendor.flags.use_mipi_interface=0;
    assert(esp_lcd_new_panel_st7701_mipi_only(expected_io,&cfg,&output)==ESP_ERR_INVALID_ARG);
    assert(mipi_calls==calls && output==(void*)23 && logs==before_log+4);
    vendor.flags.use_mipi_interface=1;
    vendor.flags.skip_mipi_id_read=1;
    vendor.init_cmds_size=7;
    driver_result=-41;
    assert(esp_lcd_new_panel_st7701_mipi_only(expected_io,&cfg,&output)==-41);
    assert(mipi_calls==calls+1 && output==(void*)19 && logs==before_log+5);
    assert(vendor.flags.skip_mipi_id_read==1 && vendor.init_cmds_size==7);
    driver_result=ESP_OK; expected_io=NULL;
    assert(esp_lcd_new_panel_st7701_mipi_only(NULL,&cfg,&output)==ESP_OK);
    assert(mipi_calls==calls+2 && logs==before_log+6);
#endif
    (void)rgb_calls; (void)mipi_calls; (void)driver_result;
    return 0;
}
'''


@pytest.mark.parametrize("rgb,mipi", [(0, 0), (1, 0), (0, 1), (1, 1)])
def test_actual_entry_validation_dispatch_and_support_gating(tmp_path, rgb, mipi):
    command = prepare(tmp_path) + [f"-DSOC_LCD_RGB_SUPPORTED={rgb}", f"-DSOC_MIPI_DSI_SUPPORTED={mipi}"]
    if not rgb and not mipi:
        # The unchanged generic dispatcher has no use for io on this target.
        command.append("-Wno-unused-parameter")
    harness = tmp_path / "harness.c"
    harness.write_text(HARNESS)
    binary = tmp_path / "entry-test"
    subprocess.run(command + [str(COMPONENT / "esp_lcd_st7701.c"), str(harness), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
    if not mipi:
        probe = tmp_path / "unsupported.c"
        probe.write_text('#include "esp_lcd_st7701.h"\nvoid *probe = &esp_lcd_new_panel_st7701_mipi_only;\n')
        result = subprocess.run(command + ["-c", str(probe), "-o", str(tmp_path / "unsupported.o")], capture_output=True, text=True)
        assert result.returncode != 0 and "esp_lcd_new_panel_st7701_mipi_only" in result.stderr


def test_mipi_entry_links_without_rgb_implementation(tmp_path):
    command = prepare(tmp_path) + ["-DSOC_LCD_RGB_SUPPORTED=1", "-DSOC_MIPI_DSI_SUPPORTED=1", "-DMIPI_LINK_ONLY"]
    harness = tmp_path / "mipi-only.c"
    harness.write_text(HARNESS)
    binary = tmp_path / "mipi-only"
    gc_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    # Deliberately provide no RGB implementation: the generic entry's reference
    # must be discardable when the caller only uses the fixed MIPI entry.
    subprocess.run(command + [str(COMPONENT / "esp_lcd_st7701.c"), str(harness), gc_flag, "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_fixed_board_selects_public_mipi_entry():
    bsp = (ROOT / "components/esp32_p4_wifi6_touch_lcd_4_3/esp32_p4_wifi6_touch_lcd_4_3.c").read_text()
    assert ".use_mipi_interface = 1" in bsp
    assert "esp_lcd_new_panel_st7701_mipi_only(io, &lcd_dev_config, &panel_handle)" in bsp
    assert "esp_lcd_new_panel_st7701(io," not in bsp
