"""Shared main-page/APP worker cancellation must not cancel each clip frame."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def test_app_clip_survives_frame_entry_but_hidden_transfer_and_exit_cancel(tmp_path):
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    code = r"""
#include <assert.h>
#include <string.h>
#include "pet_p4_prefetch_lifetime.h"
#define PET_P4_CLIP 1
static struct{unsigned token;}g_clip_lease;
static pet_prefetch_lifetime_t life;
static pet_prefetch_ticket_t ticket;
static unsigned cancels;
static void release_inactive_asset_prepares(void){}
static void cancel_pending_asset_prepares(void){++cancels;(void)pet_prefetch_cancel(ticket);}
""" + function(source, "renderer_cancel_inactive_prepares") + r"""
static void offer(void){ticket=(pet_prefetch_ticket_t){0};assert(pet_prefetch_offer(&life,1,&ticket));}
static void finish_ready(void){bool execute=false;assert(pet_prefetch_claim(ticket,&execute)&&execute);assert(pet_prefetch_complete(ticket,true));}
int main(void){
 assert(pet_prefetch_init(&life));
 /* Initial APP transition has no clip pin: retire the main-page job. */
 offer();renderer_cancel_inactive_prepares("app");assert(cancels==1&&life.cancelled);
 bool execute=true;assert(pet_prefetch_claim(ticket,&execute)&&!execute);
 assert(pet_prefetch_complete(ticket,false)&&pet_prefetch_retire(&life));
 /* Subsequent APP frame entries must preserve queued, working AND ready jobs.
  * Use the actual lifetime owner through repeated sequential presentations. */
 g_clip_lease.token=7;
 for(unsigned frame=0;frame<4;frame++){
  offer();renderer_cancel_inactive_prepares("app");assert(!life.cancelled&&life.phase==PET_PREFETCH_QUEUED);
  assert(pet_prefetch_claim(ticket,&execute)&&execute);
  renderer_cancel_inactive_prepares("app");assert(!life.cancelled&&life.phase==PET_PREFETCH_WORKING);
  assert(pet_prefetch_complete(ticket,true));
  renderer_cancel_inactive_prepares("app");assert(life.phase==PET_PREFETCH_READY);
  assert(pet_prefetch_take(ticket,1)&&pet_prefetch_release(ticket));
 }
 assert(cancels==1);
 /* Page exit cancels even if a source pin still waits for physical retirement. */
 offer();finish_ready();renderer_cancel_inactive_prepares("components");
 assert(life.cancelled&&life.phase==PET_PREFETCH_DISCARDED&&cancels==2);
 assert(pet_prefetch_retire(&life));g_clip_lease.token=0;
 offer();finish_ready();renderer_cancel_inactive_prepares("app");
 assert(life.cancelled&&life.phase==PET_PREFETCH_DISCARDED&&cancels==3);
 assert(pet_prefetch_retire(&life));offer();finish_ready();
 renderer_cancel_inactive_prepares("main");assert(life.phase==PET_PREFETCH_READY&&cancels==3);
 return 0;
}
"""
    src = tmp_path / "clip.c"
    exe = tmp_path / "clip"
    src.write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(ROOT / "main"), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    render = function(source, "pet_p4_renderer_render")
    assert render.count("renderer_cancel_inactive_prepares(page);") == 1
    assert "cancel_pending_asset_prepares();" not in render
    assert render.index("renderer_cancel_inactive_prepares(page);") < render.index("render_miniapp_page(state, now_ms);")
    # A hidden/absent clip still retires through its snapshot owner every frame.
    assert "return renderer_ext_clip_retire();" in function(source, "renderer_component_media_frame")
    assert "cancel_pending_asset_prepares();" in function(source, "pet_p4_renderer_render_transfer_status")


def test_clip_frame_reclaims_finished_ordinary_read_with_real_broker(tmp_path):
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    code = r"""
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_resources.h"
#include "pet_p4_prefetch_lifetime.h"
#define PET_P4_CLIP 1
#define PET_P4_H264_PREFETCH 1
#define PET_P4_ASSET_CACHE_SLOTS 2
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static pet_resource_broker_t broker;
typedef struct {bool loading,finished;unsigned decoder_readers;uint8_t *bytes;pet_resource_owner_t owner;pet_resource_lease_t buffer_lease,prepare_lease;} pet_p4_asset_cache_entry_t;
static pet_p4_asset_cache_entry_t g_asset_cache[2];
static struct{unsigned token;}g_clip_lease;
static int g_asset_prepare_mux;
static uint64_t g_asset_prepare_cancel_epoch;
static struct{pet_resource_owner_t owner;}g_h264_prefetch_job;
static pet_prefetch_lifetime_t life;
static pet_prefetch_ticket_t ticket;
static unsigned freed;
static void heap_caps_free(void *p){++freed;free(p);}
static void pet_resources_device_revoke(pet_resource_owner_t o){(void)pet_resources_owner_revoke(&broker,o);}
static pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t l){return pet_resources_finish(&broker,l);}
static void h264_prefetch_cancel_locked(void){(void)pet_prefetch_cancel(ticket);}
"""
    for name in ("asset_cache_release", "pet_p4_renderer_cancel_asset_preparation", "release_inactive_asset_prepares", "cancel_pending_asset_prepares", "renderer_cancel_inactive_prepares"):
        code += function(source, name) + "\n"
    code += r"""
