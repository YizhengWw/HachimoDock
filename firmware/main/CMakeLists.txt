# [Input] ESP-IDF component sources, build identity, and canonical PC built-in component JSON.
# [Output] P4 runtime with a bounded 29-component bootstrap and cached native launcher artwork.
# [Pos] ESP32-P4 main firmware component build definition.
# [Sync] If this file changes, update `firmware/.folder.md` and `protocol.md`.

idf_component_register(
  SRCS
    "pet_p4_main.c"
    "pet_p4_lcd.c"
    "pet_p4_assets.c"
    "pet_p4_audio.c"
    "pet_p4_audio_patch.c"
    "pet_p4_instrument_core.c"
    "pet_p4_sfx_core.c"
    "pet_p4_component_events.c"
    "pet_p4_component_ext.c"
    "pet_p4_component_media.c"
    "pet_p4_system_cues.c"
    "pet_p4_cue_packed.c"
    "pet_p4_audio_frontend.c"
    "pet_p4_behavior.c"
    "pet_p4_conversation.c"
    "pet_p4_diagnostics.c"
    "pet_p4_reset_record.c"
    "pet_p4_reset_phase.c"
    "pet_p4_reset_cost.c"
    "pet_p4_boot_trace.c"
    "pet_p4_boot_dma.c"
    "pet_p4_flash_phase.c"
    "pet_p4_native_panic.c"
    # MiSans16/32 share the original license; see licenses/MiSans-Font-License-Agreement.pdf
    # and licenses/MiSans-Embedded-Fonts-NOTICE.txt at the repository root.
    "pet_p4_font_cn.c"
    "pet_p4_font_cn_32.c"
    "pet_p4_font_bitmap.c"
    "pet_p4_bubble_pixels.c"
    "pet_p4_game.c"
    "pet_p4_side_scene.c"
    "pet_p4_side_scene_json.c"
    "pet_p4_input.c"
    "pet_p4_input_core.c"
    "pet_p4_miniapp.c"
    "pet_p4_local_tools.c"
    "pet_p4_app_store.c"
    "pet_p4_storage_lane.c"
    "pet_p4_selection.c"
    "pet_p4_app_host.c"
    "pet_p4_resources.c"
    "pet_p4_resources_device.c"
    "pet_p4_native_work_window.c"
    "pet_p4_control_session.c"
    "pet_p4_widget_trace.c"
    "pet_p4_transport_audit.c"
    "pet_p4_sched_trace.c"
    "pet_p4_sched_trace_rpc.c"
    "pet_p4_spiffs_hint.c"
    "pet_p4_storage_io_trace.c"
    "pet_p4_storage_io_trace_rpc.c"
    "pet_p4_file_cleanup.c"
    "pet_p4_fs_activity.c"
    "pet_p4_program_core.c"
    "pet_p4_cells.c"
    "pet_p4_program_image.c"
    "pet_p4_program_runtime.c"
    "pet_p4_program_timers.c"
    "pet_p4_event_bus.c"
    "pet_p4_observation.c"
    "pet_p4_collection.c"
    "pet_p4_collection_host.c"
    "pet_p4_host_collection.c"
    "pet_p4_program_host.c"
    "pet_p4_program_group.c"
    "pet_p4_program_scheduler.c"
    "pet_p4_program_registry.c"
    "pet_p4_program_host_device.c"
    "pet_p4_program_store.c"
    "pet_p4_program_persistence.c"
    "pet_p4_program_activation.c"
    "pet_p4_component_program.c"
    "pet_p4_variable_owners.c"
    "pet_p4_component_program_session.c"
    "pet_p4_program_probe.c"
    "pet_p4_program_publication_probe.c"
    "pet_p4_program_session_probe.c"
    "pet_p4_program_main_probe.c"
    "pet_p4_trusted_services.c"
    "pet_p4_trusted_services_device.c"
    "pet_p4_surface.c"
    "pet_p4_canvas.c"
    "pet_p4_particles.c"
    "pet_p4_form_v2.c"
    "pet_p4_form_v3.c"
    "pet_p4_widget_data.c"
    "pet_p4_media.c"
    "pet_p4_media_codec.c"
    "pet_p4_builtin_components_data.c"
    "pet_p4_ota.c"
    "pet_p4_protocol.c"
    "pet_p4_raw_assets.c"
    "pet_p4_stats.c"
    "pet_p4_touch.c"
    "pet_p4_touch_core.c"
    "pet_p4_slice_core.c"
    "pet_p4_fruit_core.c"
    "pet_p4_usb_native.c"
    "pet_p4_usb_descriptors.c"
    "pet_p4_renderer.c"
    "pet_p4_view.c"
  INCLUDE_DIRS
    "."
  PRIV_INCLUDE_DIRS
    "${IDF_PATH}/components/spiffs/spiffs/src"
  REQUIRES
    esp_adc
    esp_timer
    esp_driver_gpio
    esp_driver_jpeg
    esp_driver_ppa
    esp_driver_uart
    esp_driver_usb_serial_jtag
    espressif__esp_h264
    espressif__esp-sr
    esp_lcd
    esp_partition
    usb
    espressif__esp_lcd_touch
    espressif__esp_lcd_touch_gt911
    app_update
    json
    lvgl
    mbedtls
    nvs_flash
    spiffs
    tinyusb
)

