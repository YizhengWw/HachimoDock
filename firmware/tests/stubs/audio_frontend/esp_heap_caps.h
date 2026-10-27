#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
void *heap_caps_malloc(size_t size, unsigned caps);
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
void *heap_caps_aligned_alloc(size_t align, size_t size, unsigned caps);
void heap_caps_free(void *pointer);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
