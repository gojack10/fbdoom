/* Stubs for removed FluidSynth modules (shell, sequencer, drivers, tuning, monopoly) */
#include "fluidsynth.h"

/* Shell/settings stubs */
void fluid_shell_settings(fluid_settings_t *settings) {}
void fluid_player_settings(fluid_settings_t *settings) {}
void fluid_file_renderer_settings(fluid_settings_t *settings) {}

/* Driver settings stubs */
void fluid_audio_driver_settings(fluid_settings_t *settings) {}
void fluid_midi_driver_settings(fluid_settings_t *settings) {}

/* Tuning stubs */
void *new_fluid_tuning(const char *name, int bank, int prog) { return NULL; }
void delete_fluid_tuning(void *tuning) {}
void fluid_tuning_ref(void *tuning) {}
void fluid_tuning_unref(void *tuning, void *src) {}

/* Monopoly stubs */
int fluid_synth_noteon_mono_LOCAL(void *synth, int chan, int key, int vel) { return 1; }
int fluid_synth_noteon_mono_staccato(void *synth, int chan, int key, int vel) { return 1; }
int fluid_synth_noteoff_monopoly(void *synth, int chan) { return 1; }
