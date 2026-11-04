"""Retry incomplete native framebuffer allocation without losing a successful slot."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def test_native_buffer_allocation_recovers_either_missing_slot(tmp_path):
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    code = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PET_P4_NATIVE_OUTPUT_BUFFER_COUNT 2
#define PET_P4_PPA_BUFFER_ALIGNMENT 64
#define BSP_LCD_H_RES 480
#define BSP_LCD_V_RES 640
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_8BIT 4
static uint16_t *g_native_framebuffers[2],*g_native_framebuffer;
static bool g_native_framebuffer_ppa_compatible;
static uint16_t backing[4];
static unsigned calls,fail_first,fail_last,successes;
static void *allocate(void){
 ++calls;if(calls>=fail_first&&calls<=fail_last)return NULL;
 assert(successes<4);return &backing[successes++];
}
static void *heap_caps_aligned_calloc(size_t alignment,size_t count,size_t size,unsigned caps){
 (void)alignment;(void)count;(void)size;(void)caps;return allocate();
}
static void *heap_caps_malloc(size_t bytes,unsigned caps){(void)bytes;(void)caps;return allocate();}
""" + function(source, "renderer_init_native_buffers") + r"""
static void reset(unsigned first,unsigned last){
 g_native_framebuffers[0]=g_native_framebuffers[1]=g_native_framebuffer=NULL;
 g_native_framebuffer_ppa_compatible=false;calls=successes=0;fail_first=first;fail_last=last;
}
int main(void){
 /* Slot 0 succeeds; slot 1 exhausts all three routes. It must be retried. */
 reset(2,4);renderer_init_native_buffers();
 assert(g_native_framebuffers[0]&&!g_native_framebuffers[1]);
 uint16_t *kept=g_native_framebuffers[0];fail_first=fail_last=0;
 renderer_init_native_buffers();assert(g_native_framebuffers[0]==kept&&g_native_framebuffers[1]);
 assert(successes==2&&g_native_framebuffer==kept&&!g_native_framebuffer_ppa_compatible);
 unsigned before=calls;renderer_init_native_buffers();assert(calls==before);
 /* Slot 0 fails while slot 1 succeeds. Retry must not overwrite/leak slot 1. */
 reset(1,3);renderer_init_native_buffers();
 assert(!g_native_framebuffers[0]&&g_native_framebuffers[1]&&!g_native_framebuffer);
 kept=g_native_framebuffers[1];fail_first=fail_last=0;renderer_init_native_buffers();
 assert(g_native_framebuffers[1]==kept&&g_native_framebuffers[0]);assert(successes==2);
 assert(g_native_framebuffer==g_native_framebuffers[0]);
 /* An already selected second output buffer must survive ordinary init calls. */
 g_native_framebuffer=g_native_framebuffers[1];before=calls;renderer_init_native_buffers();
 assert(calls==before&&g_native_framebuffer==kept);
 reset(0,0);renderer_init_native_buffers();assert(successes==2&&g_native_framebuffer_ppa_compatible);
 return 0;
}
"""
    src = tmp_path / "alloc.c"
    exe = tmp_path / "alloc"
    src.write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
