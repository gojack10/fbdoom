// sf2_play.c — standalone SF2 playback test via SDL audio
// gcc -o sf2_play sf2_play.c sf2_loader.c -I../src -framework SDL2 -lm -framework Cocoa
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>

#include "../src/device/sf2_loader.h"

#define OUT_RATE 44100
#define BUFFER_SIZE 4096
#define MAX_NOTES 32

typedef struct {
    int note;
    int sample_idx;
    int sample_len;
    float sample_pos;
    float sample_inc;
    int loop_start, loop_end;
    int looping;
    float env_gain;
    float env_step;
    int velocity;
    int inst_class;
    float lp_state;
    int active;
} note_t;

static note_t notes[MAX_NOTES];
static int num_active = 0;
static SDL_AudioSpec spec;
static Uint8 *sdl_buffer;

// ADSR table (same as i_android_music.c)
static const float adsr[14][4] = {
    {0.002f, 0.020f, 0.50f, 0.050f}, {0.001f, 0.030f, 0.30f, 0.060f},
    {0.010f, 0.005f, 0.90f, 0.100f}, {0.003f, 0.015f, 0.70f, 0.040f},
    {0.005f, 0.015f, 0.70f, 0.080f}, {0.020f, 0.010f, 0.80f, 0.060f},
    {0.010f, 0.005f, 0.90f, 0.100f}, {0.015f, 0.010f, 0.80f, 0.060f},
    {0.005f, 0.010f, 0.80f, 0.050f}, {0.030f, 0.010f, 0.90f, 0.100f},
    {0.002f, 0.005f, 0.50f, 0.030f}, {0.005f, 0.010f, 0.70f, 0.050f},
    {0.001f, 0.005f, 0.20f, 0.020f}, {0.001f, 0.003f, 0.10f, 0.010f},
};
static const float lp_cut[14] = {
    0.30f, 0.40f, 0.50f, 0.35f, 0.15f, 0.25f, 0.45f, 0.20f,
    0.60f, 0.15f, 0.40f, 0.30f, 0.50f, 0.40f,
};

void note_on(int prog, int midi_note, int velocity) {
    if (num_active >= MAX_NOTES) return;
    int kl, kh;
    int sidx = sf2_get_sample(prog, midi_note, &kl, &kh);
    if (sidx < 0) { printf("note_on: no sample for prog=%d note=%d\n", prog, midi_note); return; }

    int16_t *data = sf2_get_sample_data(sidx);
    if (!data) { printf("note_on: no data for sample %d\n", sidx); return; }

    note_t *n = &notes[num_active++];
    sf2_sample_t *s = &sf2_samples[sidx];
    n->note = midi_note;
    n->sample_idx = sidx;
    n->sample_len = s->end - s->start;  // shdr values are sample counts
    n->sample_pos = 0.0f;
    // Pitch: sample_inc relative to original playback speed
    // orig_key=1 for SC-55 samples. At midi_note==orig_key, inc=1.0 (original pitch).
    float semitones = (midi_note - s->orig_key) / 12.0f;
    n->sample_inc = powf(2.0f, semitones);
    n->loop_start = s->loop_start - s->start;  // relative to sample start (sample counts)
    n->loop_end = s->loop_end - s->start;  // relative to sample start (sample counts)
    n->looping = (n->loop_end > n->loop_start) ? 1 : 0;
    n->env_gain = 0.0f;
    int ic = prog % 14;
    n->inst_class = ic;
    n->env_step = 1.0f / (adsr[ic][0] * OUT_RATE);
    n->velocity = velocity;
    n->lp_state = 0.0f;
    n->active = 1;
}

void note_off(int midi_note) {
    for (int i = 0; i < num_active; i++) {
        if (notes[i].note == midi_note && notes[i].active) {
            note_t *n = &notes[i];
            int ic = n->inst_class;
            n->env_gain = fmaxf(n->env_gain, 0.01f);
            n->env_step = -n->env_gain / (adsr[ic][3] * OUT_RATE);
            // Mark for release by setting env_step negative
            break;
        }
    }
}

static void sdl_callback(void *userdata, Uint8 *stream, int len) {
    int16_t *out = (int16_t *)stream;
    int frames = len / sizeof(int16_t);
    int32_t acc[BUFFER_SIZE * 2];  // accumulation buffer (stack-allocated, safe)

    memset(acc, 0, frames * sizeof(int32_t));
    memset(out, 0, len);

    for (int i = 0; i < num_active; i++) {
        note_t *n = &notes[i];
        if (!n->active) continue;
        int ic = n->inst_class;

        for (int f = 0; f < frames; f++) {
            // Envelope
            n->env_gain += n->env_step;
            if (n->env_step < 0 && n->env_gain <= 0.001f) {
                n->active = 0;
                break;
            }
            if (n->env_gain >= 1.0f) {
                n->env_gain = 1.0f;
                n->env_step = -(1.0f - adsr[ic][2]) / (adsr[ic][1] * OUT_RATE);
            } else if (n->env_step < 0 && n->env_gain <= adsr[ic][2]) {
                n->env_gain = adsr[ic][2];
                n->env_step = 0.0f;
            }

            // Sample playback
            int s = 0;
            if (n->sample_idx >= 0 && n->sample_len > 0) {
                int16_t *data = sf2_get_sample_data(n->sample_idx);
                if (data) {
                    float pos = n->sample_pos;
                    int idx = (int)pos;
                    float frac = pos - (float)idx;

                    if (n->looping && pos >= (float)n->loop_end) {
                        pos = (float)(n->loop_start + (int)pos - n->loop_end);
                        idx = (int)pos;
                        frac = pos - (float)idx;
                    }

                    if (idx >= n->sample_len - 1) {
                        if (n->looping) idx = n->loop_start;
                        else { n->active = 0; break; }
                    }
                    int next = idx + 1;
                    if (next >= n->sample_len) next = n->sample_len - 1;

                    s = (int)(data[idx] + (data[next] - data[idx]) * frac);
                    n->sample_pos += n->sample_inc;
                }
            }

            // LPF + amplitude
            float cutoff = lp_cut[ic];
            float filtered = n->lp_state + cutoff * ((float)s - n->lp_state);
            n->lp_state = filtered;
            float amp = n->env_gain * n->velocity / 127.0f * 0.5f;  // reduced from 2.0 to avoid clipping
            acc[f] += (int)(filtered * amp);
        }
    }

    // Convert int32 -> int16 with soft clip
    for (int f = 0; f < frames; f++) {
        float x = (float)acc[f] * 0.25f;
        float ax = x < 0 ? -x : x;
        if (ax > 32767.0f) {
            float norm = ax / 32767.0f;
            ax = ax / (1.0f + norm * 0.5f);
        }
        out[f] = (int16_t)(x < 0 ? -ax : ax);
    }

    // Compact active notes
    int j = num_active - 1;
    for (int i = 0; i <= j; i++) {
        if (!notes[i].active) {
            if (i != j) notes[i] = notes[j];
            j--; i--;
        }
    }
    num_active = j + 1;
}

