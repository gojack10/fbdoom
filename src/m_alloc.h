/*
** m_alloc.h
** Minimal allocator wrappers for TArray compatibility.
** M_Malloc/M_Free are used by tarray.h; we define them as std lib wrappers.
*/

#ifndef __M_ALLOC__
#define __M_ALLOC__

#include <stdlib.h>

static inline void *M_Malloc(size_t size) {
    return malloc(size);
}

static inline void M_Free(void *ptr) {
    free(ptr);
}

#endif
