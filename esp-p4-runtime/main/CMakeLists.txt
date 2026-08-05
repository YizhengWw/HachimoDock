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

target_compile_definitions(
  ${COMPONENT_LIB}
  PRIVATE
    PET_P4_BUILD_GIT_SHA="${PET_P4_GIT_SHA}"
    PET_P4_BUILD_ID="${PET_P4_BUILD_ID}"
    PET_P4_BUILD_DIRTY=${PET_P4_BUILD_DIRTY}
    PET_P4_PROTOCOL_SCHEMA=${PET_P4_PROTOCOL_SCHEMA}
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
