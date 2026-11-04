/* Mount-memory policy only, not a package quota or storage format change.
 * IDF SPIFFS sizes BOTH descriptor/name hints and page cache from max_files.
 * Ordinary targets retain eight. A larger pool requires a deliberately private
 * comparison build and runtime heap/render measurements before promotion.
 * Keep below bit 31: the audited SDK uses signed 1<<index in its cache bitmap.
 * No mount fallback may format, migrate, erase or reset existing user data. */
#pragma once
#ifndef PET_P4_STORAGE_CACHE_EXPERIMENT
#define PET_P4_STORAGE_CACHE_EXPERIMENT 0
#endif
#ifndef PET_P4_SPIFFS_MAX_FILES
#define PET_P4_SPIFFS_MAX_FILES 8
#endif
#if PET_P4_SPIFFS_MAX_FILES < 1 || PET_P4_SPIFFS_MAX_FILES > 31
#error "SPIFFS descriptor/cache pool outside reviewed bitmap bounds"
#endif
#if PET_P4_SPIFFS_MAX_FILES != 8 && !PET_P4_STORAGE_CACHE_EXPERIMENT
#error "non-default storage cache requires explicit private experiment"
#endif