// E1M2 "The Possessed" theme
typedef struct { int delay_ms; int prog; int note; int vel; } event_t;

static event_t melody[] = {
    // Fast bass line (Synth Bass 1, prog 38) - driving eighth notes
    {0,   38, 48, 110},  // C3
    {120, 38, 48, 0},
    {0,   38, 51, 110},  // Eb3
    {120, 38, 51, 0},
    {0,   38, 48, 110},
    {120, 38, 48, 0},
    {0,   38, 53, 110},  // E3
    {120, 38, 53, 0},
    // Bass continuation
    {0,   38, 55, 110},  // F3
    {120, 38, 55, 0},
    {0,   38, 55, 110},
    {120, 38, 55, 0},
    {0,   38, 53, 110},
    {120, 38, 53, 0},
    {0,   38, 51, 110},
    {120, 38, 51, 0},
    // Lead melody (Synth Brass 1, prog 62) - iconic E1M2 riff
    {0,   62, 83, 100},  // B4
    {120, 62, 83, 0},
    {0,   62, 81, 100},  // Bb4
    {120, 62, 81, 0},
    {0,   62, 79, 100},  // A4
    {120, 62, 79, 0},
    {0,   62, 81, 100},
    {120, 62, 81, 0},
    // Lead continuation
    {0,   62, 83, 100},
    {120, 62, 83, 0},
    {0,   62, 86, 100},  // D5
    {240, 62, 86, 0},
    {0,   62, 83, 100},
    {240, 62, 83, 0},
    // Pad chord (Syn.Strings1, prog 50)
    {0,   50, 60, 60},   // C4
    {0,   50, 63, 60},   // Eb4
    {0,   50, 67, 60},   // G4
    {480, 50, 60, 0},
    {0,   50, 63, 0},
    {0,   50, 67, 0},
    // Repeat bass
    {200, 38, 48, 110},
    {120, 38, 48, 0},
    {0,   38, 51, 110},
    {120, 38, 51, 0},
    {0,   38, 48, 110},
    {120, 38, 48, 0},
    {0,   38, 53, 110},
    {120, 38, 53, 0},
    // Lead variation
    {0,   62, 79, 100},
    {120, 62, 79, 0},
    {0,   62, 81, 100},
    {120, 62, 81, 0},
    {0,   62, 83, 100},
    {120, 62, 83, 0},
    {0,   62, 86, 100},
    {120, 62, 86, 0},
    {0,   62, 88, 100},  // Eb5
    {240, 62, 88, 0},
    {0,   62, 86, 100},
    {120, 62, 86, 0},
    {0,   62, 83, 100},
    {240, 62, 83, 0},
    // Silence before repeat
    {400, 0, 0, 0},
};
static int melody_len = sizeof(melody) / sizeof(melody[0]);

int main(int argc, char **argv) {
    const char *sf2_path = "/Users/jack/Downloads/SC-55.SF2";
    if (argc > 1) sf2_path = argv[1];

    printf("Loading SF2: %s\n", sf2_path);
    if (sf2_load(sf2_path) != 0) {
        fprintf(stderr, "FAIL: sf2_load\n");
        return 1;
    }
    printf("OK: %d samples loaded\n\n", sf2_num_samples);

    // Initialize SDL audio
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    spec.freq = OUT_RATE;
    spec.format = AUDIO_S16SYS;
    spec.channels = 1;
    spec.silence = 0;
    spec.samples = BUFFER_SIZE;
    spec.callback = sdl_callback;
    spec.userdata = NULL;

    if (SDL_OpenAudio(&spec, NULL) < 0) {
        fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
        return 1;
    }

    printf("Playing SF2 samples via SDL audio (press Ctrl+C to stop)\n");
    printf("Looping melody...\n\n");
    SDL_PauseAudio(0);

    // Schedule melody events
    int loop_count = 0;
    while (loop_count < 20) {  // loop 20 times (~60 seconds)
        for (int i = 0; i < melody_len; i++) {
            event_t *e = &melody[i];
            if (e->delay_ms > 0) SDL_Delay(e->delay_ms);
            if (e->vel > 0) {
                note_on(e->prog, e->note, e->vel);
            } else if (e->note > 0) {
                note_off(e->note);
            }
        }
        loop_count++;
    }

    // Let remaining notes finish
    SDL_Delay(2000);

    SDL_PauseAudio(1);
    SDL_CloseAudio();
    SDL_Quit();
    sf2_free();
    printf("Done.\n");
    return 0;
}