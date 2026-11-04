"""Conversation/cached-video admission and actual prefetch interleavings; offline only."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
from p4_main_frame_test import CHAIN
ROOT=Path(__file__).resolve().parents[1]

def test_cached_video_keeps_worker_lifetime_and_audio_startup_protection(tmp_path):
    source=(ROOT/'main/pet_p4_renderer.c').read_text()
    declarations=source[source.index('static esp_h264_dec_handle_t g_h264_decoder;'):source.index('static uint8_t *g_h264_ppa_input;')]
    declarations+='\nstatic uint8_t *g_h264_ppa_input;static size_t g_h264_ppa_input_capacity;static char g_h264_fs_path[80];\n'
    declarations+=source[source.index('typedef struct {\n  uint8_t *bytes;'):source.index('static bool h264_stream_has_single_slice_access_units(')]
    functions='\n'.join(function(source,name) for name in (
        'asset_prepare_audio_busy','asset_prepare_current','asset_cache_release','pet_p4_renderer_cancel_asset_preparation',
        'pet_p4_renderer_set_asset_preparation_suspended','reset_h264_decoder','reset_h264_decoder_traced',
        'start_h264_decoder_role','decode_next_h264_frame_role'))
    functions+='\n'+function(source,'pack_h264_i420_rows').replace('pack_h264_i420_rows(','actual_pack_rows(',1)
    harness=(ROOT/'tests/p4_main_frame_adapter_test.c').read_text()
    harness=harness.replace('static bool pet_p4_audio_conversation_active(void){return false;}',
       'static bool conversation_ready;\nstatic bool pet_p4_audio_conversation_active(void){return conversation_ready;}\n'
       'static bool pet_p4_audio_conversation_video_ready(void){return conversation_ready;}')
    scenario=r"""
 /* A continuous initialized conversation may advance actual decoded frames;
  * another audio initialization revokes it without releasing physical work. */
 fresh_source();
 pet_resource_realtime_t voice={0},initializing={0};
 assert(pet_resources_realtime_begin(&broker,&voice)==PET_RESOURCE_OK);
 conversation_ready=true;playback_busy=true;
 assert(!h264_prefetch_offer(&g_asset_cache[0],"conversation",16,16,true,0));
 assert(pet_resources_realtime_video_ready(&broker,voice,true));
 assert(!harness_frame(&asset,"conversation",&g_asset_cache[0],300));
 assert(h264_prefetch_worker());
 assert(harness_frame(&asset,"conversation",&g_asset_cache[0],300));
 assert(g_h264_presented_frame==0);
 assert(pet_resources_realtime_begin(&broker,&initializing)==PET_RESOURCE_OK);
 pet_resource_usage_t during;pet_resources_usage(&broker,&during);
 assert(during.leases[PET_RESOURCE_CACHED_VIDEO]==1);
 assert(h264_prefetch_worker());
 assert(g_h264_prefetch_lifetime.phase==PET_PREFETCH_DISCARDED);
 assert(pet_resources_realtime_finish(&broker,initializing));
 assert(!pet_resources_realtime_video_ready(&broker,initializing,true));
 fresh_source();
 assert(!harness_frame(&asset,"conversation2",&g_asset_cache[0],300));
 assert(h264_prefetch_worker());
 assert(harness_frame(&asset,"conversation2",&g_asset_cache[0],300));
 assert(pet_resources_realtime_finish(&broker,voice));
 conversation_ready=false;playback_busy=false;
 fresh_source();asset_cache_release(&g_asset_cache[0]);
 pet_resources_usage(&broker,&usage);
 assert(!usage.live_owners&&!usage.revoked_owners&&!usage.leases[PET_RESOURCE_CACHED_VIDEO]);
 """
    harness=harness.replace('free(g_h264_work_stream);return 0;',scenario+'\nfree(g_h264_work_stream);return 0;')
    code=harness.replace('/* ACTUAL_DECLARATIONS */',declarations).replace('/* ACTUAL_FUNCTIONS */',functions)
    src=tmp_path/'video.c';src.write_text(code);binary=tmp_path/'video'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
      '-fsanitize=address,undefined',*[f'-D{d}=1' for d in CHAIN],'-DPET_P4_H264_PREFETCH=1',
      '-DPET_P4_H264_WORKER_PACK=1','-DPET_P4_CONVERSATION_VIDEO=1',
      '-I',str(ROOT/'main'),str(src),str(ROOT/'main/pet_p4_resources.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=35)
