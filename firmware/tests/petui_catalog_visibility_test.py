"""Navigation and deletion must share the existing visibility policy."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_hidden_legacy_entry_cannot_be_the_removal_fallback(tmp_path):
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    helpers=code[code.index('static bool catalog_item_visible('):code.index('static bool catalog_entry_visible(')]
    source=tmp_path/'visible.c';exe=tmp_path/'visible'
    source.write_text('''#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef struct {char widget_id[48];} miniapp_catalog_item_t;
'''+helpers+'''
int main(void){
 miniapp_catalog_item_t next[3]={{.widget_id="music-player"},{.widget_id="first-visible"},{.widget_id="second-visible"}};
 assert(catalog_visible_preferred_in(next,3,0)==1);
 assert(catalog_visible_preferred_in(next,3,2)==2);
 assert(catalog_visible_preferred_in(next,3,99)==1);
 assert(catalog_visible_preferred_in(next,1,0)==-1);
 assert(catalog_visible_preferred_in(next,0,-1)==-1);
 return 0;
}''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    remove=code[code.index('static bool remove_component('):]
    assert 'catalog_visible_preferred_in(next_catalog,next_count,(int)next_selected)' in remove