int main(void){
 assert(pet_prefetch_init(&life));
 pet_p4_asset_cache_entry_t *old=&g_asset_cache[0];old->loading=true;
 assert(pet_resources_owner_open(&broker,PET_RESOURCE_MASK(PET_RESOURCE_ASSET_BUFFER)|PET_RESOURCE_MASK(PET_RESOURCE_ASSET_PREPARE),&old->owner)==PET_RESOURCE_OK);
 assert(pet_resources_try_acquire(&broker,old->owner,PET_RESOURCE_ASSET_BUFFER,2*1024*1024,&old->buffer_lease)==PET_RESOURCE_OK);
 assert(pet_resources_try_acquire(&broker,old->owner,PET_RESOURCE_ASSET_PREPARE,1,&old->prepare_lease)==PET_RESOURCE_OK);
 uint8_t *worker_bytes=malloc(2*1024*1024);assert(worker_bytes);
 renderer_cancel_inactive_prepares("app");assert(!freed&&old->loading&&!old->finished);
 /* A clip may pin while the old read still physically owns its buffer. */
 pet_resource_owner_t clip_owner={0};pet_resource_lease_t clip_buffer={0};
 assert(pet_resources_owner_open(&broker,PET_RESOURCE_MASK(PET_RESOURCE_ASSET_BUFFER),&clip_owner)==PET_RESOURCE_OK);
 assert(pet_resources_try_acquire(&broker,clip_owner,PET_RESOURCE_ASSET_BUFFER,4096,&clip_buffer)==PET_RESOURCE_OK);
 g_clip_lease.token=7;
 /* Worker completion is later than the first APP frame's release poll. */
 assert(pet_resources_finish(&broker,old->prepare_lease)==PET_RESOURCE_FINISH_REVOKED);
 old->bytes=worker_bytes;old->finished=true;
 assert(pet_prefetch_offer(&life,1,&ticket));
 renderer_cancel_inactive_prepares("app");
 assert(freed==1&&!old->loading&&!old->bytes&&!life.cancelled&&life.phase==PET_PREFETCH_QUEUED);
 pet_resource_usage_t usage={0};pet_resources_usage(&broker,&usage);
 assert(usage.leases[PET_RESOURCE_ASSET_BUFFER]==1&&usage.units[PET_RESOURCE_ASSET_BUFFER]==4096);
 assert(pet_resources_current(&broker,clip_buffer));
 bool execute=false;assert(pet_prefetch_claim(ticket,&execute)&&execute);
 renderer_cancel_inactive_prepares("app");assert(!life.cancelled&&freed==1);
 assert(pet_prefetch_complete(ticket,true));renderer_cancel_inactive_prepares("app");
 assert(life.phase==PET_PREFETCH_READY&&freed==1);
 return 0;
}
"""
    src=tmp_path/"reclaim.c";exe=tmp_path/"reclaim";src.write_text(code)
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-I",str(ROOT/"main"),str(src),str(ROOT/"main/pet_p4_resources.c"),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


def test_clip_snapshot_uses_shell_page_viewport_and_generation_retirement(tmp_path):
    source=(ROOT/"main/pet_p4_renderer.c").read_text()
    surface=(ROOT/"main/pet_p4_surface.c").read_text()
    code=r"""
