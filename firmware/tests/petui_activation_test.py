"""Execute the real navigation handler; never install/delete packages or use audio."""
import subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def test_installed_activation_is_checked_and_audio_safe(tmp_path):
    source=(ROOT/'main/pet_p4_protocol.c').read_text()
    body=source.split('} else if (strcmp(topic,"widget/activate")==0) {',1)[1].split('} else if (strcmp(topic, "widget/delete") == 0)',1)[0]
    harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
typedef void(*send_fn)(const char*,void*);
typedef struct{char screen_page[16];unsigned last_update_ms;int stats;} state_t;
static bool busy;static int calls;static bool ack;
static bool pet_p4_audio_active(void){return busy;}
static bool pet_p4_audio_playback_active(void){return false;}
static bool pet_p4_audio_conversation_active(void){return false;}
static bool pet_p4_audio_music_active(void){return false;}
static bool pet_p4_miniapp_catalog_activate_id(const char*id){calls++;return !strcmp(id,"existing");}
static void pet_p4_miniapp_sync_stats(int *stats){*stats=7;}
static const char*json_string(const cJSON*p,const char*k){const cJSON*v=cJSON_GetObjectItemCaseSensitive(p,k);return cJSON_IsString(v)?v->valuestring:"";}
static void copy_text(char*out,size_t n,const char*s){snprintf(out,n,"%s",s);}
static void send_protocol_ack(send_fn send,void*ctx,const char*topic,const cJSON*p,bool ok,const char*code,const char*message){(void)send;(void)ctx;(void)topic;(void)p;(void)code;(void)message;ack=ok;}
static bool run(state_t*state,const cJSON*payload){send_fn send_line=NULL;void*ctx=NULL;const char*topic="widget/activate";bool ok=false;
'''+body+r'''
return ok;}
int main(void){
 state_t state={.screen_page="main"};cJSON*p=cJSON_Parse("{\"widgetId\":\"existing\",\"show\":true}");
 busy=true;assert(!run(&state,p)&&calls==0&&!ack&&!strcmp(state.screen_page,"main"));
 busy=false;assert(run(&state,p)&&ack&&calls==1&&!strcmp(state.screen_page,"app")&&state.stats==7);
 cJSON_Delete(p);p=cJSON_Parse("{\"widgetId\":\"existing\",\"show\":3}");assert(!run(&state,p)&&calls==1);
 cJSON_Delete(p);p=cJSON_Parse("{\"widgetId\":\"existing\",\"path\":\"no\"}");assert(!run(&state,p)&&calls==1);
 cJSON_Delete(p);p=cJSON_Parse("{\"widgetId\":\"missing\"}");assert(!run(&state,p)&&!ack&&calls==2);
 cJSON_Delete(p);strcpy(state.screen_page,"main");p=cJSON_Parse("{\"widgetId\":\"existing\"}");assert(run(&state,p)&&!strcmp(state.screen_page,"main"));cJSON_Delete(p);
 return 0;
}
'''
    vendor=ROOT/'tests/vendor/cjson';path=tmp_path/'activate.c';path.write_text(harness);exe=tmp_path/'activate'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-fsanitize=address,undefined','-I',str(vendor),str(path),str(vendor/'cJSON.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    miniapp=(ROOT/'main/pet_p4_miniapp.c').read_text().split('bool pet_p4_miniapp_catalog_activate_id(',1)[1].split('\n}',1)[0]
    assert '!safe_widget_id(widget_id)||g_staging.active' in miniapp
