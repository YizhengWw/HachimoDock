#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Small navigation metadata only. Does not own package files or saved progress. */
bool pet_selection_read(uint32_t catalog_sequence,char out[48],bool *record_exists);
bool pet_selection_write(uint32_t catalog_sequence,const char *id);
