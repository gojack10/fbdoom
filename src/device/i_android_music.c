//
//  i_android_music.c
//  fbdoom — Android music backend (FluidSynth)
//
//  Uses FluidSynth to render DOOM music from MUS lumps.
//  MUS lumps are converted to MIDI via mus2midi, then events
//  are fed to FluidSynth which renders PCM into the accumulation
//  buffer shared with SFX in i_android_sound.c.
//
//  WHY: Custom renderer had hardcoded ADSR/LPF tables that couldn't
//  match SF2 spec processing. FluidSynth + SC-55.SF2 produces
//  identical output to GZDoom (verified by MUS→MIDI→FluidSynth ground truth).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>

#include "i_sound.h"
#include "doomdef.h"
#include "doomtype.h"

// FluidSynth API (embedded, no separate library)
#include <time.h>
#include "fluidsynth.h"

// ---------------------------------------------------------------------------
// MUSHeader definition (from mus2midi.h, duplicated to avoid C++ headers)
// ---------------------------------------------------------------------------

typedef unsigned char BYTE;

typedef struct {
    uint32_t Magic;
    uint16_t SongLen;
    uint16_t SongStart;
    uint16_t NumChans;
    uint16_t NumSecondaryChans;
    uint16_t NumInstruments;
    uint16_t Pad;
} MUSHeader;

// convertToMidi is extern "C" in i_mus_convert.hpp (C++), callable from C.
extern int convertToMidi(void *musData, void **midiOutput);

// SHORT macro from m_swap.h (little-endian, no-op on ARM)
#define SHORT(x) (x)

// ---------------------------------------------------------------------------
// Debug logging
// ---------------------------------------------------------------------------

