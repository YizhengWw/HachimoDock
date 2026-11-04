"""Generate checked codec sources in a build directory; never alter a package.

Pinned to the repository's esp_codec_dev 1.2.0 source content, not an SDK path.
Unknown input fails closed BEFORE writing any output. No dependency resolution.
"""
import argparse
import hashlib
from pathlib import Path

PINS = {
    "esp_codec_dev.c": "adeaea5da986ad5984d5f8e7d4c44f5a708c1e8fa38d3c7697dcea4eca292b22",
    "device/es8311/es8311.c": "372fb0395bbf6f1f7e1b7c03f577cb5f15bcac350977f39b3734306a986f4268",
    "platform/audio_codec_ctrl_i2c.c": "176314735ea809d7c4a6cf17d4e6c2d9d2d3d699efad8e13d915249fc6a738e2",
}


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError("codec patch anchor changed")
    return text.replace(old, new, 1)


def patch(name, source):
    if name == "esp_codec_dev.c":
        source = once(source, '#define TAG', 'const unsigned pet_audio_volume_codec_patch_v1 = 1;\n\n#define TAG')
        source = once(source, '''    dev->volume = volume;
    // Prefer to use software volume setting
    if (dev->sw_vol) {
        dev->sw_vol->set_vol(dev->sw_vol, db_value);
        return ESP_CODEC_DEV_OK;
    }
    if (codec && codec->set_vol) {
        codec->set_vol(codec, db_value);
        return ESP_CODEC_DEV_OK;
    }''', '''    // Publish the cache only after the real callback succeeds.
    if (dev->sw_vol) {
        ret = dev->sw_vol->set_vol(dev->sw_vol, db_value);
    } else if (codec && codec->set_vol) {
        ret = codec->set_vol(codec, db_value);
    } else {
        return ESP_CODEC_DEV_NOT_SUPPORT;
    }
    if (ret == ESP_CODEC_DEV_OK) dev->volume = volume;
    return ret;''')
        source = once(source, '''    dev->muted = mute;
    if (codec && codec->mute) {
        codec->mute(codec, mute);
        return ESP_CODEC_DEV_OK;
    }''', '''    if (codec && codec->mute) {
        ret = codec->mute(codec, mute);
        if (ret == ESP_CODEC_DEV_OK) dev->muted = mute;
        return ret;
    }''')
        # Preserve the existing unsupported software-mute fallback semantics.
        return source
    if name == "device/es8311/es8311.c":
        return once(source, '''    int ret = es8311_read_reg(codec, ES8311_DAC_REG31, &regv);
    regv &= 0x9f;
    if (mute) {
        es8311_write_reg(codec, ES8311_DAC_REG31, regv | 0x60);
    } else {
        es8311_write_reg(codec, ES8311_DAC_REG31, regv);
    }
    return ret;''', '''    int ret = es8311_read_reg(codec, ES8311_DAC_REG31, &regv);
    if (ret != ESP_CODEC_DEV_OK) return ret;
    regv &= 0x9f;
    return es8311_write_reg(codec, ES8311_DAC_REG31, mute ? regv | 0x60 : regv);''')
    source = once(source, '#include "esp_log.h"', '#include "esp_log.h"\n#include "pet_p4_audio_volume_deadline.h"')
    # Both supported SDKs use the >=5.3 master API. Legacy branches stay byte-identical.
    source = once(source, '''    int ret = i2c_master_transmit_receive(i2c_ctrl->dev_handle, addr_data, addr_len, data, data_len, DEFAULT_I2C_TRANS_TIMEOUT);
    if (ret != ESP_OK) {''', '''    int timeout_ms = pet_audio_volume_i2c_timeout_ms(DEFAULT_I2C_TRANS_TIMEOUT);
    if (timeout_ms <= 0) return ESP_CODEC_DEV_READ_FAIL;
    int ret = i2c_master_transmit_receive(i2c_ctrl->dev_handle, addr_data, addr_len, data, data_len, timeout_ms);
    if (ret != ESP_OK && !pet_audio_volume_deadline_active()) {''')
    source = once(source, '''        ret = i2c_master_transmit(i2c_ctrl->dev_handle, write_data, len, DEFAULT_I2C_TRANS_TIMEOUT);
    }
    if (ret != 0) {''', '''        int timeout_ms = pet_audio_volume_i2c_timeout_ms(DEFAULT_I2C_TRANS_TIMEOUT);
        if (timeout_ms <= 0) return ESP_CODEC_DEV_WRITE_FAIL;
        ret = i2c_master_transmit(i2c_ctrl->dev_handle, write_data, len, timeout_ms);
    }
    if (ret != 0 && !pet_audio_volume_deadline_active()) {''')
    return source


