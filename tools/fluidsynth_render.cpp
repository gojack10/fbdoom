// fluidsynth_render.c — Offline MUS→MIDI→FluidSynth render to WAV
// Proves FluidSynth produces audible output on this MUS track.
// Compiles against our embedded FluidSynth (Mac build).
//
// gcc -O2 -o fluidsynth_render fluidsynth_render.c \
//     src/i_mus_convert.cpp src/mus2midi.cpp \
//     $(find build/fluidsynth -name "*.o") \
//     -I src -I fluidsynth/include -I fluidsynth/src -I fluidsynth/src/utils \
//     -I fluidsynth/src/sfloader -I fluidsynth/src/rvoice -I fluidsynth/src/synth \
//     -I fluidsynth/src/midi -I fluidsynth/src/drivers -I fluidsynth/src/bindings \
//     -I fluidsynth/src/gentables \
//     -DWITH_FLOAT=1 -DOSAL_cpp11=1 -std=c++03 -fno-exceptions -lm -lc++

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "fluidsynth.h"

extern "C" int convertToMidi(void *musData, void **midiOutput);

// MUSHeader (from mus2midi.h)
typedef struct {
    uint32_t Magic;
    uint16_t SongLen;
    uint16_t SongStart;
    uint16_t NumChans;
    uint16_t NumSecondaryChans;
    uint16_t NumInstruments;
    uint16_t Pad;
} MUSHeader;

typedef enum {
    MIDI_NOTE_ON       = 0x90,
    MIDI_NOTE_OFF      = 0x80,
    MIDI_PROGRAM_CHANGE= 0xC0,
    MIDI_CTRL_CHANGE   = 0xB0,
    MIDI_PITCH_BEND    = 0xE0,
    MIDI_TEMPO         = 0xFF,
} midi_type_t;

typedef struct {
    int tick, type, channel, param1, param2;
} midi_event_t;

#define MAX_EVENTS 8192
#define OUT_RATE   44100
#define MAX_TICK_SAMPLES 2000

static int read_var_len(const uint8_t *buf, int pos, int *val) {
    int v = 0;
    do { buf[pos++]; } while (buf[pos++] & 0x80); // simplified
    return pos;
}

static int parse_midi(const uint8_t *data, int datalen, midi_event_t *events, int max_events) {
    int tick = 0, ne = 0, pos = 0, status = 0;
    while (pos < datalen && ne < max_events) {
        int delta = 0, consumed = 0;
        unsigned char b;
        do { b = data[pos++]; delta = (delta << 7) | (b & 0x7F); consumed++; } while (b & 0x80);
        tick += delta;

        int byte = data[pos++];
        if (byte < 0x80) { pos--; if (status == 0) break; goto use_running; }
        status = byte;

    use_running:
        int type = status & 0xF0;
        int chan = status & 0x0F;

        switch (type) {
            case 0x80: case 0x90:
                if (pos + 2 > datalen || ne >= max_events) goto done;
                { int note = data[pos], vel = data[pos+1]; pos += 2;
                  events[ne].tick = tick; events[ne].channel = chan;
                  events[ne].param1 = note; events[ne].param2 = vel;
                  events[ne].type = (type == 0x80 || vel == 0) ? MIDI_NOTE_OFF : MIDI_NOTE_ON;
                  ne++; }
                break;
            case 0xC0:
                if (pos + 1 > datalen || ne >= max_events) goto done;
                events[ne].tick = tick; events[ne].type = MIDI_PROGRAM_CHANGE;
                events[ne].channel = chan; events[ne].param1 = data[pos++]; events[ne].param2 = 0;
                ne++; break;
            case 0xB0:
                if (pos + 2 > datalen) goto done;
                if (ne < max_events) {
                    events[ne].tick = tick; events[ne].type = MIDI_CTRL_CHANGE;
                    events[ne].channel = chan; events[ne].param1 = data[pos];
                    events[ne].param2 = data[pos+1]; pos += 2; ne++;
                } else pos += 2;
                break;
            case 0xE0:
                if (pos + 2 > datalen) goto done;
                { int p1 = data[pos], p2 = data[pos+1]; pos += 2;
                  if (ne < max_events) { events[ne].tick = tick; events[ne].type = MIDI_PITCH_BEND;
                    events[ne].channel = chan; events[ne].param1 = p1; events[ne].param2 = p2; ne++; }
                } break;
            case 0xF0:
                if (status == 0xFF) {
                    if (pos + 1 > datalen) goto done;
                    int meta_type = data[pos++];
                    int meta_len = 0, ml_pos = pos;
                    unsigned char mb;
                    do { mb = data[pos++]; meta_len = (meta_len << 7) | (mb & 0x7F); } while (mb & 0x80);
                    if (meta_type == 0x51 && meta_len == 3 && ne < max_events) {
                        int tempo_us = (data[pos] << 16) | (data[pos+1] << 8) | data[pos+2];
                        events[ne].tick = tick; events[ne].type = MIDI_TEMPO;
                        events[ne].channel = 0; events[ne].param1 = tempo_us; events[ne].param2 = 0;
                        ne++;
                    } else if (meta_type == 0x2F) { goto done; }
                    pos += meta_len;
                } else {
                    int skip = 0, sp = pos; unsigned char sb;
                    do { sb = data[pos++]; skip = (skip << 7) | (sb & 0x7F); } while (sb & 0x80);
                    pos += skip;
                }
                break;
            default:
                if (pos + 2 <= datalen) pos += 2; else goto done;
                break;
        }
    }
done:
    return ne;
}

