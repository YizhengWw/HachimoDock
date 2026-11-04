"""Execute the production clip adapter/reset with deterministic cancellation hooks."""
from pathlib import Path
import subprocess
import pytest
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def clip_binary(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("clip")
    ext = (ROOT / "main/pet_p4_renderer_ext.inc").read_text()
    prefetch = (ROOT / "main/pet_p4_h264_prefetch.inc").read_text()
    code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_component_media.h"
#include "pet_p4_prefetch_lifetime.h"
#define EXT_RAM_BSS_ATTR
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
#define PET_RESOURCE_ASSET_BUFFER 1
#define PET_RESOURCE_MASK(x) (x)
#define PET_RESOURCE_OK 0
typedef struct {unsigned generation;} pet_resource_owner_t;
typedef unsigned pet_resource_lease_t;
typedef struct {uint8_t*bytes;size_t size;bool finished,h264,single_slice_valid;unsigned decoder_readers;char fs_path[80];pet_resource_owner_t owner;pet_resource_lease_t buffer_lease;} pet_p4_asset_cache_entry_t;
typedef struct {int x,y,w,h;uint16_t color;} pet_surface_node_t;
typedef struct {const uint8_t*data;} pet_h264_packed_view_t;
typedef struct {char path[80];pet_prefetch_ticket_t ticket;pet_resource_owner_t owner;uint64_t cancel_epoch;unsigned output_size;const uint8_t*output;int frame;} pet_h264_prefetch_job_t;
static int mux_depth;
static int g_asset_prepare_mux;
#define portENTER_CRITICAL(p) do{(void)(p);assert(!mux_depth);mux_depth=1;}while(0)
#define portEXIT_CRITICAL(p) do{(void)(p);assert(mux_depth==1);mux_depth=0;}while(0)
static pet_prefetch_lifetime_t g_h264_prefetch_lifetime;
static pet_h264_prefetch_job_t g_h264_prefetch_job;
static pet_h264_packed_view_t g_h264_packed_current;
static int g_h264_presented_frame=-1;
static uint64_t g_h264_first_presented_ms;
static char g_h264_presented_path[80];
static bool g_h264_reset_requested;
static void *g_h264_decoder;
static unsigned g_h264_ppa_input_capacity=32;
static uint32_t g_ext_snapshot_generation=7;
static uint16_t framebuffer[640*480],*g_framebuffer=framebuffer;
static uint8_t old_pixels[16],new_pixels[16],encoded[16];
static unsigned fills,pixel_reads,offers,unpins,finishes,revokes;
static int cancel_at;
static bool h264_prefetch_offer(pet_p4_asset_cache_entry_t*e,const char*p,int w,int h,bool r,int f){(void)e;(void)p;(void)w;(void)h;(void)r;(void)f;offers++;return false;}
static void pet_resources_device_revoke(pet_resource_owner_t o){assert(!mux_depth);if(o.generation)revokes++;}
static int pet_resources_device_finish(pet_resource_lease_t l){assert(!mux_depth);if(l)finishes++;return 0;}
static bool pet_resources_device_current(pet_resource_lease_t l){assert(!mux_depth);return l!=0;}
static int pet_resources_device_owner_open(unsigned m,pet_resource_owner_t*o){(void)m;o->generation=1;return 0;}
static int pet_resources_device_acquire(pet_resource_owner_t o,unsigned k,uint32_t n,pet_resource_lease_t*l){(void)o;(void)k;(void)n;*l=1;return 0;}
bool pet_cmedia_generation_try(uint32_t g){assert(!mux_depth);return g==7;}
bool pet_cmedia_clip_pin_try_at(unsigned i,uint32_t g,pet_cmedia_clip_lease_t*l){(void)i;(void)g;(void)l;assert(!"unexpected repin");return false;}
bool pet_cmedia_unpin_deferred(uint64_t t){assert(!mux_depth&&t==17&&g_h264_prefetch_lifetime.phase==PET_PREFETCH_EMPTY);unpins++;return true;}
static void fill_rect(int x,int y,int w,int h,uint16_t c){assert(x==10&&y==20&&w==2&&h==2&&c==0x1234);fills++;}
static bool app_clip_contains(int x,int y){return x>=11&&x<12&&y>=20&&y<22;}
static bool resolve_h264_i420_layout(int w,int h,unsigned n,int*s,int*t){assert(w==2&&h==2&&n==16);*s=2;*t=2;return true;}
static void cancel_during_pack(void){assert(g_h264_prefetch_lifetime.phase==PET_PREFETCH_CONSUMING);assert(pet_prefetch_cancel(g_h264_prefetch_job.ticket));}
static bool h264_worker_pack_adopt(const pet_h264_prefetch_job_t*j,int w,int h,int s,int t){(void)j;(void)w;(void)h;(void)s;(void)t;
 if(cancel_at==2)return false;g_h264_packed_current.data=new_pixels;if(cancel_at==1)cancel_during_pack();return true;
}
static bool pack_h264_i420_for_ppa(const uint8_t*p,int w,int h,int s,int t,const uint8_t**out){(void)p;(void)w;(void)h;(void)s;(void)t;*out=new_pixels;cancel_during_pack();return true;}
static bool pet_h264_packed_view_init(pet_h264_packed_view_t*v,const uint8_t*p,unsigned cap,unsigned stride,int w,int h,int sh){(void)cap;(void)stride;(void)w;(void)h;(void)sh;v->data=p;return true;}
static uint16_t pet_h264_packed_pixel(const pet_h264_packed_view_t*v,int x,int y){assert(v->data==new_pixels&&x==1&&y>=0&&y<2);pixel_reads++;return 0x5a5a;}
'''
    code += function(prefetch, "h264_prefetch_cancel_locked")
    code += function(prefetch, "h264_prefetch_reset")
    # Production descriptor definitions and all four real adapter functions.
    start = ext.index("static struct { pet_cext_clip_state_t")
    end = ext.index("bool renderer_ext_clip_retire", start)
    code += ext[start:end]
    for name in ("renderer_ext_clip_retire", "renderer_clip_entry", "renderer_clip_fill", "renderer_ext_draw_clip"):
        code += function(ext, name)
    code += r'''
/* The portable clock is not under test; keep target at the next frame. */
int pet_cext_clip_target(pet_cext_clip_state_t*s,const pet_cext_clip_t*m,bool visible,uint64_t now){(void)s;(void)m;(void)now;assert(visible);return 1;}
static void setup(void){
 assert(pet_prefetch_init(&g_h264_prefetch_lifetime));
 g_clip_lease=(pet_cmedia_clip_lease_t){.token=17,.generation=7,.stream=encoded,.bytes=sizeof(encoded),.meta={.w=2,.h=2,.frames=2,.fps=1}};
 strcpy(g_clip.path,"clip:sample");g_clip.presented=0;g_clip.generation=7;
 g_clip_entry.bytes=encoded;g_clip_entry.size=sizeof(encoded);g_clip_entry.owner.generation=1;g_clip_entry.buffer_lease=1;
 g_h264_packed_current.data=old_pixels;
 assert(pet_prefetch_offer(&g_h264_prefetch_lifetime,1,&g_h264_prefetch_job.ticket));
 strcpy(g_h264_prefetch_job.path,g_clip.path);g_h264_prefetch_job.frame=1;g_h264_prefetch_job.output_size=16;g_h264_prefetch_job.output=new_pixels;
 bool execute=false;assert(pet_prefetch_claim(g_h264_prefetch_job.ticket,&execute)&&execute);
}
int main(int argc,char**argv){
 assert(argc==2);cancel_at=atoi(argv[1]);setup();
 if(cancel_at==3){
  g_clip_entry.decoder_readers=1;
  assert(!renderer_ext_clip_retire());assert(!unpins&&g_clip_lease.token==17&&g_clip_entry.bytes==encoded);
  g_clip_entry.decoder_readers=0;assert(pet_prefetch_complete(g_h264_prefetch_job.ticket,false));
  assert(renderer_ext_clip_retire());assert(unpins==1&&finishes==1&&!g_clip_lease.token&&!g_clip_entry.bytes);return 0;
 }
 assert(pet_prefetch_complete(g_h264_prefetch_job.ticket,true));
 pet_surface_node_t n={.x=10,.y=20,.w=2,.h=2,.color=0x1234};renderer_ext_draw_clip(&n,2000);
 assert(!mux_depth&&!offers);
 if(cancel_at){assert(fills==1&&!pixel_reads&&unpins==1&&finishes==1&&!g_clip_lease.token&&!g_h264_packed_current.data);}
 else {assert(!fills&&pixel_reads==2&&!unpins&&!finishes&&g_clip.presented==1&&g_clip_lease.token==17);assert(framebuffer[20*640+11]==0x5a5a&&framebuffer[20*640+10]==0);}
 return 0;
}
'''
    src, binary = tmp / "clip.c", tmp / "clip"
    src.write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                    "-I", str(ROOT / "tests/vendor/cjson"), str(src), "-o", str(binary)], check=True)
    return binary


@pytest.mark.parametrize("scenario", [0, 1, 2, 3], ids=["normal-clipped-frame", "cancel-worker-adopt", "cancel-fallback-pack", "retire-waits-for-worker"])
def test_clip_delivery_and_source_retirement(clip_binary, scenario):
    subprocess.run([str(clip_binary), str(scenario)], check=True)
