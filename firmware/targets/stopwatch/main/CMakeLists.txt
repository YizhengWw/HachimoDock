set(shared "${CMAKE_CURRENT_LIST_DIR}/../../../main")
idf_component_register(
  SRCS "stopwatch_main.c" "stopwatch_renderer.c"
       "${shared}/pet_device_sessions.c" "${shared}/pet_p4_input_core.c"
       "${shared}/pet_p4_font_cn.c"
  INCLUDE_DIRS "." "${shared}"
  REQUIRES board_stopwatch esp_driver_usb_serial_jtag esp_timer esp_hw_support
           esp_app_format nvs_flash json lvgl)
execute_process(COMMAND "${PYTHON}"
  "${CMAKE_CURRENT_LIST_DIR}/../../../tools/build_stopwatch_assets.py"
  --output "${CMAKE_BINARY_DIR}/stopwatch_pet.bin"
  RESULT_VARIABLE asset_status)
if(NOT asset_status EQUAL 0)
  message(FATAL_ERROR "StopWatch factory pet generation failed")
endif()
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/stopwatch_pet.bin" BINARY)
execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --short=12 HEAD
  OUTPUT_VARIABLE board_git_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT board_git_sha)
  set(board_git_sha "unknown")
endif()
execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" status --porcelain --untracked-files=normal
  OUTPUT_VARIABLE board_git_status ERROR_QUIET)
set(board_dirty 0)
set(board_suffix "")
if(board_git_status)
  set(board_dirty 1)
  set(board_suffix "-dirty")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
  HACHIMO_GIT_SHA="${board_git_sha}" HACHIMO_BUILD_DIRTY=${board_dirty}
  HACHIMO_BUILD_ID="${PROJECT_VER}+${board_git_sha}${board_suffix}")
# A bounded frame must not become an unbounded recursive parse on a 12 KiB stack.
idf_component_get_property(stopwatch_json_lib json COMPONENT_LIB)
target_compile_definitions(${stopwatch_json_lib} PRIVATE CJSON_NESTING_LIMIT=32)
