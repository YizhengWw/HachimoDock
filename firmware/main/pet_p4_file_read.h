/* Bounded extent of an already-open, privately owned seekable file.
 * No read beyond EOF and no clearerr/SPIFFS error reinterpretation. This is not
 * locking: the caller keeps its exact storage lease/path ownership, checks I/O
 * errors and verifies post-read extent/content before publication. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef enum {PET_FILE_SIZE_OK,PET_FILE_SIZE_BOUNDS,PET_FILE_SIZE_IO} pet_file_size_result_t;
/* On success starts at offset zero; on failure output is unchanged, the caller
 * must still close the file. Range rejection is distinct from an I/O failure
 * so a private save cannot turn unavailable data into an invented empty save. */
static inline pet_file_size_result_t pet_file_read_extent(FILE *file,size_t limit,size_t *out){
  if(!file||!out)return PET_FILE_SIZE_BOUNDS;
  if(fseek(file,0,SEEK_END)!=0)return PET_FILE_SIZE_IO;
  long end=ftell(file);
  if(end<0)return PET_FILE_SIZE_IO;
  if((uintmax_t)end>(uintmax_t)limit)return PET_FILE_SIZE_BOUNDS;
  if(fseek(file,0,SEEK_SET)!=0)return PET_FILE_SIZE_IO;
  *out=(size_t)end;return PET_FILE_SIZE_OK;
}
