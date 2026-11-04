#pragma once
#include <stddef.h>
/* Pure form-v1 inline helpers declare this; form-v2 never calls formatted I/O. */
int snprintf(char *,size_t,const char *,...);
