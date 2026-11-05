"""Actual NVS selection adapter: epochs, idempotence, fail-closed and old data isolation."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]
def test_small_selection_record_preserves_catalog_epochs(tmp_path):
    (tmp_path/'nvs.h').write_text('''#pragma once
#include <stddef.h>
typedef int esp_err_t;typedef int nvs_handle_t;
enum {ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,ERROR=2,NVS_READONLY=3,NVS_READWRITE=4};
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_commit(nvs_handle_t);void nvs_close(nvs_handle_t);
''')
    source=tmp_path/'selection.c';source.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "pet_p4_selection.c"
static selection_t disk,pending;static bool exists,open_error,get_error,set_error,commit_error;
static unsigned writes,commits;
esp_err_t nvs_open(const char*name,int mode,nvs_handle_t*out){assert(!strcmp(name,"p4_widgets"));*out=1;if(open_error)return ERROR;if(mode==NVS_READONLY&&!exists)return ESP_ERR_NVS_NOT_FOUND;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char*key,void*out,size_t*size){assert(h==1&&!strcmp(key,"selection"));if(get_error)return ERROR;if(!exists)return ESP_ERR_NVS_NOT_FOUND;assert(*size==sizeof(disk));memcpy(out,&disk,sizeof(disk));return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char*key,const void*v,size_t size){assert(h==1&&!strcmp(key,"selection")&&size==sizeof(pending));writes++;if(set_error)return ERROR;memcpy(&pending,v,size);return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);commits++;if(commit_error)return ERROR;disk=pending;exists=true;return ESP_OK;}
void nvs_close(nvs_handle_t h){assert(h==1);}
int main(void){
 char out[48];bool has=true;assert(!pet_selection_read(10,out,&has)&&!has&&!*out);
 assert(!pet_selection_write(10,"../private")&&!pet_selection_write(10,"Upper")&&!pet_selection_write(10,NULL)&&writes==0);
 assert(pet_selection_write(10,"first")&&writes==1&&commits==1);
 for(int i=0;i<100;i++)assert(pet_selection_write(10,"first"));
 assert(writes==1&&commits==1);
 assert(pet_selection_read(10,out,&has)&&has&&!strcmp(out,"first"));
 assert(!pet_selection_read(11,out,&has)&&has&&!*out); // New catalog wins, no stale legacy fallback.
 assert(pet_selection_write(11,"second")&&writes==2&&commits==2);
 commit_error=true;assert(!pet_selection_write(11,"third"));commit_error=false;
 assert(pet_selection_read(11,out,&has)&&!strcmp(out,"second"));
 set_error=true;assert(!pet_selection_write(11,"third"));set_error=false;
 assert(pet_selection_read(11,out,&has)&&!strcmp(out,"second"));
 memset(disk.id,'x',48);assert(!pet_selection_read(11,out,&has)&&has&&!*out);
 get_error=true;assert(!pet_selection_read(11,out,&has)&&has);get_error=false;
 open_error=true;assert(!pet_selection_read(11,out,&has)&&has&&!pet_selection_write(11,"third"));
 return 0;
}
''')
    exe=tmp_path/'selection'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(tmp_path),'-I'+str(ROOT/'main'),str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    c=(ROOT/'main/pet_p4_miniapp.c').read_text()
    activation=c.split('static pet_p4_miniapp_activation_result_t activate_catalog_index_prepared(',1)[1].split('static bool remove_component(',1)[0]
    assert 'write_file_atomic(' not in activation and 'pet_selection_write(sequence,item.widget_id)' in activation
    assert activation.index('g_runtime = *next') < activation.index('pet_selection_write(sequence,item.widget_id)')
    navigation=function(c,'catalog_restore_navigation')
    assert '!*selection_exists&&read_file(MINIAPP_LAST_ID_PATH' in navigation
    assert 'pet_selection_read(g_catalog_sequence,last_opened,selection_exists)' in navigation
    assert 'activated&&preferred>=0&&!selection_exists' in c
    assert '!pet_selection_write(g_catalog_sequence,g_catalog[preferred].widget_id)' in c
