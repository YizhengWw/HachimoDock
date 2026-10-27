"""Production boot gate and catalog ordering; no device or persistent writes."""
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
def test_pagination_is_single_equal_circle_and_retirement_can_preserve_saves():
    renderer=(ROOT/'main/pet_p4_renderer.c').read_text()
    pagination=renderer.split('for (size_t page=0; page<pages; ++page)',1)[1].split('fill_round_rect(24,435',1)[0]
    assert 'fill_ellipse(dot_x, 415, 3, 3,' in pagination
    assert 'pages-1' in pagination
    assert 'fill_round_rect' not in pagination
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    assert 'if (!preserve_save) pet_app_store_forget(widget_id);' in code
    assert 'return remove_component(widget_id, error, error_size, true);' in code
    protocol=(ROOT/'main/pet_p4_protocol.c').read_text()
    assert '"widgetDeletePreserveSave", true' in protocol
    assert 'cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload,"preserveSave"))' in protocol

def test_boot_preinstallation_is_independent_of_touch_but_external_install_is_not(tmp_path):
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    start=code.index('static bool component_install_touch_allowed(')
    fn=code[start:code.index('\n}\n',start)+3]
    harness=tmp_path/'gate.c';exe=tmp_path/'gate'
    harness.write_text('''#include <stdbool.h>
#include <assert.h>
static bool ready,g_builtin_sync_in_progress;
static bool pet_p4_touch_ready(void){return ready;}
'''+fn+'''
int main(void){
 assert(component_install_touch_allowed(false));
 assert(!component_install_touch_allowed(true));
 g_builtin_sync_in_progress=true;
 assert(component_install_touch_allowed(true));
 g_builtin_sync_in_progress=false;
 assert(!component_install_touch_allowed(true));
 ready=true;assert(component_install_touch_allowed(true));return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(harness),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    install=code.split('bool pet_p4_miniapp_install_commit(',1)[1].split('static bool commit_builtin_package(',1)[0]
    assert 'component_install_touch_allowed(next->slicing)' in install
    sync=code.split('esp_err_t pet_p4_miniapp_sync_builtins(void)',1)[1].split('static bool remove_file_if_present',1)[0]
    assert sync.count('failed += 1;\n')==2
    assert sync.index('if (failed)')<sync.index('MINIAPP_BUILTIN_MARKER_TMP_PATH')
    assert 'continue;' in sync.split('failed to sync built-in id=%s: %s',1)[1].split('catalog_changed = true;',1)[0]

def test_actual_ordering_preserves_slots_custom_order_and_active_selection(tmp_path):
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    fn=code[code.index('static bool reorder_catalog_for_builtin_bundle('):code.index('esp_err_t pet_p4_miniapp_sync_builtins(void)')]
    harness=tmp_path/'order.c';exe=tmp_path/'order'
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#define PET_P4_MINIAPP_CATALOG_MAX 32
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
typedef struct {char widget_id[48];int slot,checksum;} miniapp_catalog_item_t;
static miniapp_catalog_item_t g_catalog[32]={{"book",5,50},{"custom-new",9,90},{"radio",2,20},{"custom-old",8,80},{"news",7,70}};
static size_t g_catalog_count=5,g_catalog_selected;
static struct {bool active;char widget_id[48];}g_runtime={true,"book"};
static void catalog_scratch_free(miniapp_catalog_item_t **p){free(*p);}
static const char *json_string(const cJSON *j,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static int catalog_find(const miniapp_catalog_item_t *a,size_t n,const char *id){for(size_t i=0;i<n;i++)if(!strcmp(a[i].widget_id,id))return i;return -1;}
'''+fn+r'''
int main(void){
 cJSON *list=cJSON_Parse("[{\"id\":\"radio\"},{\"id\":\"missing\"},{\"id\":\"news\"},{\"id\":\"book\"}]");
 assert(reorder_catalog_for_builtin_bundle(list));
 const char *ids[]={"custom-new","custom-old","radio","news","book"};int slots[]={9,8,2,7,5};
 for(int i=0;i<5;i++){assert(!strcmp(ids[i],g_catalog[i].widget_id));assert(g_catalog[i].slot==slots[i]);assert(g_catalog[i].checksum==slots[i]*10);}
 assert(g_catalog_selected==4);assert(!reorder_catalog_for_builtin_bundle(list));assert(g_catalog_count==5);
 cJSON_Delete(list);return 0;
}
''')
    vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-fsanitize=address,undefined','-I',str(vendor),str(harness),str(vendor/'cJSON.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
