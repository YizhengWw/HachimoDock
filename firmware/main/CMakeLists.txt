# [Input] ESP-IDF component sources, build identity, and canonical PC built-in component JSON.
# [Output] P4 runtime component with all eight sprite-complete built-ins embedded in the A/B application image.
# [Pos] ESP32-P4 main firmware component build definition.
# [Sync] If this file changes, update `firmware/.folder.md` and `protocol.md`.

idf_component_register(
  SRCS
    "pet_p4_main.c"
    "pet_p4_lcd.c"
    "pet_p4_assets.c"
    "pet_p4_audio.c"
    "pet_p4_behavior.c"
    "pet_p4_diagnostics.c"
    "pet_p4_font_cn.c"
    "pet_p4_game.c"
    "pet_p4_input.c"
    "pet_p4_input_core.c"
    "pet_p4_miniapp.c"
    "pet_p4_builtin_components_data.c"
    "pet_p4_ota.c"
    "pet_p4_protocol.c"
    "pet_p4_raw_assets.c"
    "pet_p4_stats.c"
    "pet_p4_touch.c"
    "pet_p4_touch_core.c"
    "pet_p4_usb_native.c"
    "pet_p4_usb_descriptors.c"
    "pet_p4_renderer.c"
    "pet_p4_view.c"
  INCLUDE_DIRS
    "."
  REQUIRES
    esp_adc
    esp_timer
    esp_driver_gpio
    esp_driver_jpeg
    esp_driver_ppa
    esp_driver_uart
    esp_driver_usb_serial_jtag
    espressif__esp_h264
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
