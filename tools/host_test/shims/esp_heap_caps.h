#pragma once
/* Stand-in: on a computer all memory is the same. */
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_calloc(n, size, caps) calloc(n, size)
#define heap_caps_realloc(p, size, caps) realloc(p, size)