#include <assert.h>
#include <string.h>
#include "pet_p4_surface.h"
#include "pet_p4_prefetch_lifetime.h"
static struct{unsigned token;uint32_t generation;}g_clip_lease;
typedef struct {bool surface_active;uint32_t media_generation;struct{bool active,app_surface;uint8_t surface_page;}view;pet_surface_t surface;}pet_p4_miniapp_render_snapshot_t;
static pet_prefetch_lifetime_t life;
static pet_prefetch_ticket_t ticket;
static unsigned retire_calls;
static void renderer_ext_media_snapshot(uint32_t generation){(void)generation;}
static bool renderer_ext_clip_retire(void){
 ++retire_calls;if(!g_clip_lease.token)return true;
 (void)pet_prefetch_cancel(ticket);
 if(!pet_prefetch_retire(&life))return false;
 g_clip_lease.token=0;return true;
}
"""
    for name in ("pet_surface_clamp_offset","pet_surface_node_viewport","pet_surface_project"):
        code+=function(surface,name)+"\n"
    code+=function(source,"renderer_component_media_frame")+r"""
static void offer(void){ticket=(pet_prefetch_ticket_t){0};assert(pet_prefetch_offer(&life,1,&ticket));g_clip_lease.token=7;g_clip_lease.generation=1;}
static void finish_ready(void){bool execute=false;assert(pet_prefetch_claim(ticket,&execute)&&execute);assert(pet_prefetch_complete(ticket,true));}
int main(void){
 assert(pet_prefetch_init(&life));
 pet_p4_miniapp_render_snapshot_t s={.surface_active=true,.media_generation=1,.view={.active=true,.app_surface=true}};
 s.surface.count=1;s.surface.nodes[0]=(pet_surface_node_t){.type=20,.w=160,.h=120};
 unsigned pages[]={4,12,16,20,27};
 for(unsigned i=0;i<5;i++){
  s.surface.nodes[0].page=pages[i];s.view.surface_page=pages[i]>=16?pages[i]-16:0;offer();
  unsigned before=retire_calls;assert(renderer_component_media_frame(&s));assert(retire_calls==before&&!life.cancelled);
  finish_ready();assert(renderer_ext_clip_retire());
 }
 s.surface.nodes[0].page=16;s.view.surface_page=1;offer();finish_ready();assert(renderer_component_media_frame(&s));assert(!g_clip_lease.token);
 s.surface.nodes[0].page=4;s.view.surface_page=0;s.surface.nodes[0].in_view=true;
 s.surface.viewport=(pet_surface_viewport_t){.enabled=true,.w=320,.h=240,.content_height=720};
 offer();assert(renderer_component_media_frame(&s));assert(g_clip_lease.token);
 bool execute=false;assert(pet_prefetch_claim(ticket,&execute)&&execute);
 s.surface.viewport.offset=240;assert(!renderer_component_media_frame(&s));assert(life.cancelled&&g_clip_lease.token);
 assert(pet_prefetch_complete(ticket,false));assert(renderer_component_media_frame(&s));assert(!g_clip_lease.token);
 /* Stale generation retires even if a geometrically visible clip is present. */
 s.surface.viewport.offset=0;offer();finish_ready();s.media_generation=2;assert(renderer_component_media_frame(&s));assert(!g_clip_lease.token);
 assert(renderer_component_media_frame(NULL));
 return 0;
}
"""
    src=tmp_path/"visible.c";exe=tmp_path/"visible";src.write_text(code)
    flags=["-DPET_P4_CLIP=1","-DPET_P4_H264_WORKER_PACK=1","-DPET_P4_H264_PREFETCH=1","-DPET_P4_APP_PPA_COPY=1","-DPET_P4_APP_COLD_FAST=1","-DPET_P4_APP_CLEAR_OPAQUE_CENTER=1","-DPET_P4_APP_DIRTY_CACHE=1"]
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-misleading-indentation","-fsanitize=address,undefined",*flags,"-I",str(ROOT/"main"),"-I",str(ROOT/"tests/vendor/cjson"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
