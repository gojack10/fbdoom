/* FluidSynth main header — fbdoom embedded build */
#ifndef _FLUIDSYNTH_H
#define _FLUIDSYNTH_H

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLUIDSYNTH_API
#define FLUID_DEPRECATED

#include "fluidsynth/types.h"
#include "fluidsynth/settings.h"
#include "fluidsynth/synth.h"
#include "fluidsynth/shell.h"
#include "fluidsynth/sfont.h"
#include "fluidsynth/audio.h"
#include "fluidsynth/event.h"
#include "fluidsynth/midi.h"
#include "fluidsynth/seq.h"
#include "fluidsynth/seqbind.h"
#include "fluidsynth/log.h"
#include "fluidsynth/misc.h"
#include "fluidsynth/mod.h"
#include "fluidsynth/gen.h"
#include "fluidsynth/voice.h"
#include "fluidsynth/version.h"

#ifdef __cplusplus
}
#endif

#endif /* _FLUIDSYNTH_H */