# [Input] ESP-IDF component sources, build identity, and canonical PC built-in component JSON.
# [Output] P4 runtime with a bounded 28-component bootstrap and cached native launcher artwork.
# [Pos] ESP32-P4 main firmware component build definition.
# [Sync] If this file changes, update `firmware/.folder.md` and `protocol.md`.

idf_component_register(
  SRCS
    "pet_p4_main.c"
    "pet_p4_lcd.c"
    "pet_p4_assets.c"
    "pet_p4_audio.c"
    "pet_p4_instrument_core.c"
    "pet_p4_system_cues.c"
    "pet_p4_audio_frontend.c"
    "pet_p4_behavior.c"
    "pet_p4_conversation.c"
    "pet_p4_diagnostics.c"
    "pet_p4_font_cn.c"
    "pet_p4_game.c"
    "pet_p4_input.c"
    "pet_p4_input_core.c"
    "pet_p4_miniapp.c"
    "pet_p4_net_core.c"
    "pet_p4_network.c"
    "pet_p4_local_tools.c"
    "pet_p4_app_store.c"
    "pet_p4_app_host.c"
    "pet_p4_surface.c"
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
  REQUIRES
    esp_adc
    esp_event
    esp_netif
    esp_timer
    esp_wifi
    espressif__esp_hosted
    espressif__esp_wifi_remote
    lwip
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

# Retain compiler stack reports for the nested component/media render path.
# Large snapshots belong to renderer-owned storage, not the 10 KiB main stack.
set_source_files_properties("pet_p4_renderer.c" PROPERTIES COMPILE_OPTIONS "-fstack-usage;-Wframe-larger-than=4096")
# Bound declaration parsing/interpreter code growth as the PetUI contract grows.
# Keep audio DSP, video and per-pixel rendering at the configured performance
# optimization; never enlarge OTA partitions to accommodate resource features.
set_source_files_properties("pet_p4_surface.c" "pet_p4_miniapp.c" PROPERTIES COMPILE_OPTIONS "-Os")
foreach(layer body mallet)
  target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/instruments/wooden_fish_${layer}.deflate" BINARY)
endforeach()
foreach(art center_background.jpg center_thumbs.deflate)
  target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/component-center/${art}" BINARY)
endforeach()
execute_process(
  COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/compress_instrument_assets.py" "--check"
  RESULT_VARIABLE PET_INSTRUMENT_PACK_STATUS
)
if(NOT PET_INSTRUMENT_PACK_STATUS EQUAL 0)
  message(FATAL_ERROR "Lossless instrument assets are stale; run tools/compress_instrument_assets.py")
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

# esp_hosted registers a boot-time constructor that brings up the C6 transport, resets the
# co-processor and aborts on failure. Wrapping its only caller turns that into a no-op;
# pet_p4_network.c calls __real_esp_hosted_init() on demand, after Wi-Fi was configured over USB.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_hosted_init")

# The co-processor drivers log every RPC at INFO; compiled out, those messages and their format
# strings cost tens of KB of the OTA slot. Warnings and errors stay. The application's own logs
# and ESP_ERROR_CHECK diagnostics are untouched.
foreach(wifi_component espressif__esp_hosted espressif__esp_wifi_remote)
  idf_component_get_property(wifi_lib ${wifi_component} COMPONENT_LIB)
  target_compile_definitions(${wifi_lib} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_WARN)
endforeach()

# The project builds for speed (-O2), but this link only moves status, session and key-event JSON.
# Optimizing the radio stack for size, as pet_p4_surface.c and pet_p4_miniapp.c already are, keeps
# it from consuming the OTA headroom that audio, video and rendering never get back.
foreach(wifi_component espressif__esp_hosted espressif__esp_wifi_remote lwip esp_netif esp_event sdmmc)
  idf_component_get_property(wifi_lib ${wifi_component} COMPONENT_LIB)
  target_compile_options(${wifi_lib} PRIVATE -Os)
endforeach()

cmake_policy(SET CMP0079 NEW)
target_link_libraries(${tusb_lib} PRIVATE ${COMPONENT_LIB})
