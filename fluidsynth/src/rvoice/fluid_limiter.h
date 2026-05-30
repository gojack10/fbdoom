/* fluid_limiter.h stub — LIMITER_SUPPORT not defined */
#ifndef _FLUID_LIMITER_H
#define _FLUID_LIMITER_H

#if defined(LIMITER_SUPPORT)
/* Real limiter types when enabled */
typedef struct _fluid_limiter_t fluid_limiter_t;
typedef struct _fluid_limiter_settings_t {
    int enable;
    float level;
} fluid_limiter_settings_t;
#else
/* Empty stubs */
typedef struct _fluid_limiter_t fluid_limiter_t;
typedef struct _fluid_limiter_settings_t { int dummy; } fluid_limiter_settings_t;
#endif

#endif /* _FLUID_LIMITER_H */