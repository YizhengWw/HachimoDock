"""Execute production per-component recovery; never corrupt a user's device."""
from pathlib import Path
import subprocess
import json
import sys

ROOT = Path(__file__).resolve().parents[1]

def test_partial_catalog_does_not_roll_back_unrelated_packages(tmp_path):
    code = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    recovery = code[code.index('static bool recover_catalog_items('):code.index('static bool catalog_entry_visible(')]
    validate = code[code.index('static bool validate_catalog_snapshot('):code.index('static bool load_catalog_snapshot(')]
    source = r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#define PET_P4_MINIAPP_CATALOG_MAX 32
#define ESP_LOGW(...) ((void)0)
typedef struct {char widget_id[48],title[64];uint8_t slot,package_generation;uint32_t widget_checksum,buttons_checksum,sprites_checksum;uint8_t resource_state,image_count;} miniapp_catalog_item_t;
typedef struct {bool parsed,valid,needs_upgrade;uint8_t file_generation;uint32_t sequence;char active_widget_id[48];miniapp_catalog_item_t items[32];size_t count;} miniapp_catalog_snapshot_t;
typedef struct {int unused;} miniapp_runtime_t;
static bool load_catalog_runtime_item(miniapp_catalog_item_t *i, miniapp_runtime_t *r,bool missing,char *e,size_t n){
 (void)r;(void)missing;(void)e;(void)n;
 if(i->widget_checksum==999)return false;
 i->resource_state=i->sprites_checksum?1:2;return true;
}
static int catalog_find(const miniapp_catalog_item_t*a,size_t n,const char*id){for(size_t i=0;i<n;i++)if(!strcmp(a[i].widget_id,id))return i;return -1;}
''' + validate + recovery + r'''
int main(void){
 miniapp_catalog_snapshot_t latest={.parsed=true,.count=3,.sequence=12},older={.parsed=true,.count=3,.sequence=11};
 for(int i=0;i<3;i++){
  snprintf(latest.items[i].widget_id,48,"component-%d",i);
  latest.items[i].slot=i;latest.items[i].widget_checksum=10+i;latest.items[i].buttons_checksum=20+i;latest.items[i].sprites_checksum=30+i;
 }
 older=latest;older.sequence=11;
 latest.items[0].widget_checksum=999;latest.items[1].widget_checksum=80;
 assert(!validate_catalog_snapshot(&latest));assert(latest.items[0].resource_state==3);
 assert(latest.items[1].resource_state==1&&latest.items[2].resource_state==1);
 assert(validate_catalog_snapshot(&older));
 assert(recover_catalog_items(&latest,&older));
 assert(latest.items[0].widget_checksum==10);assert(latest.items[1].widget_checksum==80);
 assert(latest.sequence==12&&latest.count==3);
 // Reproduce original all-or-nothing rejection: one common damaged component
 // made both catalogs invalid. New path retains all IDs/order and good files.
 latest.items[0].widget_checksum=999;older.items[0].widget_checksum=999;
 assert(!validate_catalog_snapshot(&latest));assert(!validate_catalog_snapshot(&older));
 assert(!recover_catalog_items(&latest,&older));assert(latest.count==3);
 assert(latest.items[1].widget_checksum==80&&latest.items[2].resource_state==1);
 // Known full package cannot recover to a bootstrap, an orphan, different ID
 // or another slot; deletion in newest catalog must not resurrect old IDs.
 older.items[0].widget_checksum=10;older.items[0].sprites_checksum=0;
 validate_catalog_snapshot(&older);assert(!recover_catalog_items(&latest,&older));
 older.items[0].sprites_checksum=30;validate_catalog_snapshot(&older);
 older.items[0].slot=4;assert(!recover_catalog_items(&latest,&older));
 older.items[0].slot=0;older.parsed=false;assert(!recover_catalog_items(&latest,&older));
 older.parsed=true;latest.count=0;assert(!recover_catalog_items(&latest,&older));assert(latest.count==0);
 return 0;
}
'''
    path=tmp_path/'recovery.c';path.write_text(source);exe=tmp_path/'recovery'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

def test_bootstrap_identity_matches_actual_cjson_wire_bytes(tmp_path):
    sys.path.insert(0,str(ROOT/'tools'))
    from build_builtin_bundle import render_bundle, render_data_source
    bundle=render_bundle();data=tmp_path/'bundle.c';data.write_text(render_data_source(bundle))
    fixture=tmp_path/'bundle.json';fixture.write_text(bundle)
    source=tmp_path/'identity.c';source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include "cJSON.h"
extern bool pet_p4_builtin_is_bootstrap(const char*,uint32_t);
int main(int argc,char**argv){(void)argc;FILE*f=fopen(argv[1],"rb");assert(f);char*b=calloc(1,170000);fread(b,1,169999,f);fclose(f);cJSON*j=cJSON_Parse(b);assert(j);cJSON*p;
cJSON_ArrayForEach(p,cJSON_GetObjectItemCaseSensitive(j,"components")){
 const char*id=cJSON_GetObjectItemCaseSensitive(p,"id")->valuestring;
 char*w=cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(p,"widget"));uint32_t h=2166136261u;for(char*c=w;*c;c++)h=(h^(uint8_t)*c)*16777619u;
 bool external=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"resourcePackage"));
 assert(pet_p4_builtin_is_bootstrap(id,h)==external);assert(!pet_p4_builtin_is_bootstrap("unrelated",h));assert(!pet_p4_builtin_is_bootstrap(id,h^1));free(w);
}cJSON_Delete(j);free(b);return 0;}
''')
    vendor=ROOT/'tests/vendor/cjson';exe=tmp_path/'identity'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I',str(vendor),str(source),str(data),str(vendor/'cJSON.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe),str(fixture)],check=True)

