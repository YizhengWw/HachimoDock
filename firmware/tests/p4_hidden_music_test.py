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
        "static bool catalog_entry_visible(", "static int catalog_visible_index(",
        "static int catalog_visible_preferred(", "size_t pet_p4_miniapp_catalog_count(",
        "size_t pet_p4_miniapp_catalog_selected(", "bool pet_p4_miniapp_catalog_move(",
        "bool pet_p4_miniapp_catalog_activate_selected(",
    ]]
    harness = tmp_path / "catalog.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static struct { const char *widget_id; } g_catalog[4] = {
  {"stock-watchlist"},{"music-player"},{"wooden-fish"},{"upcoming-todos"}
};
static size_t g_catalog_count=4, g_catalog_selected=0, opened=99;
static bool activate_catalog_index(size_t index) {opened=index;return true;}
''' + "\n".join(functions) + r'''
int main(void) {
  assert(pet_p4_miniapp_catalog_count()==3);
  assert(catalog_visible_index(0)==0 && catalog_visible_index(1)==2 && catalog_visible_index(2)==3);
  assert(catalog_visible_index(3)==-1 && catalog_visible_preferred(1)==0);
  assert(pet_p4_miniapp_catalog_move(1) && g_catalog_selected==2);
  assert(pet_p4_miniapp_catalog_selected()==1);
  assert(pet_p4_miniapp_catalog_activate_selected() && opened==2);
  assert(pet_p4_miniapp_catalog_move(-1) && g_catalog_selected==0);
  assert(pet_p4_miniapp_catalog_move(-1) && g_catalog_selected==3);
  assert(g_catalog_count==4 && !strcmp(g_catalog[1].widget_id,"music-player"));
  g_catalog_count=1; g_catalog_selected=0; g_catalog[0].widget_id="music-player";
  assert(pet_p4_miniapp_catalog_count()==0 && catalog_visible_preferred(0)==-1);
  assert(!pet_p4_miniapp_catalog_move(1));
  g_catalog_count=0; assert(catalog_visible_index(0)==-1);
}
''')
    exe = tmp_path / "catalog"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    assert "preferred = catalog_visible_preferred(preferred);" in source
    assert "if (!catalog_entry_visible(index)) return false;" in source
