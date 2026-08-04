/*
 * [Input] Build identity values injected by the top-level CMake project.
 * [Output] Safe fallback build identity macros for standalone source tooling.
 * [Pos] Shared compile-time build identity contract for the ESP32-P4 runtime.
 * [Sync] If this file changes, update esp-p4-runtime/.folder.md.
 */

#pragma once

#ifndef PET_P4_BUILD_GIT_SHA
#define PET_P4_BUILD_GIT_SHA "unknown"
#endif

#ifndef PET_P4_BUILD_ID
#define PET_P4_BUILD_ID "unknown"
#endif

#ifndef PET_P4_BUILD_DIRTY
#define PET_P4_BUILD_DIRTY 0
#endif

#ifndef PET_P4_PROTOCOL_SCHEMA
#define PET_P4_PROTOCOL_SCHEMA 0
#endif
