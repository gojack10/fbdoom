/* fluid_ladspa.h stub — LADSPA disabled for embedded build */
#ifndef _FLUID_LADSPA_H
#define _FLUID_LADSPA_H

#if !LADSPA

/* Empty stubs when LADSPA is disabled */
typedef struct _fluid_ladspa_fx_t fluid_ladspa_fx_t;

static inline fluid_ladspa_fx_t *new_fluid_ladspa_fx(void *rvoice) { return NULL; }
static inline void delete_fluid_ladspa_fx(fluid_ladspa_fx_t *fx) { }
static inline int fluid_ladspa_fx_process(fluid_ladspa_fx_t *fx, int len, float *in, float *out) { return FLUID_OK; }

#else
#include "fluidsynth/ladspa.h"
#endif

#endif /* _FLUID_LADSPA_H */