def generate(vendor, output):
    vendor, output = Path(vendor).resolve(), Path(output).resolve()
    repo_firmware = Path(__file__).resolve().parents[1]
    source_dirs = [repo_firmware / p for p in ("main", "tools", "tests", "components", "managed_components")]
    if (output == vendor or vendor in output.parents or output == repo_firmware
            or any(output == p or p in output.parents for p in source_dirs)):
        raise ValueError("output must be a separate build directory")
    sources = {}
    for name, expected in PINS.items():
        data = (vendor / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"unrecognized pinned codec source: {name}")
        sources[name] = patch(name, data.decode())
    for name in sources:
        dest = output / name
        if dest.is_symlink() or output not in dest.resolve().parents:
            raise ValueError("generated destination escapes build directory")
    for name, data in sources.items():
        dest = output / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists() or dest.read_text() != data:
            dest.write_text(data)


def cmake_integration():
    return r'''# Paste at the end of firmware/main/CMakeLists.txt (after component registration).
# Generated sources only; managed components, SDK packages and lockfiles are unchanged.
set(pet_volume_main "${CMAKE_CURRENT_SOURCE_DIR}")
idf_component_get_property(pet_volume_codec_lib espressif__esp_codec_dev COMPONENT_LIB)
idf_component_get_property(pet_volume_codec_dir espressif__esp_codec_dev COMPONENT_DIR)
set(pet_volume_generated "${CMAKE_BINARY_DIR}/pet_audio_master_volume_codec")
set(pet_volume_patcher "${pet_volume_main}/../tools/patch_audio_master_volume.py")
set(pet_volume_vendor_sources
  "esp_codec_dev.c" "device/es8311/es8311.c" "platform/audio_codec_ctrl_i2c.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pet_volume_patcher}")
foreach(pet_volume_rel IN LISTS pet_volume_vendor_sources)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${pet_volume_codec_dir}/${pet_volume_rel}")
endforeach()
execute_process(
  COMMAND "${PYTHON}" "${pet_volume_patcher}"
    --vendor-dir "${pet_volume_codec_dir}" --output-dir "${pet_volume_generated}"
  RESULT_VARIABLE pet_volume_patch_status)
if(NOT pet_volume_patch_status EQUAL 0)
  message(FATAL_ERROR "Master-volume codec source hash/patch verification failed")
endif()
get_target_property(pet_volume_sources ${pet_volume_codec_lib} SOURCES)
set(pet_volume_replaced_sources)
set(pet_volume_replaced_count 0)
foreach(pet_volume_src IN LISTS pet_volume_sources)
  get_filename_component(pet_volume_abs "${pet_volume_src}" REALPATH BASE_DIR "${pet_volume_codec_dir}")
  set(pet_volume_new "${pet_volume_src}")
  foreach(pet_volume_rel IN LISTS pet_volume_vendor_sources)
    get_filename_component(pet_volume_original "${pet_volume_codec_dir}/${pet_volume_rel}" REALPATH)
    if(pet_volume_abs STREQUAL pet_volume_original)
      set(pet_volume_new "${pet_volume_generated}/${pet_volume_rel}")
      math(EXPR pet_volume_replaced_count "${pet_volume_replaced_count}+1")
    endif()
  endforeach()
  list(APPEND pet_volume_replaced_sources "${pet_volume_new}")
endforeach()
if(NOT pet_volume_replaced_count EQUAL 3)
  message(FATAL_ERROR "Master-volume codec target must replace exactly three pinned sources")
endif()
set_property(TARGET ${pet_volume_codec_lib} PROPERTY SOURCES "${pet_volume_replaced_sources}")
target_include_directories(${pet_volume_codec_lib} PRIVATE
  "${pet_volume_main}" "${pet_volume_codec_dir}" "${pet_volume_codec_dir}/device/es8311")
target_sources(${COMPONENT_LIB} PRIVATE
  "${pet_volume_main}/pet_p4_audio_volume.c"
  "${pet_volume_main}/pet_p4_audio_volume_deadline.c")
# Only these new cold modules use -Os; existing audio/DSP/vendor paths retain flags.
set_source_files_properties(
  "${pet_volume_main}/pet_p4_audio_volume.c"
  "${pet_volume_main}/pet_p4_audio_volume_deadline.c"
  PROPERTIES COMPILE_OPTIONS "-Os")
'''


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vendor-dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--print-cmake", action="store_true")
    args = parser.parse_args()
    if args.print_cmake:
        print(cmake_integration(), end="")
    elif args.vendor_dir and args.output_dir:
        generate(args.vendor_dir, args.output_dir)
    else:
        parser.error("provide --vendor-dir and --output-dir, or --print-cmake")
