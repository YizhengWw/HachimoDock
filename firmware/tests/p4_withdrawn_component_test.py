"""Run production visibility policy against old persisted entries, without device I/O."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_old_withdrawn_slot_is_hidden_and_cannot_activate_but_is_preserved(tmp_path):
    source = (ROOT / "main/pet_p4_miniapp.c").read_text()
    functions = [function(source, name) for name in [
        "static bool component_withdrawn(", "static bool catalog_entry_visible(",
        "static int catalog_visible_index(", "static int catalog_visible_preferred(",
        "size_t pet_p4_miniapp_catalog_count(", "size_t pet_p4_miniapp_catalog_selected(",
        "bool pet_p4_miniapp_catalog_select(", "bool pet_p4_miniapp_catalog_move(",
        "bool pet_p4_miniapp_catalog_activate_selected(", "bool pet_p4_miniapp_catalog_activate_id(",
    ]]
    harness = tmp_path / "withdrawn.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
typedef struct {const char *widget_id;int slot,save;} item_t;
static item_t g_catalog[]={{"city-radio",7,1234},{"wooden-fish",4,55},{"music-player",2,66},{"custom-radio",9,77}};
static size_t g_catalog_count=4,g_catalog_selected=0,opened=99;
static bool catalog_entry_visible(size_t index);
static bool activate_catalog_index(size_t index){if(!catalog_entry_visible(index))return false;opened=index;return true;}
static int catalog_find(const item_t *items,size_t count,const char *id){for(size_t i=0;i<count;i++)if(!strcmp(items[i].widget_id,id))return (int)i;return -1;}
''' + "\n".join(functions) + r'''
int main(void){
 item_t before[4];memcpy(before,g_catalog,sizeof(before));
 assert(component_withdrawn("city-radio"));assert(!component_withdrawn(NULL));
 assert(!component_withdrawn("custom-radio")&&!component_withdrawn("city-radio-custom"));
 assert(pet_p4_miniapp_catalog_count()==2);
 assert(catalog_visible_index(0)==1&&catalog_visible_index(1)==3&&catalog_visible_index(2)==-1);
 assert(catalog_visible_preferred(0)==1); /* Last active component was withdrawn. */
 assert(!pet_p4_miniapp_catalog_select(0));
 assert(!pet_p4_miniapp_catalog_activate_selected()&&opened==99);
 assert(!pet_p4_miniapp_catalog_activate_id("city-radio")&&opened==99);
 assert(pet_p4_miniapp_catalog_move(1)&&g_catalog_selected==1);
 assert(pet_p4_miniapp_catalog_selected()==0);
 assert(pet_p4_miniapp_catalog_move(1)&&g_catalog_selected==3);
 assert(pet_p4_miniapp_catalog_move(1)&&g_catalog_selected==1);
 assert(pet_p4_miniapp_catalog_activate_id("custom-radio")&&opened==3);
 assert(!memcmp(before,g_catalog,sizeof(before))&&g_catalog_count==4);
 g_catalog_count=1;g_catalog_selected=0;
 assert(pet_p4_miniapp_catalog_count()==0&&catalog_visible_preferred(0)==-1);
 assert(!pet_p4_miniapp_catalog_move(1)&&!pet_p4_miniapp_catalog_activate_selected());
 assert(g_catalog[0].slot==7&&g_catalog[0].save==1234);
 return 0;
}
''')
    executable = tmp_path / "withdrawn"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(harness), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


def test_every_activation_and_install_route_enforces_policy_without_deletion():
    source = (ROOT / "main/pet_p4_miniapp.c").read_text()
    for signature in ["static bool activate_catalog_index_in_memory(", "static bool activate_catalog_index("]:
        body = function(source, signature)
        assert body.index("if (!catalog_entry_visible(index)) return false;") < body.index("calloc(")
    begin = function(source, "bool pet_p4_miniapp_install_begin(")
    commit = function(source, "bool pet_p4_miniapp_install_commit(")
    assert begin.index("component_withdrawn(widget_id)") < begin.index("memset(&g_staging")
    assert commit.index("component_withdrawn(widget_id)") < commit.index("calloc(")
    boot = function(source, "esp_err_t pet_p4_miniapp_init(")
    assert boot.count("preferred = catalog_visible_preferred(preferred);") == 2
    assert "if (!catalog_entry_visible(0)) memset(&g_runtime, 0, sizeof(g_runtime));" in boot
    assert "catalog_visible_index(index)" in function(source, "bool pet_p4_miniapp_catalog_get(")
    retired = source.split("g_retired_builtin_ids[] = {", 1)[1].split("};", 1)[0]
    assert "city-radio" not in retired  # Do not route withdrawal through destructive migration.
