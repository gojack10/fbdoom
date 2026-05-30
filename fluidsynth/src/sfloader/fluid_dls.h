/* fluid_dls.h stub — ENABLE_NATIVE_DLS not defined */
#ifndef _FLUID_DLS_H
#define _FLUID_DLS_H

#if defined(ENABLE_NATIVE_DLS)
typedef struct _fluid_sfont_t fluid_sfont_t;
typedef struct _fluid_synth_t fluid_synth_t;
fluid_sfont_t *new_fluid_dls_loader(fluid_synth_t *synth, void *settings);
#endif

#endif /* _FLUID_DLS_H */