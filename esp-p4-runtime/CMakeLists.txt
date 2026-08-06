# [Input] ESP-IDF environment, project sources, and optional Git build identity.
# [Output] Versioned ESP32-P4 firmware project with protocol/build metadata.
# [Pos] Firmware build entry for esp-p4-runtime.
# [Sync] If this file changes, update `esp-p4-runtime/.folder.md` and README.md.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.7.29-p4")
set(PET_P4_PROTOCOL_SCHEMA 5)
set(PET_P4_GIT_SHA "unknown")
set(PET_P4_BUILD_DIRTY 0)
find_program(PET_P4_GIT_EXECUTABLE git)
if(PET_P4_GIT_EXECUTABLE)
  execute_process(
    COMMAND ${PET_P4_GIT_EXECUTABLE} -C ${CMAKE_CURRENT_LIST_DIR} rev-parse --short=12 HEAD
    RESULT_VARIABLE PET_P4_GIT_SHA_RESULT
    OUTPUT_VARIABLE PET_P4_GIT_SHA_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(PET_P4_GIT_SHA_RESULT EQUAL 0 AND NOT PET_P4_GIT_SHA_OUTPUT STREQUAL "")
    set(PET_P4_GIT_SHA "${PET_P4_GIT_SHA_OUTPUT}")
  endif()
  execute_process(
    COMMAND ${PET_P4_GIT_EXECUTABLE} -C ${CMAKE_CURRENT_LIST_DIR} status --porcelain --untracked-files=normal
    RESULT_VARIABLE PET_P4_GIT_STATUS_RESULT
    OUTPUT_VARIABLE PET_P4_GIT_STATUS_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(PET_P4_GIT_STATUS_RESULT EQUAL 0 AND NOT PET_P4_GIT_STATUS_OUTPUT STREQUAL "")
    set(PET_P4_BUILD_DIRTY 1)
  endif()
endif()
set(PET_P4_BUILD_ID "${PROJECT_VER}+${PET_P4_GIT_SHA}")
if(PET_P4_BUILD_DIRTY)
  string(APPEND PET_P4_BUILD_ID "-dirty")
endif()
message(STATUS "P4 build identity: ${PET_P4_BUILD_ID}, protocol schema ${PET_P4_PROTOCOL_SCHEMA}")
project(pet_manager_p4_runtime)
