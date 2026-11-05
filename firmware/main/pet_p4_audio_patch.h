#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
/* Namespace 2 is global audio storage, never a physical appearance slot. */
#define PET_AUDIO_PATCH_GLOBAL_SLOT 2
#define PET_AUDIO_PATCH_GLOBAL_PACK "0000000000000000000000000000000000000000000000000000000000000001"
#define PET_AUDIO_PATCH_MAX_FILES 24U
#define PET_AUDIO_PATCH_MAX_BYTES (2U*1024U*1024U)
#define PET_AUDIO_PATCH_MAX_FILE (1024U*1024U+4096U)
#define PET_AUDIO_PATCH_CHUNK_BYTES 4096U
/* All calls require the existing physical appearance STORAGE_WORK owner, except
 * resolve which runs only under the playback worker's realtime owner. These
 * mutually exclude each other; this module never takes the runtime state lock. */
bool pet_audio_patch_begin(int slot,const char *pack,const cJSON *files,uint32_t total,bool *attempted);
bool pet_audio_patch_chunk(const char *path,uint32_t index,uint32_t offset,const uint8_t *bytes,size_t size,bool *attempted);
bool pet_audio_patch_commit(bool *attempted,uint32_t *generation);
bool pet_audio_patch_abort(bool *attempted);
void pet_audio_patch_retire(void);
bool pet_audio_patch_progress(const char *path,uint32_t *index,uint32_t *bytes);
unsigned pet_audio_patch_file_count(void);
uint32_t pet_audio_patch_total_bytes(void);
bool pet_audio_patch_resolve(int slot,const char *pack,const char *family,char *out,size_t capacity);

/* Lock-free admission hints: refreshed before RX tasks at boot and after an
 * acknowledged global publication. No FS or runtime mutex in enabled(). */
void pet_audio_patch_global_refresh(void);
bool pet_audio_patch_global_enabled(const char *family);
bool pet_audio_patch_global_family_known(const char *family);
