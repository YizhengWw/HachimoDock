"""Exercise the production ID-based activation helper independently of storage."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_media_activation_uses_exact_catalog_id(tmp_path):
    source = (ROOT / "main/pet_p4_miniapp.c").read_text()
    start = source.index("bool pet_p4_miniapp_catalog_activate_id(")
    end = source.index("\n}\n", start) + 3
    harness = tmp_path / "activate.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static const char *g_catalog[]={"stocks","other","third","fourth","fifth","music-player"};
static size_t g_catalog_count=6, activated=999;
static int catalog_find(const char **items,size_t count,const char *id){
  for(size_t i=0;i<count;i++)if(!strcmp(items[i],id))return (int)i;
  return -1;
}
static bool activate_catalog_index(size_t index){activated=index;return true;}
''' + source[start:end] + r'''
int main(void){
  assert(pet_p4_miniapp_catalog_activate_id("music-player") && activated==5);
  assert(pet_p4_miniapp_catalog_activate_id("stocks") && activated==0);
  assert(pet_p4_miniapp_catalog_activate_id("other") && activated==1);
  assert(!pet_p4_miniapp_catalog_activate_id("missing") && activated==1);
  assert(!pet_p4_miniapp_catalog_activate_id(NULL));
  assert(!pet_p4_miniapp_catalog_activate_id(""));
}
''')
    exe = tmp_path / "activate"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    protocol = (ROOT / "main/pet_p4_protocol.c").read_text()
    block = protocol.split('if(!strcmp(topic,"media/begin")', 1)[1].split('strcmp(topic, "audio/query")', 1)[0]
    assert 'pet_p4_miniapp_catalog_activate_id("music-player")' in block
    assert "pet_p4_miniapp_catalog_move" not in block


def test_builtin_sync_does_not_reject_new_components():
    import json
    bundle = json.loads((ROOT / "main/pet_p4_builtin_components.json").read_text())
    assert any(item["id"] == "music-player" for item in bundle["components"])
    source = (ROOT / "main/pet_p4_miniapp.c").read_text()
    sync = source.split("esp_err_t pet_p4_miniapp_sync_builtins(void)", 1)[1]
    assert "cJSON_GetArraySize(components) !=" not in sync
    assert "cJSON_GetArraySize(components) > PET_P4_MINIAPP_CATALOG_MAX" in sync
