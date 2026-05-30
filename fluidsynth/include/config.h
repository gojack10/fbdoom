#ifndef CONFIG_H
#define CONFIG_H

/* No external drivers or backends */
/* No ALSA, JACK, OSS, PulseAudio, PortAudio, CoreAudio, etc. */
/* No network, no readline, no LADSPA, no limiter */

/* Headers available on both macOS and Android NDK */
#define HAVE_ERRNO_H        1
#define HAVE_FCNTL_H        1
#define HAVE_INTTYPES_H     1
#define HAVE_LIMITS_H       1
#define HAVE_MATH_H         1
#define HAVE_PTHREAD_H      1
#define HAVE_SIGNAL_H       1
#define HAVE_STDARG_H       1
#define HAVE_STDINT_H       1
#define HAVE_STDIO_H        1
#define HAVE_STDLIB_H       1
#define HAVE_STRING_H       1
#define HAVE_STRINGS_H      1
#define HAVE_SYS_STAT_H     1
#define HAVE_SYS_TIME_H     1
#define HAVE_SYS_TYPES_H    1
#define HAVE_UNISTD_H       1
#define STDC_HEADERS        1

/* Single-precision floating point DSP */
#define WITH_FLOAT          1

/* C math functions */
#define HAVE_SINF           1
#define HAVE_COSF           1
#define HAVE_FABSF          1
#define HAVE_POWF           1
#define HAVE_SQRTF          1
#define HAVE_LOGF           1

/* OSAL: use cpp11 (C++11 threading primitives) */
#define OSAL_cpp11          1

/* No GUI dialogs */
#define NO_GUI              1

/* Deprecation attribute (not needed for embedded build) */
#define FLUID_DEPRECATED

/* Support VLA if compiler allows */
#define SUPPORTS_VLA        1

#endif /* CONFIG_H */