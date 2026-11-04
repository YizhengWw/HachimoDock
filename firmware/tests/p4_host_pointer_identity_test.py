"""Actual capture helpers used on both pointer boundaries, including fixed UI.

The production host's instance/variable-epoch implementation is separately run
by petui_resource_lifecycle_test; this checks caller routing and no-scroll guard.
No physical input, user data, audio, or device access.
"""
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def test_fixed_host_rows_capture_identity_without_viewports(tmp_path):
    source=(ROOT/'main/pet_p4_miniapp.c').read_text()
    start=source.index('static void host_pointer_capture(')
    end=source.index('void pet_p4_miniapp_pointer(',start)
    helpers=source[start:end]
    pointer=source[end:source.index('bool pet_p4_miniapp_media_active(',end)]
    assert pointer.count('host_pointer_capture(')==2
    assert 'pet_host_observation_equal(&press_host,&host_observed)' in pointer
    assert 'dispatch_action_observed(dispatch,event->ts_ms,dispatch_host_guarded?&dispatch_host:NULL)' in pointer
    dispatch=source[source.index('static bool dispatch_action_observed('):source.index('static bool button_matches_event(')]
    assert 'observed->revision!=g_image_revision' in dispatch
    assert 'pet_app_host_observation_current(g_runtime.widget_id,observed)' in dispatch
    assert 'pet_app_host_observed_action(id,rev,page,action,observed,now_ms)' in dispatch
    assert 'viewport' not in helpers
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pet_p4_host_observation.h"
static struct {bool app_host;char widget_id[48];} g_runtime;
static uint64_t epoch;static const char *context;static unsigned instance=1;
static bool pet_app_host_observe(const char *id,pet_host_observation_t *out){
 assert(!strcmp(id,"records"));memset(out,0,sizeof(*out));out->variable_epoch=epoch;out->incarnation=instance;
 out->revision=7;strcpy(out->component_id,id);strcpy(out->context,context);out->valid=true;return true;
}
'''+helpers+r'''
int main(void){
 pet_host_observation_t down,up;strcpy(g_runtime.widget_id,"records");g_runtime.app_host=true;
 epoch=1;context="record-set-a";host_pointer_capture(&down);
 epoch=2;context="record-set-b";host_pointer_capture(&up);
 assert(!pet_host_observation_equal(&down,&up));
 // Returning to identical displayed labels does not revive the old gesture.
 epoch=3;context="record-set-a";host_pointer_capture(&up);
 assert(!strcmp(down.context,up.context)&&!pet_host_observation_equal(&down,&up));
 host_pointer_capture(&down);host_pointer_capture(&up);assert(pet_host_observation_equal(&down,&up));
 instance++;host_pointer_capture(&up);assert(!pet_host_observation_equal(&down,&up));
 g_runtime.app_host=false;host_pointer_capture(&up);assert(!up.valid&&!up.context[0]&&!up.variable_epoch);
 assert(!pet_host_observation_equal(&up,&up));
 return 0;
}
'''
    path=tmp_path/'guard.c';path.write_text(code);binary=tmp_path/'guard'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
