"""R143 PPA frame copy: flag gating, the six APP clean-frame copy sites route through the self-checked PPA copy, CPU fallback
semantics, env chain. Offline only (the bit-exactness of the hardware copy is self-checked on the device and counted)."""
from pathlib import Path
import configparser
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def test_copy_sites_fallback_and_self_check_are_in_place(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text();diag=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    render=text[text.index('static bool render_package_app('):text.index('static void render_miniapp_page(')]
    assert render.count('pet_app_frame_copy(g_framebuffer,g_surface_background,sizeof(g_surface_background))')==4 and render.count('pet_app_frame_copy(g_surface_background,g_framebuffer,sizeof(g_surface_background))')==2
    helper=text[text.index('static void app_frame_copy_ppa('):text.index('static bool render_package_app(')]
    assert '#define pet_app_frame_copy(to,from,bytes) app_frame_copy_ppa((to),(from),(bytes))' in helper and helper.index('pet_app_frame_copy(dst,src,bytes);\n}')<helper.index('#define pet_app_frame_copy(')
    assert 'bytes==640*480*sizeof(uint16_t)&&' in helper
    # 1:1 blocking SRM copy, RGB565 both sides, no scale/rotate/mirror/swap; aligned PSRAM buffers only.
    for needle in ('.rotation_angle=PPA_SRM_ROTATION_ANGLE_0','.scale_x=1.0f,.scale_y=1.0f','.rgb_swap=false,.byte_swap=false,.mode=PPA_TRANS_MODE_BLOCKING',
                   '.srm_cm=PPA_SRM_COLOR_MODE_RGB565','(((uintptr_t)dst|(uintptr_t)src)&63u)==0','g_framebuffer_ppa_compatible'):
        assert needle in helper,needle
    # Self-check on the first 8 copies over four sampled rows; any mismatch or PPA error disables the path for the boot and falls back.
    assert 'if(g_ppa_copies<=8){' in helper and 'static const int rows[4]={0,159,320,479};' in helper
    assert helper.count('g_ppa_copy_disabled=true;')==2 and helper.count('pet_app_frame_copy(dst,src,bytes);')==2
    assert '#define PET_SURFACE_BACKGROUND_ALIGN __attribute__((aligned(64)))' in text and text.count('uint16_t g_surface_background[640*480]')==1 and 'static EXT_RAM_BSS_ATTR PET_SURFACE_BACKGROUND_ALIGN uint16_t g_surface_background[640*480];' in text
    for key in ('"ppaCopy"','"copies"','"fallbacks"','"mismatches"'):assert key in diag
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_ppa_copy_parent':
            assert config[section]['extends']=='env:esp32_p4_cold_fast_parent' and config[section]['custom_p4_project_version']=='0.7.103-p4-r143-ppacopy'
            assert 'PET_P4_APP_PPA_COPY=1' in flags and not any(v in flags for v in ('TASK_PRIORITY','F_CPU','BUDGET_US','WATCHDOG','AFFINITY'))
        else:assert 'PET_P4_APP_PPA_COPY=1' not in flags
    assert config['env:esp32_p4_framework_parent']['extends']=='env:esp32_p4_ppa_copy_parent' and config['env:esp32_p4_main_frame_parent']['extends']=='env:esp32_p4_framework_parent' and config['env:esp32_p4_capability_parent']['extends']=='env:esp32_p4_main_frame_parent' and config['env:esp32_p4_program_public_candidate']['extends']=='env:esp32_p4_capability_parent'
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_render_lifetime.h"\nint main(void){return PET_P4_APP_PPA_COPY;}\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_APP_PPA_COPY=1','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'PPA frame copy requires' in result.stderr
    assert subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsyntax-only','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True).returncode==0