static FILE *mus_log = NULL;
static void mus_dbg(const char *fmt, ...) {
    if (!mus_log) mus_log = fopen("/data/local/tmp/mus.log", "w");
    if (!mus_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(mus_log, fmt, ap);
    va_end(ap);
    fflush(mus_log);
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MAX_MIDI_EVENTS   8192
#define MAX_SONGS         4
#define SOUNDFONT_PATH    "/data/local/bin/SC-55.SF2"

// ---------------------------------------------------------------------------
// MIDI event types
// ---------------------------------------------------------------------------

typedef enum {
    MIDI_EVENT_NOTE_ON       = 0x90,
    MIDI_EVENT_NOTE_OFF      = 0x80,
    MIDI_EVENT_PROGRAM_CHANGE= 0xC0,
    MIDI_EVENT_CTRL_CHANGE   = 0xB0,
    MIDI_EVENT_PITCH_BEND    = 0xE0,
    MIDI_EVENT_TEMPO         = 0xFF,
    MIDI_EVENT_EOT           = 0xFE,
} midi_event_type_t;

typedef struct {
    int tick;
    int type;
    int channel;
    int param1;
    int param2;
} midi_event_t;

// ---------------------------------------------------------------------------
// Song data
// ---------------------------------------------------------------------------

#define MUSIC_SAMPLE_RATE 44100  // pre-render sample rate (must match mixer OUT_RATE)
#define MUSIC_PATH "/data/local/bin/music"  // directory for pre-rendered PCM files

typedef struct {
    int           used;
    char          name[16];
    midi_event_t *events;
    int           num_events;
    int           num_ticks;
    int           divisions;
    // Pre-rendered PCM (mono int16 at MUSIC_SAMPLE_RATE Hz)
    int16_t      *pcm_data;
    int           pcm_length;
} song_t;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static int          music_inited = 0;
static song_t       songs[MAX_SONGS];

// FluidSynth state
static fluid_synth_t      *synth       = NULL;
static fluid_settings_t   *settings    = NULL;
static int                sfont_id     = 0;  // SoundFont ID for program selection

// Playback state
static song_t      *playing      = NULL;
static int          looping      = 0;
static int          music_paused = 0;
static int          cur_tick     = 0;
static int          cur_event    = 0;
static double       pcm_pos      = 0.0;  // fractional sample position for PCM playback
static float        cur_tempo    = 500000.0f;
static float        tick_frac    = 0.0f;
static int          music_volume = 127;

// Timing diagnostics (logged every ~1 second at 44100 Hz)
static double       music_start_time = 0.0;  // monotonic clock when song started
static long long    music_diag_samples = 0;  // samples since last diagnostic log

// Per-channel program selection
static int          channel_programs[16];
// Per-channel CC7 volume
static float        channel_volume[16];

// ---------------------------------------------------------------------------
// MIDI parsing helpers (same as before, proven correct)
// ---------------------------------------------------------------------------

static int read16be(const unsigned char *p) {
    return (int)((unsigned int)p[0] << 8) | p[1];
}

static int read32be(const unsigned char *p) {
    return ((int)p[0] << 24) | ((int)p[1] << 16) | ((int)p[2] << 8) | p[3];
}

static int read_var_len(const unsigned char *buf, int *val) {
    int v = 0, ofs = 0;
    unsigned char b;
    do { b = buf[ofs++]; v = (v << 7) | (b & 0x7F); } while (b & 0x80);
    *val = v;
    return ofs;
}

static int parse_midi_events(const unsigned char *data, int datalen,
                             int divisions, midi_event_t *events, int max_events)
{
    int tick = 0, ne = 0, pos = 0, status = 0;

    while (pos < datalen && ne < max_events) {
        int delta = 0;
        int consumed = read_var_len(&data[pos], &delta);
        pos += consumed;
        tick += delta;

        int byte = data[pos++];
        if (byte < 0x80) {
            pos--;
            if (status == 0) break;
            goto use_running;
        }
        status = byte;

use_running:
        int type = status & 0xF0;
        int chan = status & 0x0F;

        switch (type) {
            case 0x80: case 0x90:
                if (pos + 2 > datalen) goto done;
                { int note = data[pos], vel = data[pos+1]; pos += 2;
                  events[ne].tick = tick; events[ne].channel = chan;
                  events[ne].param1 = note; events[ne].param2 = vel;
                  events[ne].type = (type == 0x80 || vel == 0) ? MIDI_EVENT_NOTE_OFF : MIDI_EVENT_NOTE_ON;
                  ne++; }
                break;
            case 0xC0:
                if (pos + 1 > datalen) goto done;
                events[ne].tick = tick; events[ne].type = MIDI_EVENT_PROGRAM_CHANGE;
                events[ne].channel = chan; events[ne].param1 = data[pos++]; events[ne].param2 = 0;
                ne++;
                break;
            case 0xB0:
                if (pos + 2 > datalen) goto done;
                events[ne].tick = tick; events[ne].type = MIDI_EVENT_CTRL_CHANGE;
                events[ne].channel = chan; events[ne].param1 = data[pos];
                events[ne].param2 = data[pos+1]; pos += 2; ne++;
                break;
            case 0xE0:
                if (pos + 2 > datalen) goto done;
                events[ne].tick = tick; events[ne].type = MIDI_EVENT_PITCH_BEND;
                events[ne].channel = chan; events[ne].param1 = data[pos];
                events[ne].param2 = data[pos+1]; pos += 2; ne++;
                break;
            case 0xF0:
                if (status == 0xFF) {
                    if (pos + 1 > datalen) goto done;
                    int meta_type = data[pos++];
                    int meta_len = 0;
                    pos += read_var_len(&data[pos], &meta_len);
                    if (pos + meta_len > datalen) goto done;
                    if (meta_type == 0x51 && meta_len == 3) {
                        int tempo_us = (data[pos] << 16) | (data[pos+1] << 8) | data[pos+2];
                        events[ne].tick = tick; events[ne].type = MIDI_EVENT_TEMPO;
                        events[ne].channel = 0; events[ne].param1 = tempo_us; events[ne].param2 = 0;
                        ne++;
                    } else if (meta_type == 0x2F) {
                        goto done;
                    }
                    pos += meta_len;
                } else {
                    int skip = 0;
                    pos += read_var_len(&data[pos], &skip);
                    pos += skip;
                }
                break;
            default:
                pos += 2;
                break;
        }
    }
done:
    return ne;
}

// ---------------------------------------------------------------------------
// MUS→MIDI song registration (same as before, proven correct)
// ---------------------------------------------------------------------------

static int16_t *pre_render_song(song_t *s, fluid_synth_t *synth_g);

static int register_mus_song(song_t *s, void *mus_data, int mus_len, const char *name)
{
    // Free previous allocations if re-registering same slot
    if (s->events) { free(s->events); s->events = NULL; }
    if (s->pcm_data) { free(s->pcm_data); s->pcm_data = NULL; s->pcm_length = 0; }

    void *midi_buf = NULL;
    if (!convertToMidi(mus_data, &midi_buf) || !midi_buf) {
        mus_dbg("register: convertToMidi failed for %s\n", name);
        return 0;
    }

    unsigned char *m = (unsigned char *)midi_buf;
    int midi_len    = read32be(&m[18]);
    int divisions   = read16be(&m[12]);
    unsigned char *track_data = &m[22];

    s->events = (midi_event_t *)calloc(MAX_MIDI_EVENTS, sizeof(midi_event_t));
    if (!s->events) { free(midi_buf); return 0; }

    s->num_events = parse_midi_events(track_data, midi_len, divisions,
                                      s->events, MAX_MIDI_EVENTS);
    s->num_ticks  = (s->num_events > 0) ? s->events[s->num_events - 1].tick : 0;
    s->divisions  = divisions;

    strncpy(s->name, name ? name : "?", 15);
    s->name[15] = '\0';
    s->used = 1;

    mus_dbg("register: %s events=%d ticks=%d div=%d\n",
            s->name, s->num_events, s->num_ticks, s->divisions);

    // Load pre-rendered PCM from disk (offline pre-render strategy)
    // WHY: eliminates on-device FluidSynth rendering (2-3 min/song on Cortex-A8)
    // PCM files rendered at build time on desktop: 44100Hz mono int16
    {
        char pcm_path[128];
        snprintf(pcm_path, sizeof(pcm_path), "%s/D_%s.pcm", MUSIC_PATH, name);
        // Convert song name to uppercase for file lookup (skip path + "D_", stop at ".")
        for (char *p = pcm_path + strlen(MUSIC_PATH) + 3; *p && *p != '.'; p++) {
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        
        FILE *f = fopen(pcm_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long file_size = ftell(f);
            fseek(f, 0, SEEK_SET);
            int16_t *pcm = (int16_t *)malloc(file_size);
            if (pcm) {
                fread(pcm, 1, file_size, f);
                s->pcm_data = pcm;
                s->pcm_length = file_size / 2;  // int16 samples
                mus_dbg("loadpcm: %s from %s (%d samples, %.1f sec)\n",
                        s->name, pcm_path, s->pcm_length,
                        (double)s->pcm_length / MUSIC_SAMPLE_RATE);
            } else {
                mus_dbg("loadpcm: %s malloc failed (%ld bytes)\n", name, file_size);
                s->pcm_data = NULL;
                s->pcm_length = 0;
            }
            fclose(f);
        } else {
            mus_dbg("loadpcm: %s not found\n", pcm_path);
            s->pcm_data = NULL;
            s->pcm_length = 0;
        }
    }

    free(midi_buf);
    return 1;
}

// ---------------------------------------------------------------------------
// Pre-render + PCM playback (replaces real-time FluidSynth DSP)
// ---------------------------------------------------------------------------

// FluidSynth render buffers (static to avoid malloc in render path)
#define MUSIC_RENDER_BUF_SIZE 4096
static float pr_buf_l[MUSIC_RENDER_BUF_SIZE];
static float pr_buf_r[MUSIC_RENDER_BUF_SIZE];

// Pre-render entire song to PCM at load time (synchronous, blocks caller)
static int16_t *pre_render_song(song_t *s, fluid_synth_t *synth_g) {
    if (!s->num_events || !synth_g) return NULL;

    int est_samples = s->num_ticks * 150;
    int16_t *pcm = (int16_t *)malloc(est_samples * sizeof(int16_t));
    if (!pcm) return NULL;

    int total_written = 0;
    int cur_ev = 0;
    int cur_tk = 0;
    float tempo = 500000.0f;
    float tick_frac = 0.0f;

    // Reset synth state for this song
    for (int ch = 0; ch < 16; ch++) {
        channel_volume[ch] = 1.0f;
        channel_programs[ch] = 0;
        fluid_synth_cc(synth_g, ch, 7, 127);
    }

    // Render all ticks (continue even after last event for trailing notes)
    while (cur_tk < s->num_ticks) {
        float spb = (tempo / (float)s->divisions) * ((float)MUSIC_SAMPLE_RATE / 1000000.0f);

        // Process MIDI events at current tick
        while (cur_ev < s->num_events && s->events[cur_ev].tick == cur_tk) {
            midi_event_t *ev = &s->events[cur_ev];
            switch (ev->type) {
                case MIDI_EVENT_NOTE_ON:
                    fluid_synth_noteon(synth_g, ev->channel, ev->param1, ev->param2);
                    break;
                case MIDI_EVENT_NOTE_OFF:
                    fluid_synth_noteoff(synth_g, ev->channel, ev->param1);
                    break;
                case MIDI_EVENT_TEMPO:
                    tempo = (float)ev->param1;
                    spb = (tempo / (float)s->divisions) * ((float)MUSIC_SAMPLE_RATE / 1000000.0f);
                    break;
                case MIDI_EVENT_PROGRAM_CHANGE:
                    fluid_synth_program_change(synth_g, ev->channel, ev->param1);
                    break;
                case MIDI_EVENT_CTRL_CHANGE:
                    if (ev->param1 == 7 && ev->channel < 16) {
                        channel_volume[ev->channel] = (float)ev->param2 / 127.0f;
                        fluid_synth_cc(synth_g, ev->channel, 7, ev->param2);
                    }
                    break;
                default:
                    break;
            }
            cur_ev++;
        }

        // Calculate samples for this tick
        float tick_samples_f = spb + tick_frac;
        int tick_samples = (int)(tick_samples_f + 0.5f);
        tick_frac = tick_samples_f - (float)tick_samples;
        if (tick_frac >= 1.0f) { tick_frac -= 1.0f; tick_samples++; }
        if (tick_samples < 1) tick_samples = 1;

        // Grow buffer if needed
        if (total_written + tick_samples > est_samples) {
            est_samples = (total_written + tick_samples) * 2;
            int16_t *new_pcm = (int16_t *)realloc(pcm, est_samples * sizeof(int16_t));
            if (!new_pcm) { free(pcm); return NULL; }
            pcm = new_pcm;
        }

        // Render through FluidSynth (may need multiple passes for large batches)
        int remaining = tick_samples;
        int write_pos = 0;
        while (remaining > 0) {
            int buf_size = remaining < MUSIC_RENDER_BUF_SIZE ? remaining : MUSIC_RENDER_BUF_SIZE;
            fluid_synth_write_float(synth_g, buf_size, pr_buf_l, 0, 1, pr_buf_r, 0, 1);

            for (int i = 0; i < buf_size; i++) {
                float sample = (pr_buf_l[i] + pr_buf_r[i]) * 0.5f;
                if (sample > 1.0f) sample = 1.0f;
                if (sample < -1.0f) sample = -1.0f;
                pcm[total_written + write_pos + i] = (int16_t)(sample * 32767.0f);
            }
            write_pos += buf_size;
            remaining -= buf_size;
        }
        total_written += tick_samples;
        cur_tk++;
    }

    // All notes off
    for (int ch = 0; ch < 16; ch++) fluid_synth_all_notes_off(synth_g, ch);

    s->pcm_length = total_written;
    return pcm;
}

// Playback: read from pre-rendered PCM buffer (zero FluidSynth overhead)
void music_render(int32_t *acc, int num_samples, int sample_rate) {
    if (!playing || !playing->pcm_data || music_paused) return;
    if (sample_rate != MUSIC_SAMPLE_RATE) return; // pre-rendered at MUSIC_SAMPLE_RATE, no resampling

    int len = playing->pcm_length;
    if (len < 1) return;

    float vol_factor = (float)music_volume / 127.0f;

    for (int i = 0; i < num_samples; i++) {
        if (pcm_pos >= len) {
            if (looping) {
                pcm_pos = 0.0;
            } else {
                playing = NULL;
                return;
            }
        }

        // Linear interpolation between nearest samples
        int idx = (int)pcm_pos;
        double frac = pcm_pos - idx;
        int idx_next = (idx + 1 < len) ? idx + 1 : idx;

        int16_t s0 = playing->pcm_data[idx];
        int16_t s1 = playing->pcm_data[idx_next];
        float sample = ((float)(s1 - s0) * frac + (float)s0) / 32767.0f * vol_factor;

        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        acc[i] += (int)(sample * 32768.0f);

        pcm_pos += 1.0;
    }

    // Timing diagnostic: log every ~1 second (44100 samples)
    music_diag_samples += num_samples;
    if (music_diag_samples >= 44100) {
        music_diag_samples -= 44100;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        double elapsed = now - music_start_time;
        double expected = pcm_pos / (double)sample_rate;
        double drift_ms = (elapsed - expected) * 1000.0;
        mus_dbg("TIMING: %.1fs elapsed=%.3fs expected=%.3fs drift=%+.1fms pos=%.0f/%d loop=%d\n",
                elapsed, elapsed, expected, drift_ms, pcm_pos, len, looping);
    }
}

// ---------------------------------------------------------------------------
// Public API — implements i_sound.h music section
// ---------------------------------------------------------------------------

void I_InitMusic(void) {
    if (music_inited) return;

    settings = new_fluid_settings();
    if (!settings) {
        mus_dbg("INIT: failed to create fluid_settings\n");
        return;
    }

    // Configure for embedded use (no audio/MIDI drivers)
    fluid_settings_setstr(settings, "audio.driver", "null");
    fluid_settings_setint(settings, "synth.sample-rate", MUSIC_SAMPLE_RATE);
    fluid_settings_setint(settings, "synth.cpu-cores", 1);

    synth = new_fluid_synth(settings);
    if (!synth) {
        mus_dbg("INIT: failed to create fluid_synth\n");
        delete_fluid_settings(settings);
        settings = NULL;
        return;
    }

    // FIX: set FluidSynth gain to 0.5 (default is 0.2 which is too quiet)
    fluid_synth_set_gain(synth, 0.5f);

    // PERFORMANCE: disable reverb — DOOM music doesn't need it, saves DSP per sample
    fluid_synth_reverb_on(synth, 0, 0);
    // PERFORMANCE: disable chorus — detuned voice copies + LFO = pure overhead for DOOM
    fluid_settings_setint(settings, "synth.chorus", 0);
    // PERFORMANCE: cap polyphony at 32 — E1M1 uses ~25-29 voices, default 256 is wasteful
    fluid_synth_set_polyphony(synth, 32);

    // Diagnostic: check audio channel count
    mus_dbg("synth: audio_chans=%d midi_chans=%d polyphony=%d\n",
        fluid_synth_count_audio_channels(synth),
        fluid_synth_count_midi_channels(synth),
        fluid_synth_get_polyphony(synth));

    // Load SoundFont
    sfont_id = fluid_synth_sfload(synth, SOUNDFONT_PATH, 1);
    if (sfont_id < 0) {
        mus_dbg("INIT: failed to load %s\n", SOUNDFONT_PATH);
    } else {
        mus_dbg("INIT: loaded %s (id=%d)\n", SOUNDFONT_PATH, sfont_id);
    }

    // Initialize channels
    for (int i = 0; i < 16; i++) {
        channel_programs[i] = 0;
        channel_volume[i] = 1.0f;
        fluid_synth_cc(synth, i, 7, 127);
    }

    memset(songs, 0, sizeof(songs));
    playing = NULL;
    music_inited = 1;
    mus_dbg("INIT: FluidSynth ready, gain=%.2f\n", fluid_synth_get_gain(synth));
}

void I_ShutdownMusic(void) {
    if (!music_inited) return;

    if (synth) { delete_fluid_synth(synth); synth = NULL; }
    if (settings) { delete_fluid_settings(settings); settings = NULL; }

    for (int i = 0; i < MAX_SONGS; i++) {
        if (songs[i].used && songs[i].events) {
            free(songs[i].events);
            songs[i].events = NULL;
        }
        if (songs[i].pcm_data) {
            free(songs[i].pcm_data);
            songs[i].pcm_data = NULL;
        }
        songs[i].used = 0;
    }
    playing = NULL;
    music_inited = 0;
    mus_dbg("SHUTDOWN\n");
}

void I_SetMusicVolume(int volume) {
    if (volume < 0)   volume = 0;
    if (volume > 127) volume = 127;
    music_volume = volume;
    mus_dbg("volume=%d\n", volume);
}

void I_PauseSong(int handle) {
    (void)handle;
    music_paused = 1;
}

void I_ResumeSong(int handle) {
    (void)handle;
    music_paused = 0;
}

int I_RegisterSong(void *data, const char *name) {
    if (!music_inited || !data) return 0;

    int slot = -1;
    for (int i = 0; i < MAX_SONGS; i++) {
        if (!songs[i].used) { slot = i; break; }
    }
    if (slot < 0) return 0;

    MUSHeader *hdr = (MUSHeader *)data;
    int mus_len = SHORT(hdr->SongStart) + SHORT(hdr->SongLen);

    if (!register_mus_song(&songs[slot], data, mus_len, name)) {
        songs[slot].used = 0;
        return 0;
    }

    mus_dbg("register: slot=%d name=%s handle=%d\n", slot, songs[slot].name, slot + 1);
    return slot + 1;
}

void I_PlaySong(int handle, int looping_flag) {
    if (!music_inited || !synth) return;

    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_SONGS || !songs[slot].used) return;

    // Reset all channels
    for (int ch = 0; ch < 16; ch++) fluid_synth_all_notes_off(synth, ch);

    playing = &songs[slot];
    looping = looping_flag;
    pcm_pos = 0.0;
    cur_tick  = 0;
    cur_event = 0;
    cur_tempo = 500000.0f;
    tick_frac = 0.0f;
    music_paused = 0;

    // Timing diagnostics: record start time
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    music_start_time = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    music_diag_samples = 0;
    memset(channel_programs, 0, sizeof(channel_programs));
    for (int i = 0; i < 16; i++) {
        channel_volume[i] = 1.0f;
        fluid_synth_cc(synth, i, 7, 127);
    }

    // If first event is tempo, use it
    if (songs[slot].num_events > 0 &&
        songs[slot].events[0].type == MIDI_EVENT_TEMPO) {
        cur_tempo = (float)songs[slot].events[0].param1;
    }

    mus_dbg("play: %s loop=%d tempo=%.0f\n",
            songs[slot].name, looping, cur_tempo);
}

void I_StopSong(int handle) {
    (void)handle;
    if (!music_inited || !synth) return;

    for (int ch = 0; ch < 16; ch++) fluid_synth_all_notes_off(synth, ch);
    playing = NULL;
    mus_dbg("stop\n");
}

void I_UnRegisterSong(int handle) {
    if (!music_inited) return;

    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_SONGS || !songs[slot].used) return;

    if (playing == &songs[slot]) {
        for (int ch = 0; ch < 16; ch++) fluid_synth_all_notes_off(synth, ch);
        playing = NULL;
    }

    if (songs[slot].events) {
        free(songs[slot].events);
        songs[slot].events = NULL;
    }
    songs[slot].used = 0;
}