def test_navigation_and_invalid_catalog_never_trigger_destructive_reseed():
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    activate=code.split('static bool activate_catalog_index(size_t index)',1)[1].split('static bool remove_component(',1)[0]
    assert 'persist_catalog_snapshot(' not in activate
    assert 'MINIAPP_LAST_ID_TMP_PATH' in activate
    assert 'read_file(MINIAPP_LAST_ID_PATH, last_opened' in code
    init=code.split('esp_err_t pet_p4_miniapp_init(void)',1)[1].split('size_t pet_p4_miniapp_catalog_count(',1)[0]
    assert init.index('if (catalog_files_present)') < init.index('load_legacy_catalog(legacy)')
    assert 'if (snapshot0->parsed) selected = snapshot0;' in init
    assert 'if (g_catalog_recovery_blocked) return ESP_ERR_INVALID_STATE;' in code
    assert 'refused image-free bootstrap over existing full resource package' in code
    assert 'resources-incomplete' in (ROOT/'main/pet_p4_protocol.c').read_text()
    protocol=(ROOT/'main/pet_p4_protocol.c').read_text()
    assert '"complete", !pet_p4_miniapp_catalog_recovery_blocked()' in protocol
    assert 'const bool repair_resources = pet_p4_miniapp_resource_repair_active();' in protocol
    assert 'if (!repair_resources) copy_text(state->screen_page' in protocol

def test_damaged_package_repair_requires_exact_committed_payload(tmp_path):
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    helper=code[code.index('static bool resource_repair_package_matches('):code.index('bool pet_p4_miniapp_install_commit(')]
    source=tmp_path/'repair_identity.c'
    source.write_text('''#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
typedef struct {uint8_t resource_state;uint32_t widget_checksum,sprites_checksum;} miniapp_catalog_item_t;
''' + helper + '''
int main(void){
 miniapp_catalog_item_t i={3,123,456};
 assert(resource_repair_package_matches(&i,123,456));
 assert(!resource_repair_package_matches(&i,124,456));
 assert(!resource_repair_package_matches(&i,123,457));
 i.resource_state=1;assert(!resource_repair_package_matches(&i,123,456));
 i.resource_state=2;assert(resource_repair_package_matches(&i,999,456));
 assert(!resource_repair_package_matches(&i,999,0));
 assert(!resource_repair_package_matches(NULL,123,456));
}
''')
    exe=tmp_path/'repair_identity'
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
