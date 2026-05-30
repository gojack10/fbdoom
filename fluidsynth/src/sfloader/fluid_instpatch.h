/* fluid_instpatch.h stub — LIBINSTPATCH_SUPPORT not defined */
#ifndef _FLUID_INSTPATCH_H
#define _FLUID_INSTPATCH_H

#if defined(LIBINSTPATCH_SUPPORT)
typedef struct _fluid_sfont_t fluid_sfont_t;
void fluid_instpatch_init(void);
void fluid_instpatch_deinit(void);
int fluid_instpatch_supports_multi_init(void);
fluid_sfont_t *new_fluid_instpatch_loader(void *settings);
#else
static inline void fluid_instpatch_init(void) {}
static inline void fluid_instpatch_deinit(void) {}
static inline int fluid_instpatch_supports_multi_init(void) { return 0; }
#endif

#endif /* _FLUID_INSTPATCH_H */