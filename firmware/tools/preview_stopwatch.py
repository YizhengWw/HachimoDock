"""Render the production round-screen backend on the host (no board required).

Requires the LVGL component resolved by a firmware configuration, CMake,
a native C/C++ compiler, FFmpeg and Pillow. Generated host shims and media live
under firmware/build/stopwatch-preview, never in the source tree.
"""
import argparse
import pathlib
import subprocess
import sys
from build_stopwatch_assets import build


def preview(output, pattern="pet"):
    firmware = pathlib.Path(__file__).resolve().parents[1]
    work = firmware / "build/stopwatch-preview"
    work.mkdir(parents=True, exist_ok=True)
    lvgl = firmware / "managed_components/lvgl__lvgl"
    if not lvgl.exists():
        raise RuntimeError("Configure/build the StopWatch firmware first to resolve LVGL")
    pack = work / "stopwatch_pet.bin"
    build(pack)
    (work / "esp_err.h").write_text(
        "#pragma once\n"
        "typedef int esp_err_t;\n"
        "#define ESP_OK 0\n#define ESP_ERR_NO_MEM 1\n#define ESP_ERR_INVALID_STATE 2\n")
    (work / "esp_heap_caps.h").write_text(
        "#pragma once\n#include <stdlib.h>\n"
        "#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n"
        "#define heap_caps_malloc(n,c) malloc(n)\n"
        "#define heap_caps_calloc(n,s,c) calloc(n,s)\n")
    (work / "lv_conf.h").write_text(
        "#ifndef LV_CONF_H\n#define LV_CONF_H\n"
        "#define LV_USE_TJPGD 1\n#define LV_COLOR_DEPTH 16\n"
        "#define LV_USE_OS LV_OS_NONE\n#define LV_USE_THORVG_INTERNAL 0\n#endif\n")
    prefix = "_" if sys.platform == "darwin" else ""
    section = ".section __DATA,__const" if sys.platform == "darwin" else ".section .rodata"
    (work / "pack.S").write_text(
        section + "\n.global " + prefix + "_binary_stopwatch_pet_bin_start\n"
        ".global " + prefix + "_binary_stopwatch_pet_bin_end\n" +
        prefix + '_binary_stopwatch_pet_bin_start:\n.incbin "' + pack.as_posix() + '"\n' +
        prefix + "_binary_stopwatch_pet_bin_end:\n")
    (work / "preview.c").write_text(r'''
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "lvgl.h"
#include "board_stopwatch.h"
#include "stopwatch_renderer.h"
static pet_device_sessions_t model;
esp_err_t stopwatch_board_present(const uint16_t *pixels) {
  FILE *f = fopen("preview.ppm","wb");
  assert(f);
  fprintf(f,"P6\n466 466\n255\n");
  unsigned colored = 0;
  for (unsigned i=0;i<466*466;++i) {
    unsigned p=pixels[i];
    unsigned char rgb[] = {((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};
    if (p) ++colored;
    fwrite(rgb,1,3,f);
  }
  fclose(f);
  assert(colored > 5000); // Detect missing/failed factory JPEG decode.
  return ESP_OK;
}
int main(int argc, char **argv) {
  lv_init();
  assert(stopwatch_renderer_init()==ESP_OK);
  cJSON *payload=cJSON_Parse("{\"agentId\":\"codex\",\"sessions\":["
    "{\"id\":\"preview\",\"title\":\"适配 StopWatch 圆屏\","
    "\"content\":\"正在运行 HachimoDock 会话\",\"state\":\"working\"}]}");
  assert(pet_device_sessions_handle(&model,"session/current",payload,1000));
  cJSON_Delete(payload);
  int pattern = argc > 1 ? atoi(argv[1]) : 0;
  if (pattern) assert(stopwatch_renderer_diagnostic(pattern,233,233)==ESP_OK);
  else assert(stopwatch_renderer_draw(&model,1000,3980)==ESP_OK);
  return 0;
}
''')
    (work / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.16)
project(stopwatch_preview LANGUAGES C CXX ASM)
set(LV_BUILD_CONF_PATH "{work.as_posix()}/lv_conf.h" CACHE STRING "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("{lvgl.as_posix()}" lvgl)
add_executable(preview
  preview.c pack.S
  "{firmware.as_posix()}/targets/stopwatch/main/stopwatch_renderer.c"
  "{firmware.as_posix()}/main/pet_device_sessions.c"
  "{firmware.as_posix()}/main/pet_p4_font_cn.c"
  "{firmware.as_posix()}/tests/vendor/cjson/cJSON.c")
target_include_directories(preview PRIVATE
  "{work.as_posix()}"
  "{lvgl.as_posix()}/src"
  "{firmware.as_posix()}/targets/stopwatch/components/board_stopwatch/include"
  "{firmware.as_posix()}/targets/stopwatch/main"
  "{firmware.as_posix()}/main"
  "{firmware.as_posix()}/tests/vendor/cjson")
target_link_libraries(preview PRIVATE lvgl m)
''')
    subprocess.run(["cmake", "-S", str(work), "-B", str(work / "native")], check=True)
    subprocess.run(["cmake", "--build", str(work / "native"), "--parallel", "4"], check=True)
    pattern_id = {"pet": 0, "colorbars": 1, "touch": 2}[pattern]
    subprocess.run([str(work / "native/preview"), str(pattern_id)], cwd=work, check=True, timeout=20)
    from PIL import Image
    output.parent.mkdir(parents=True, exist_ok=True)
    Image.open(work / "preview.ppm").save(output)
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--pattern", choices=["pet", "colorbars", "touch"], default="pet")
    args = parser.parse_args()
    preview(args.output.resolve(), args.pattern)
