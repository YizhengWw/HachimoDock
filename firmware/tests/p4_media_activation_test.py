"""Exercise the production ID-based activation helper independently of storage."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_reopening_loaded_media_does_not_write_flash_or_reset_images(tmp_path):
    source = (ROOT / "main/pet_p4_miniapp.c").read_text()
    start = source.index("static bool resume_catalog_index(size_t index)")
    end = source.index("\n}\n", start) + 3
    harness = tmp_path / "resume_media.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define miniapp_runtime_lock() ((void)0)
#define miniapp_runtime_unlock() ((void)0)
typedef struct {bool active,media;char widget_id[48];int retained;unsigned save_version;} miniapp_runtime_t;
typedef struct {char widget_id[48];unsigned slot;bool media;} item_t;
static miniapp_runtime_t g_runtime;
static item_t g_catalog[]={{"city-radio",0,true},{"other-radio",1,true},{"game",2,false},{"music-player",3,true}};
static size_t g_catalog_selected;
''' + source[start:end] + r'''
int main(void){
  g_runtime=(miniapp_runtime_t){.active=true,.media=true,.retained=42};
  strcpy(g_runtime.widget_id,"city-radio");g_catalog_selected=2;
  for(int i=0;i<100;i++)assert(resume_catalog_index(0));
  assert(g_catalog_selected==0 && g_runtime.retained==42);
  assert(!resume_catalog_index(1) && g_catalog_selected==0 && g_runtime.retained==42);
  g_runtime.media=false;g_runtime.save_version=1;g_runtime.retained=123;
  assert(resume_catalog_index(0) && g_runtime.retained==123);
  g_runtime.save_version=0;assert(!resume_catalog_index(0));
  g_runtime.media=true;g_runtime.active=false;assert(!resume_catalog_index(0));
}
''')
    exe = tmp_path / "resume_media"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-include", str(ROOT / "main/pet_p4_widget_trace.h"), str(harness), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


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
#define miniapp_runtime_lock() ((void)0)
#define miniapp_runtime_unlock() ((void)0)
static const char *g_catalog[]={"stocks","other","third","fourth","fifth","music-player"};
static size_t g_catalog_count=6, activated=999;
static int catalog_find(const char **items,size_t count,const char *id){
  for(size_t i=0;i<count;i++)if(!strcmp(items[i],id))return (int)i;
  return -1;
}
static bool activate_catalog_index(size_t index){activated=index;return true;}
static struct {bool active;} g_staging;
static bool storage_busy;
static bool pet_p4_miniapp_storage_busy(void){return storage_busy;}
static bool safe_widget_id(const char*id){return id&&id[0]&&strlen(id)<48;}
''' + source[start:end] + r'''
int main(void){
  storage_busy=true;assert(!pet_p4_miniapp_catalog_activate_id("stocks")&&activated==999);storage_busy=false;
  assert(pet_p4_miniapp_catalog_activate_id("music-player") && activated==5);
  assert(pet_p4_miniapp_catalog_activate_id("stocks") && activated==0);
  assert(pet_p4_miniapp_catalog_activate_id("other") && activated==1);
  assert(!pet_p4_miniapp_catalog_activate_id("missing") && activated==1);
  assert(!pet_p4_miniapp_catalog_activate_id(NULL));
  assert(!pet_p4_miniapp_catalog_activate_id(""));
  g_staging.active=true;assert(!pet_p4_miniapp_catalog_activate_id("stocks")&&activated==1);
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
    assert not any(item["id"] == "music-player" for item in bundle["components"])
    assert {"cloud-hop","daily-almanac","book-pocket"}.issubset({item["id"] for item in bundle["components"]})
    source = (ROOT / "main/pet_p4_miniapp.c").read_text()
    sync = source.split("esp_err_t pet_p4_miniapp_sync_builtins(void)", 1)[1]
    assert "cJSON_GetArraySize(components) !=" not in sync
    assert "cJSON_GetArraySize(components) > PET_P4_MINIAPP_CATALOG_MAX" in sync
