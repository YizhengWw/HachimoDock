"""Production media persistence: empty stages cannot trigger device-wide scans."""
from pathlib import Path
import subprocess


def test_empty_media_install_skips_budget_scan_without_bypassing_storage_guards(tmp_path):
    root = Path(__file__).resolve().parents[1]
    (tmp_path / "freertos").mkdir()
    (tmp_path / "esp_heap_caps.h").write_text("#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n#define heap_caps_malloc(n,c) malloc(n)\n")
    (tmp_path / "esp_log.h").write_text("#define ESP_LOGW(tag,...) ((void)(tag))\n")
    (tmp_path / "freertos/FreeRTOS.h").write_text("#include <assert.h>\n#define configASSERT(x) assert(x)\n#define pdTRUE 1\n#define portMAX_DELAY 0xffffffffu\n")
    (tmp_path / "freertos/semphr.h").write_text("typedef void *SemaphoreHandle_t;\nstatic inline void *xSemaphoreCreateMutex(void){return (void*)1;}\n#define xSemaphoreTake(a,b) ((void)(a),(void)(b),1)\n#define xSemaphoreGive(a) ((void)(a))\n#define vSemaphoreDelete(a) ((void)(a))\n")
    exe = tmp_path / "media-budget"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-variable", "-Wno-deprecated-declarations",
                    "-fsanitize=address,undefined", "-I", str(tmp_path),
                    "-I", str(root / "main"), "-I", str(root / "tests/vendor/cjson"),
                    str(root / "tests/component_media_budget_test.c"),
                    *[str(root / "main" / name) for name in
                      ("pet_p4_component_ext.c", "pet_p4_component_events.c", "pet_p4_sfx_core.c")],
                    str(root / "tests/vendor/cjson/cJSON.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