# Shared, OTA-updatable status cues live in the application, not appearance slots.
# Retain compiler stack reports for the nested component/media render path.
# Large snapshots belong to renderer-owned storage, not the 10 KiB main stack.
set_source_files_properties("pet_p4_renderer.c" PROPERTIES COMPILE_OPTIONS "-fstack-usage;-Wframe-larger-than=4096")
# Bound declaration parsing/interpreter code growth as the PetUI contract grows.
# Keep audio DSP, video and per-pixel rendering at the configured performance
# optimization; the factory partition layout is validated separately.
set_source_files_properties("pet_p4_surface.c" "pet_p4_miniapp.c" PROPERTIES COMPILE_OPTIONS "-Os")
# New declaration/controller logic is not a pixel/DSP hot loop. Keep its OTA
# cost bounded without changing the VM, audio, renderer or partition budgets.
set_source_files_properties("pet_p4_component_program.c" "pet_p4_component_program_session.c" "pet_p4_variable_owners.c" PROPERTIES COMPILE_OPTIONS "-Os")
# Bounded lifecycle routing, never instructions, DSP or pixel processing.
set_source_files_properties("pet_p4_program_group.c" "pet_p4_program_scheduler.c" PROPERTIES COMPILE_OPTIONS "-Os")
# Cold install-time declaration validation and diagnostic serialization only.
set_source_files_properties("pet_p4_side_scene_json.c" "pet_p4_diagnostics.c" PROPERTIES COMPILE_OPTIONS "-Os")
# Admission/queue/lifecycle/serialization controllers are separate from the
# unchanged integer instruction/runtime implementation and audio/render loops.
set_source_files_properties("pet_p4_program_host.c" "pet_p4_program_timers.c" "pet_p4_event_bus.c" "pet_p4_program_store.c" "pet_p4_app_store.c" PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties("pet_p4_program_persistence.c" "pet_p4_program_publication_probe.c" "pet_p4_ota.c" PROPERTIES COMPILE_OPTIONS "-Os")
# Opt-in fixed diagnostics are control/formatting code, not the interpreter or
# an audio/render hot loop. Keep measured production core optimization intact.
set_source_files_properties("pet_p4_program_probe.c" PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties("pet_p4_program_session_probe.c" PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties("pet_p4_program_main_probe.c" PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties("pet_p4_sched_trace_rpc.c" PROPERTIES COMPILE_OPTIONS "-Os")
# The reviewed v4 report consumes six actual module reports. Append this after
# the per-source optimization properties so their existing flags remain intact.
set_property(SOURCE "pet_p4_miniapp.c" "pet_p4_app_host.c" "pet_p4_form_v2.c"
  "pet_p4_surface.c" "pet_p4_host_collection.c"
  APPEND PROPERTY COMPILE_OPTIONS "-fstack-usage")

foreach(layer body mallet)
  target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/instruments/wooden_fish_${layer}.deflate" BINARY)
endforeach()
foreach(art center_background.jpg center_thumbs.deflate)
  target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/component-center/${art}" BINARY)
endforeach()
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pet_cn_16.deflate" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pet_cn_metrics.deflate" BINARY)
# Both inputs exist; only the gate-selected metrics symbols may survive GC.
# Parent must confirm this again in BOTH final linked images/maps.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pet_cn_metrics_columns.deflate" BINARY)
execute_process(
  COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/compress_font_native32.py" "--check"
  RESULT_VARIABLE PET_FONT_NATIVE32_STATUS
)
if(NOT PET_FONT_NATIVE32_STATUS EQUAL 0)
  message(FATAL_ERROR "Native32 source/identity asset is stale")
endif()
execute_process(
  COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/compress_font_metrics_columns.py"
    "--source" "${CMAKE_CURRENT_LIST_DIR}/pet_p4_font_cn.c" "--check"
  RESULT_VARIABLE PET_FONT_COLUMNS_STATUS
)
if(NOT PET_FONT_COLUMNS_STATUS EQUAL 0)
  message(FATAL_ERROR "Native32 metric columns are stale")
endif()
execute_process(
  COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/compress_font_metrics.py" "--check"
  RESULT_VARIABLE PET_FONT_METRICS_STATUS
)
if(NOT PET_FONT_METRICS_STATUS EQUAL 0)
  message(FATAL_ERROR "Stale lossless font metric asset")
endif()
execute_process(
  COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/compress_font_asset.py" "--check"
  RESULT_VARIABLE PET_FONT_PACK_STATUS
)
if(NOT PET_FONT_PACK_STATUS EQUAL 0)
  message(FATAL_ERROR "Lossless font asset is stale; run tools/compress_font_asset.py")
endif()
execute_process(
  COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/compress_instrument_assets.py" "--check"
  RESULT_VARIABLE PET_INSTRUMENT_PACK_STATUS
)
if(NOT PET_INSTRUMENT_PACK_STATUS EQUAL 0)
  message(FATAL_ERROR "Lossless instrument assets are stale; run tools/compress_instrument_assets.py")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../pc/public/terrier-clips/error.wav" PET_ERROR_CUE_SHA)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../pc/public/terrier-clips/waiting_user.wav" PET_WAIT_CUE_SHA)
if(NOT PET_ERROR_CUE_SHA STREQUAL PET_WAIT_CUE_SHA)
  message(FATAL_ERROR "Shared waiting/error cue changed: restore separate embedding before shipping")
endif()
# All seven factory cues keep exact PCM, stored once as lossless packed
# C data. Never also embed the original WAVs, which would erase the OTA saving.
execute_process(
  COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/compress_system_cues.py" "--check"
  RESULT_VARIABLE PET_CUE_PACKED_STATUS
)
if(NOT PET_CUE_PACKED_STATUS EQUAL 0)
  message(FATAL_ERROR "Stale lossless factory cue payload; run tools/compress_system_cues.py")
endif()

execute_process(
  COMMAND
    "${PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/build_builtin_bundle.py"
    "--check"
  WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
  RESULT_VARIABLE PET_P4_BUILTIN_BUNDLE_STATUS
)
if(NOT PET_P4_BUILTIN_BUNDLE_STATUS EQUAL 0)
  message(FATAL_ERROR "Embedded built-in component bundle is stale; run tools/build_builtin_bundle.py")
endif()

target_compile_definitions(
  ${COMPONENT_LIB}
  PRIVATE
    PET_P4_BUILD_GIT_SHA="${PET_P4_GIT_SHA}"
    PET_P4_BUILD_ID="${PET_P4_BUILD_ID}"
    PET_P4_BUILD_DIRTY=${PET_P4_BUILD_DIRTY}
    PET_P4_PROTOCOL_SCHEMA=${PET_P4_PROTOCOL_SCHEMA}
    PET_P4_UART_BAUD=${PET_P4_UART_BAUD}
)

idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
  set(tinyusb_name tinyusb)
else()
  set(tinyusb_name espressif__tinyusb)
endif()

idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
# pet_p4_usb_descriptors.c is compiled in the main component and provides the
# TinyUSB descriptor callbacks while this include path exposes tusb_config.h.

cmake_policy(SET CMP0079 NEW)
target_link_libraries(${tusb_lib} PRIVATE ${COMPONENT_LIB})

# Paste at the end of firmware/main/CMakeLists.txt (after component registration).
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


# Pin the SDK restart implementation; V1 receives only the native V3 AHB reset
# pair in a build-local copy. The installed SDK and V3 source stay untouched.
set(pet_restart_patch "${CMAKE_CURRENT_LIST_DIR}/../tools/patch_p4_restart_ahb.py")
set(pet_restart_original "${IDF_PATH}/components/esp_system/port/soc/esp32p4/system_internal.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pet_restart_patch}" "${pet_restart_original}")
execute_process(COMMAND "${PYTHON}" "${pet_restart_patch}" --sdk-dir "${IDF_PATH}" --build-dir "${CMAKE_BINARY_DIR}"
  RESULT_VARIABLE pet_restart_status OUTPUT_VARIABLE pet_restart_selected OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT pet_restart_status EQUAL 0)
  message(FATAL_ERROR "Unrecognized P4 SDK restart source or invalid generated backport")
endif()
idf_component_get_property(pet_restart_lib esp_system COMPONENT_LIB)
idf_component_get_property(pet_restart_dir esp_system COMPONENT_DIR)
get_target_property(pet_restart_sources ${pet_restart_lib} SOURCES)
set(pet_restart_replaced)
set(pet_restart_count 0)
foreach(pet_restart_src IN LISTS pet_restart_sources)
  get_filename_component(pet_restart_abs "${pet_restart_src}" REALPATH BASE_DIR "${pet_restart_dir}")
  if(pet_restart_abs STREQUAL pet_restart_original)
    list(APPEND pet_restart_replaced "${pet_restart_selected}")
    math(EXPR pet_restart_count "${pet_restart_count}+1")
  else()
    list(APPEND pet_restart_replaced "${pet_restart_src}")
  endif()
endforeach()
if(NOT pet_restart_count EQUAL 1)
  message(FATAL_ERROR "P4 restart target must contain exactly one pinned source")
endif()
set_property(TARGET ${pet_restart_lib} PROPERTY SOURCES "${pet_restart_replaced}")
