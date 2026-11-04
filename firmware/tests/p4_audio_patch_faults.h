#pragma once
#include <stdio.h>
#include <stddef.h>
int patch_test_fsync(int fd);
FILE *patch_index_open(const char *,const char *);
size_t patch_index_read(void *,size_t,size_t,FILE *);
int patch_index_error(FILE *);
int patch_index_close(FILE *);
int patch_remove(const char *);
