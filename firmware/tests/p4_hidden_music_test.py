"""Exercise production catalog projection without deleting persisted packages."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def test_hidden_music_keeps_storage_and_navigation_indices(tmp_path):
    source = (ROOT / "main/pet_p4_miniapp.c").read_text()
    def function(signature):
        start = source.index(signature)
        end = source.index("{", start) + 1
        depth = 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[start:end]
    functions = [function(name) for name in [
        "static bool catalog_item_visible(", "static int catalog_visible_preferred_in(",
        "static bool catalog_entry_visible(", "static int catalog_visible_index(",
        "static int catalog_visible_preferred(", "size_t pet_p4_miniapp_catalog_count(",
        "size_t pet_p4_miniapp_catalog_capacity(",
        "size_t pet_p4_miniapp_catalog_selected(", "bool pet_p4_miniapp_catalog_move(",
        "bool pet_p4_miniapp_catalog_select(",
        "bool pet_p4_miniapp_catalog_activate_selected(",
    ]]
    harness = tmp_path / "catalog.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
#define miniapp_runtime_lock() ((void)0)
#define miniapp_runtime_unlock() ((void)0)
#define PET_P4_MINIAPP_CATALOG_MAX 32
typedef struct { const char *widget_id; } miniapp_catalog_item_t;
static miniapp_catalog_item_t g_catalog[4] = {
  {"stock-watchlist"},{"music-player"},{"wooden-fish"},{"upcoming-todos"}
};
static size_t g_catalog_count=4, g_catalog_selected=0, opened=99;
static void copy_utf8(char*out,size_t n,const char*id){snprintf(out,n,"%s",id);}
/* Isolate the cursor queue boundary; this projection test never performs IO. */
static bool catalog_select_id_queued(const char*id){
 for(size_t i=0;i<g_catalog_count;i++)if(!strcmp(g_catalog[i].widget_id,id)){g_catalog_selected=i;return true;}
 return false;
}
static bool activate_catalog_index(size_t index) {opened=index;return true;}
''' + "\n".join(functions) + r'''
int main(void) {
  assert(pet_p4_miniapp_catalog_count()==3);
  assert(pet_p4_miniapp_catalog_capacity()==31);
  assert(catalog_visible_index(0)==0 && catalog_visible_index(1)==2 && catalog_visible_index(2)==3);
  assert(catalog_visible_index(3)==-1 && catalog_visible_preferred(1)==0);
  assert(pet_p4_miniapp_catalog_move(1) && g_catalog_selected==2);
  assert(pet_p4_miniapp_catalog_selected()==1);
  assert(pet_p4_miniapp_catalog_activate_selected() && opened==2);
  assert(pet_p4_miniapp_catalog_move(-1) && g_catalog_selected==0);
  assert(pet_p4_miniapp_catalog_move(-1) && g_catalog_selected==3);
  assert(pet_p4_miniapp_catalog_select(1)&&g_catalog_selected==2);
  assert(pet_p4_miniapp_catalog_activate_selected()&&opened==2);
  assert(!pet_p4_miniapp_catalog_select(3)&&g_catalog_selected==2);
  assert(g_catalog_count==4 && !strcmp(g_catalog[1].widget_id,"music-player"));
  g_catalog[0].widget_id="music-player";g_catalog[1].widget_id="visible-first";
  assert(pet_p4_miniapp_catalog_select(0)&&g_catalog_selected==1);
  assert(pet_p4_miniapp_catalog_activate_selected()&&opened==1);
  assert(pet_p4_miniapp_catalog_selected()==0&&g_catalog_count==4);
  g_catalog_count=1; g_catalog_selected=0; g_catalog[0].widget_id="music-player";
  assert(pet_p4_miniapp_catalog_count()==0 && catalog_visible_preferred(0)==-1);
  assert(pet_p4_miniapp_catalog_capacity()==31);
  assert(!pet_p4_miniapp_catalog_move(1));
  g_catalog_count=0; assert(catalog_visible_index(0)==-1);
  assert(pet_p4_miniapp_catalog_capacity()==32);
}
''')
    exe = tmp_path / "catalog"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    assert "preferred = catalog_visible_preferred(preferred);" in source
    # Recovery quarantine is an additional refusal, not a change to hidden IDs.
    activation = function("static bool activate_catalog_index_in_memory(")
    assert "if (g_catalog_recovery_blocked||!catalog_entry_visible(index)) return false;" in activation
    assert activation.index("!catalog_entry_visible(index)") < activation.index("calloc(")
