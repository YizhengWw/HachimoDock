cmake_minimum_required(VERSION 3.16)

project(board_runtime_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_definitions(_GNU_SOURCE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
if(APPLE)
  add_compile_definitions(_DARWIN_C_SOURCE)
endif()

option(BOARD_RUNTIME_BUILD_TESTS "Build board runtime unit tests" ON)

add_library(board_runtime_common STATIC
  src/runtime_common.c
  src/runtime_json.c
  src/runtime_protocol.c
  src/runtime_mqtt.c
  src/runtime_usb_serial.c
  src/runtime_pairing.c
  src/runtime_debug.c
  src/runtime_session_state.c
  src/runtime_stats.c
  src/rotary_decoder.c
  src/screen_page.c
  src/touch_gesture.c
  src/voice_button.c
  src/runtime_wifi.c
)

target_include_directories(board_runtime_common PUBLIC src)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(board_runtime_common PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(ZLIB REQUIRED)
target_link_libraries(board_runtime_common PUBLIC ZLIB::ZLIB)

find_package(Threads REQUIRED)
target_link_libraries(board_runtime_common PUBLIC Threads::Threads)

if(UNIX AND NOT APPLE)
  target_link_libraries(board_runtime_common PUBLIC m)
elseif(APPLE)
  target_link_libraries(board_runtime_common PUBLIC m)
endif()

add_executable(board-server src/board_server.c)
target_link_libraries(board-server PRIVATE board_runtime_common)

add_executable(board-touch-input src/board_touch_input.c)
target_link_libraries(board-touch-input PRIVATE board_runtime_common)

add_executable(board-rotary-input src/board_rotary_input.c)
target_link_libraries(board-rotary-input PRIVATE board_runtime_common)

add_executable(board-serial-bridge src/board_serial_bridge.c)
target_link_libraries(board-serial-bridge PRIVATE board_runtime_common)

add_executable(fb-speech-overlay src/fb_speech_overlay.c)
target_link_libraries(fb-speech-overlay PRIVATE board_runtime_common)

if(BOARD_RUNTIME_BUILD_TESTS)
  enable_testing()
  add_executable(board-runtime-tests tests/runtime_tests.c)
  target_link_libraries(board-runtime-tests PRIVATE board_runtime_common)
  add_test(NAME board-runtime-tests COMMAND board-runtime-tests)

  add_executable(runtime-core-tests tests/runtime_core_tests.c)
  target_link_libraries(runtime-core-tests PRIVATE board_runtime_common)
  add_test(NAME runtime-core-tests COMMAND runtime-core-tests)

  add_executable(asset-transaction-tests tests/asset_transaction_tests.c)
  target_link_libraries(asset-transaction-tests PRIVATE board_runtime_common)
  add_test(NAME asset-transaction-tests COMMAND asset-transaction-tests)

  add_executable(fb-speech-overlay-layout-tests tests/fb_speech_overlay_layout_tests.c)
  target_link_libraries(fb-speech-overlay-layout-tests PRIVATE board_runtime_common)
  add_test(NAME fb-speech-overlay-layout-tests COMMAND fb-speech-overlay-layout-tests)
  set_tests_properties(fb-speech-overlay-layout-tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_executable(rotary-decoder-tests tests/rotary_decoder_tests.c)
  target_link_libraries(rotary-decoder-tests PRIVATE board_runtime_common)
  add_test(NAME rotary-decoder-tests COMMAND rotary-decoder-tests)

  add_executable(runtime-wifi-tests tests/runtime_wifi_tests.c)
  target_link_libraries(runtime-wifi-tests PRIVATE board_runtime_common)
  add_test(NAME runtime-wifi-tests COMMAND runtime-wifi-tests)

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME fb-display-selftest
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fb_display_selftest.py
    )
    add_test(NAME fb-stats-renderer-test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fb_stats_renderer_test.py
    )
    add_test(NAME rpi-pairing-ap-config-test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/rpi_pairing_ap_config_test.py
    )
  endif()

  add_test(NAME fb-device-resolver-tests
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/fb_device_resolver_tests.sh
  )
  set_tests_properties(fb-device-resolver-tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )

  find_program(Node_EXECUTABLE node)
  if(Node_EXECUTABLE)
    add_test(NAME pet-animation-config-test
      COMMAND ${Node_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pet_animation_config_test.js
    )
    add_test(NAME pet-ui-logic-tests
      COMMAND ${Node_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pet_ui_logic_tests.js
    )
  endif()
endif()
