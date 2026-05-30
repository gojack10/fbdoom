#ifndef SF2_LOADER_H
#define SF2_LOADER_H

#include <stdint.h>

// SC-55 SoundFont runtime loader
// Parses SF2 file headers at startup, provides sample data for rendering

typedef struct {
    int32_t start;       // sample start index (in samples)
    int32_t end;         // sample end index
    int32_t loop_start;  // loop start index
    int32_t loop_end;    // loop end index
    int32_t sample_rate; // original sample rate (Hz)
    int32_t orig_key;    // original MIDI key
    int16_t *data;       // loaded PCM data (NULL if not loaded)
} sf2_sample_t;

typedef struct {
    int sample_idx; // index into sf2_samples
    int key_lo;     // low key for this zone
    int key_hi;     // high key for this zone
} sf2_zone_t;

typedef struct {
    sf2_zone_t *zones;
    int num_zones;
} sf2_program_t;

// Global state
extern sf2_sample_t *sf2_samples;
extern sf2_program_t sf2_programs[128];
extern int sf2_num_samples;

// Load SF2 from file path. Returns 0 on success.
int sf2_load(const char *path);

// Get sample data for a program + note. Returns sample index or -1.
// Sets key_lo/key_hi to the matching zone's key range.
int sf2_get_sample(int program, int note, int *key_lo, int *key_hi);

// Ensure sample data is loaded into memory
int16_t *sf2_get_sample_data(int sample_idx);

// Free all loaded samples
void sf2_free(void);

#endif // SF2_LOADER_H