int main(int argc, char **argv) {
    const char *mus_path = "/tmp/e1m2.mus";
    const char *sf2_path = "/Users/jack/Downloads/SC-55.SF2";
    const char *wav_path = "/tmp/e1m2_fluidsynth.wav";

    if (argc > 1) mus_path = argv[1];
    if (argc > 2) sf2_path = argv[2];

    // Load MUS
    printf("Opening MUS: %s\n", mus_path);
    FILE *mf = fopen(mus_path, "rb");
    if (!mf) { perror("MUS open"); return 1; }
    fseek(mf, 0, SEEK_END); int msize = ftell(mf); fseek(mf, 0, SEEK_SET);
    uint8_t *mus = (uint8_t *)malloc(msize);
    if (!mus) { fprintf(stderr, "MUS malloc failed\n"); return 1; }
    fread(mus, 1, msize, mf); fclose(mf);
    printf("MUS: %d bytes\n", msize);

    // MUS→MIDI
    printf("Converting MUS→MIDI...\n");
    void *midi_buf = NULL;
    if (!convertToMidi(mus, &midi_buf)) { printf("convertToMidi FAILED\n"); free(mus); return 1; }
    free(mus);

    uint8_t *m = (uint8_t *)midi_buf;
    int midi_len = (m[18] << 24) | (m[19] << 16) | (m[20] << 8) | m[21];
    int divisions = (m[12] << 8) | m[13];
    uint8_t *track_data = &m[22];
    printf("MIDI: %d bytes, divisions=%d\n", midi_len, divisions);

    // Parse MIDI events
    printf("Parsing MIDI...\n");
    midi_event_t events[MAX_EVENTS];
    int num_events = parse_midi(track_data, midi_len, events, MAX_EVENTS);
    free(midi_buf);
    printf("Events: %d\n", num_events);

    if (num_events == 0) { printf("NO EVENTS PARSED\n"); return 1; }

    int last_tick = 0;
    for (int i = 0; i < num_events; i++) {
        if (events[i].tick > last_tick) last_tick = events[i].tick;
    }
    printf("Last tick: %d\n", last_tick);

    // Count events by type
    int note_on_count = 0, note_off_count = 0, pgm_count = 0, cc_count = 0;
    for (int i = 0; i < num_events; i++) {
        switch (events[i].type) {
            case MIDI_NOTE_ON: note_on_count++; break;
            case MIDI_NOTE_OFF: note_off_count++; break;
            case MIDI_PROGRAM_CHANGE: pgm_count++; break;
            case MIDI_CTRL_CHANGE: cc_count++; break;
        }
    }
    printf("NOTEON:%d NOTEOFF:%d PGM:%d CC:%d\n", note_on_count, note_off_count, pgm_count, cc_count);

    // Initialize FluidSynth
    printf("Creating fluid_settings...\n");
    fluid_settings_t *settings = new_fluid_settings();
    if (!settings) { printf("fluid_settings FAILED\n"); return 1; }

    fluid_settings_setstr(settings, "audio.driver", "jamoma");
    fluid_settings_setnum(settings, "synth.sample-rate", (double)OUT_RATE);
    fluid_settings_setint(settings, "synth.cpu-cores", 1);

    // Disable reverb and chorus for clean diagnostic output
    fluid_settings_setint(settings, "synth.reverb.active", 0);
    fluid_settings_setint(settings, "synth.chorus.active", 0);

    printf("Creating fluid_synth...\n");
    fluid_synth_t *synth = new_fluid_synth(settings);
    if (!synth) { printf("fluid_synth FAILED\n"); delete_fluid_settings(settings); return 1; }
    delete_fluid_settings(settings);

    printf("Loading SF2: %s\n", sf2_path);
    int sfont_id = fluid_synth_sfload(synth, sf2_path, 1);
    if (sfont_id < 0) { printf("SF2 load FAILED (%s)\n", sf2_path); delete_fluid_synth(synth); return 1; }
    printf("SF2 loaded: id=%d\n", sfont_id);

    // Set gain
    fluid_synth_set_gain(synth, 0.5f);

    // Calculate total samples
    int max_samples = last_tick * MAX_TICK_SAMPLES + OUT_RATE * 5;
    float *buf_l = (float *)calloc(max_samples, sizeof(float));
    float *buf_r = (float *)calloc(max_samples, sizeof(float));
    if (!buf_l || !buf_r) { printf("Buffer malloc FAILED\n"); delete_fluid_synth(synth); return 1; }

    // Render: process MIDI events and generate audio
    float tempo = 500000.0f;
    float tick_frac = 0.0f;
    int cur_tick = 0, cur_event = 0;
    int wp = 0; // write position in samples

    // Count how many ticks we process
    int total_ticks = 0;

    printf("Rendering...\n");

    while (cur_event < num_events && wp < max_samples) {
        // Process all events at current tick
        while (cur_event < num_events && events[cur_event].tick == cur_tick) {
            midi_event_t *ev = &events[cur_event];
            switch (ev->type) {
                case MIDI_NOTE_ON:
                    fluid_synth_noteon(synth, ev->channel, ev->param1, ev->param2);
                    break;
                case MIDI_NOTE_OFF:
                    fluid_synth_noteoff(synth, ev->channel, ev->param1);
                    break;
                case MIDI_TEMPO:
                    tempo = (float)ev->param1;
                    break;
                case MIDI_PROGRAM_CHANGE:
                    fluid_synth_program_select(synth, ev->channel, 0, 0, ev->param1);
                    break;
                case MIDI_CTRL_CHANGE:
                    if (ev->param1 == 7) fluid_synth_cc(synth, ev->channel, 7, ev->param2);
                    break;
            }
            cur_event++;
        }

        // Calculate samples for this tick
        float spb = (tempo / (float)divisions) * ((float)OUT_RATE / 1000000.0f);
        float tick_samples_f = spb + tick_frac;
        int tick_samples = (int)(tick_samples_f + 0.5f);
        tick_frac = tick_samples_f - (float)tick_samples;

        if (tick_samples < 1) tick_samples = 1;
        if (wp + tick_samples > max_samples) tick_samples = max_samples - wp;

        // Render audio
        int rendered = fluid_synth_write_float(synth, tick_samples,
            buf_l + wp, 0, 1,
            buf_r + wp, 0, 1);

        if (rendered <= 0) {
            printf("fluid_synth_write_float returned %d at tick %d\n", rendered, cur_tick);
            // Skip this tick with silence
        } else {
            wp += rendered;
        }

        cur_tick++;
        total_ticks++;

        // End of song handling
        if (cur_event >= num_events) {
            for (int ch = 0; ch < 16; ch++) fluid_synth_all_notes_off(synth, ch);
            break;
        }
    }

    printf("Rendered %d samples (%.1fs), %d ticks\n", wp, (double)wp / OUT_RATE, total_ticks);

    // Analyze output: find max sample values
    float max_abs = 0;
    int max_idx = 0;
    for (int i = 0; i < wp; i++) {
        float a = buf_l[i]; if (a < 0) a = -a;
        float b = buf_r[i]; if (b < 0) b = -b;
        float m = a > b ? a : b;
        if (m > max_abs) { max_abs = m; max_idx = i; }
    }
    printf("Max abs sample: %.6f at sample %d (%.2fs)\n", max_abs, max_idx, (double)max_idx / OUT_RATE);

    // Dump first 100 samples
    printf("First 100 samples (L,R):\n");
    for (int i = 0; i < 100 && i < wp; i++) {
        printf("  [%4d] L=%+.6f R=%+.6f\n", i, buf_l[i], buf_r[i]);
    }

    // Write WAV (16-bit mono, L+R mixed)
    FILE *wf = fopen(wav_path, "wb");
    if (!wf) { perror("WAV open"); free(buf_l); free(buf_r); delete_fluid_synth(synth); return 1; }

    int dsz = wp * 2, wsz = 44 + dsz;
    unsigned char *h = (unsigned char *)malloc(wsz);
    memcpy(h, "RIFF", 4);
    *(uint32_t *)(h + 4) = wsz - 8;
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    *(uint32_t *)(h + 16) = 16;
    *(uint16_t *)(h + 20) = 1; // PCM
    *(uint16_t *)(h + 22) = 1; // mono
    *(uint32_t *)(h + 24) = OUT_RATE;
    *(uint32_t *)(h + 28) = OUT_RATE * 2;
    *(uint16_t *)(h + 32) = 2;
    *(uint16_t *)(h + 34) = 16;
    memcpy(h + 36, "data", 4);
    *(uint32_t *)(h + 40) = dsz;

    for (int i = 0; i < wp; i++) {
        float sample = (buf_l[i] + buf_r[i]) * 0.5f;
        int32_t s = (int32_t)(sample * 32767.0f);
        if (s < -32768) s = -32768;
        if (s > 32767) s = 32767;
        int16_t val = (int16_t)s;
        h[44 + i * 2] = val & 0xFF;
        h[45 + i * 2] = (val >> 8) & 0xFF;
    }

    fwrite(h, 1, wsz, wf);
    fclose(wf);
    free(h);
    free(buf_l);
    free(buf_r);
    delete_fluid_synth(synth);

    printf("Wrote: %s\n", wav_path);
    printf("Run: afplay %s\n", wav_path);
    return 0;
}