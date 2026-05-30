//
//  i_android_sound.c
//  fbdoom — Android framebuffer sound backend
//
//  Bridges doom's sound API to libaudiowrapper.so via dlopen.
//  libaudiowrapper.so wraps android::AudioTrack from libmedia.so.
//
//  Sound format: DOOM WAD lumps are 8-bit unsigned PCM at ~11025 Hz
//  with an 8-byte header (4-byte rate + 4-byte length).
//  AudioTrack outputs 16-bit signed PCM at 22050 Hz.
//  Mixer thread handles 8→16-bit conversion, 11025→22050 resampling,
//  and multi-channel mixing.
//
//  WHY: AudioTrack via libmedia.so is the only working audio path on
//  Samsung Fascinate (ALSA shim rejects ioctls, TinyALSA ABI mismatch,
//  OpenSL is a 9KB stub). Wrapper .so avoids C++ name mangling from
//  the C doom binary. Never dlclose the wrapper or delete AudioTrack
//  (NDK libstdc++ allocator ≠ bionic heap → SIGSEGV).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>

#include "i_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

// Music backend integration
extern void music_render(int32_t *acc, int num_samples, int sample_rate);
extern void I_InitMusic(void);
extern void I_ShutdownMusic(void);

// Debug log file
static FILE *snd_log = NULL;
static void snd_dbg(const char *fmt, ...) {
    if (!snd_log) snd_log = fopen("/data/local/tmp/snd.log", "w");
    if (!snd_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(snd_log, fmt, ap);
    va_end(ap);
    fflush(snd_log);
}

// Diagnostic counters (logged periodically)
static int mixer_enobufs_count = 0;
static int mixer_partial_count = 0;

// ---------------------------------------------------------------------------
// Audio wrapper function pointers (from libaudiowrapper.so)
// ---------------------------------------------------------------------------

typedef int  (*audio_init_fn)(int sample_rate, int bits_per_sample);
typedef int  (*audio_write_fn)(const void *buffer, unsigned int size);
typedef void (*audio_stop_fn)(void);
typedef void (*audio_shutdown_fn)(void);

static void           *lib_handle    = NULL;
static audio_init_fn   fn_init       = NULL;
static audio_write_fn  fn_write      = NULL;
static audio_stop_fn   fn_stop       = NULL;
static audio_shutdown_fn fn_shutdown = NULL;

// ---------------------------------------------------------------------------
// Channel pool — backend's own channel management
// ---------------------------------------------------------------------------

#define MAX_CHANNELS 32

typedef struct {
    unsigned char  *data;    // 8-bit unsigned PCM (after 8-byte header)
    int             length;  // sample count
    int             pos;     // current playback position (fractional, <<16)
    int             vol;     // 0-127
    int             pitch;   // 128 = normal
    int             playing;
} chan_t;

static chan_t channels[MAX_CHANNELS];
static int    initialized   = 0;

// Global SFX volume (0-127). Applied in mixer thread for real-time slider response.
static int    sfx_volume    = 127;

// ---------------------------------------------------------------------------
// Mixer thread
// ---------------------------------------------------------------------------

static int     mixer_running = 0;
static pthread_t mixer_tid;
static int     mixer_write_count = 0;

// Audio output params (44100 Hz matches the proven test_audiowrapper config)
#define OUT_RATE 44100
#define OUT_BITS 16

// Per-iteration batch — small enough to keep latency low, large enough
// to keep the AudioTrack buffer fed. AudioTrack's 4096-byte buffer
// absorbs timing jitter when we write faster than real-time.
#define MIXER_BATCH 512

static void *mixer_thread(void *arg)
{
    // Use int32 accumulation buffer to sum multiple channels without clipping
    int32_t acc[MIXER_BATCH];
    int16_t out[MIXER_BATCH];
    int first_log = 1;
    int32_t dc_acc = 0;

    // No sleep between batches — AudioTrack's buffer absorbs timing jitter.
    // The mixer renders and writes as fast as possible; AudioTrack drains
    // at 44100 Hz. Writing faster than real-time keeps the buffer filled.

    while (mixer_running) {
        // Zero accumulation buffer
        int i;
        for (i = 0; i < MIXER_BATCH; i++)
            acc[i] = 0;

        int max_wrote = 0;
        int active = 0;

        int c;
        for (c = 0; c < MAX_CHANNELS; c++) {
            chan_t *ch = &channels[c];
            if (!ch->playing || !ch->data || ch->length <= 0)
                continue;
            active++;

            // Resampling ratio: source (11025 Hz) → output (44100 Hz), pitch-adjusted
            float ratio = (11025.0f / (float)OUT_RATE) * ((float)ch->pitch / 128.0f);
            int   phase = ch->pos;
            int   vol   = ch->vol;
            int   ch_wrote = 0;

            for (i = 0; i < MIXER_BATCH; i++) {
                int idx = phase >> 16;
                if (idx >= ch->length) {
                    ch->playing = 0;
                    break;
                }

                // Linear interpolation between adjacent 8-bit samples
                int frac   = phase & 0xFFFF;
                int s0     = ch->data[idx];
                int s1     = (idx + 1 < ch->length) ? ch->data[idx + 1] : s0;
                int sample = s0 + ((s1 - s0) * frac >> 16);

                // 8-bit unsigned (0-255) → 16-bit signed, apply volume
                // Gain: vol * 256 / 127 gives correct 8→16 bit scaling at max vol.
                // int32 accumulator + soft-clipper handle multi-channel summing.
                sample = ((sample - 128) * vol * 256) / 127;

                // ACCUMULATE into int32 buffer (allows multiple channels to sum)
                acc[i] += sample;
                ch_wrote++;

                phase += (int)(ratio * 65536.0f);
            }

            ch->pos = phase;
            if (!ch->playing) {
                ch->pos = 0;
            }
            if (ch_wrote > max_wrote)
                max_wrote = ch_wrote;
        }

        if (fn_write) {
            // Apply global SFX volume to accumulated SFX samples
            if (sfx_volume < 127) {
                float sfx_vol = (float)sfx_volume / 127.0f;
                for (i = 0; i < MIXER_BATCH; i++) {
                    acc[i] = (int32_t)((float)acc[i] * sfx_vol);
                }
            }

            // Mix music into accumulation buffer (always, even during silent SFX)
            music_render(acc, MIXER_BATCH, OUT_RATE);

            // Use max_wrote if SFX channels active, otherwise MIXER_BATCH
            int to_write = (max_wrote > 0) ? max_wrote : MIXER_BATCH;

            // Find max absolute value for diagnostic logging
            int32_t max_abs = 0;
            for (i = 0; i < to_write; i++) {
                int32_t a = acc[i];
                if (a < 0) a = -a;
                if (a > max_abs) max_abs = a;
            }

            // master_gain 0.5→1.0: with volume sliders fixed and soft-clipper in place,
            // 0.5 was cutting max volume to -6dB. Clipping is handled by soft-clipper.
            float master_gain = 1.0f;

            // Soft-clip int32 accumulation to int16 range and copy to output buffer
            for (i = 0; i < to_write; i++) {
                float x = (float)acc[i] * master_gain;
                float ax = (x < 0.0f) ? -x : x;
                if (ax > 32767.0f) {
                    float norm = ax / 32767.0f;
                    float clipped = ax / (1.0f + norm * 0.5f);
                    x = (x < 0.0f) ? -clipped : clipped;
                }
                int32_t s = (int32_t)(x);
                if (s < -32768) s = -32768;
                if (s > 32767)  s = 32767;
                out[i] = (int16_t)s;
            }

            // DC blocking: leaky HPF (one-pole high-pass filter)
            // FIX: replaced batch-average subtraction with smooth HPF
            // Batch-average caused per-batch discontinuities (86Hz modulation)
            // HPF: y[n] = x[n] - x[n-1] + alpha * y[n-1]
            static float dc_prev_in = 0.0f;
            static float dc_prev_out = 0.0f;
            float alpha = 0.997f; // cutoff ~1Hz at 44100Hz: 1 - exp(-2*pi*1/44100)
            for (i = 0; i < to_write; i++) {
                float x_in = (float)out[i];
                float x_out = x_in - dc_prev_in + alpha * dc_prev_out;
                dc_prev_in = x_in;
                dc_prev_out = x_out;
                int32_t s = (int32_t)x_out;
                if (s < -32768) s = -32768;
                if (s > 32767)  s = 32767;
                out[i] = (int16_t)s;
            }

            int rc = fn_write(out, (unsigned int)(to_write * sizeof(int16_t)));
            mixer_write_count++;

            // Diagnostic logging: log first batch, then every 1000 batches
            if (first_log && mixer_write_count == 1) {
                snd_dbg("mixer: first write %d bytes rc=%d active=%d max_abs=%d gain=%.2f\n",
                        to_write * sizeof(int16_t), rc, active, max_abs, master_gain);
                first_log = 0;
            }
            if (mixer_write_count % 1000 == 0) {
                snd_dbg("mixer: batch %d rc=%d max_abs=%d enobufs=%d partial=%d\n",
                        mixer_write_count, rc, max_abs, mixer_enobufs_count, mixer_partial_count);
            }

            // Track write return value vs expected
            if (rc > 0 && rc < (int)(to_write * sizeof(int16_t))) {
                mixer_partial_count++;
                // Partial write — AudioTrack buffer is nearly full
            }
            if (rc < 0) {
                if (rc == -19) {
                    mixer_enobufs_count++;
                    if (mixer_enobufs_count <= 10) {
                        snd_dbg("mixer: ENOBUFS at batch %d (total=%d)\n", mixer_write_count, mixer_enobufs_count);
                    }
                } else {
                    snd_dbg("mixer: write error %d at batch %d\n", rc, mixer_write_count);
                }
                usleep(3000);
            }
        }

        // No sleep — AudioTrack's buffer absorbs timing jitter.
        // Writing as fast as possible keeps the buffer filled.
    }

    snd_dbg("mixer: exited after %d writes\n", mixer_write_count);
    return NULL;
}

// ---------------------------------------------------------------------------
// Sound lump loading
// ---------------------------------------------------------------------------

// DOOM sound lump format:
//   bytes 0-3:  sample rate (LE int32, usually 11025)
//   bytes 4-7:  sample count (LE int32, not always accurate)
//   bytes 8+:   raw 8-bit unsigned PCM data
//
// We trust (lump_length - 8) over the header's sample count.

static unsigned char *load_sound_data(int lumpnum, int *out_length)
{
    int lump_len = W_LumpLength(lumpnum);
    if (lump_len < 2) {
        return NULL;
    }

    void *lump = W_CacheLumpNum(lumpnum, PU_STATIC);
    if (!lump)
        return NULL;

    // DS lumps appear to be raw 8-bit unsigned PCM (no header).
    // The DP lumps are the digitized sound params (compressed/delta).
    // DS lumps are the raw PCM data starting at byte 0.
    unsigned char *pcm = (unsigned char *)lump;
    *out_length = lump_len;
    return pcm;
}

// ---------------------------------------------------------------------------
// Public API (implements i_sound.h)
// ---------------------------------------------------------------------------

void I_InitSound(void)
{
    if (initialized)
        return;

    // Lower doom's priority so audioflinger gets CPU time
    setpriority(PRIO_PROCESS, 0, 10);

    // Preload libutils.so with RTLD_GLOBAL so its symbols are visible
    // to libaudiowrapper.so's DT_NEEDED resolution.
    void *libutils_handle = dlopen("libutils.so", RTLD_NOW | RTLD_GLOBAL);
    snd_dbg("init: dlopen libutils=%p\n", libutils_handle);
    if (!libutils_handle) {
        snd_dbg("init: libutils dlopen FAILED: %s\n", dlerror());
    }

    lib_handle = dlopen("/data/local/bin/libaudiowrapper.so", RTLD_NOW);
    if (!lib_handle) {
        return;
    }

    fn_init       = (audio_init_fn)dlsym(lib_handle, "audio_init");
    fn_write      = (audio_write_fn)dlsym(lib_handle, "audio_write");
    fn_stop       = (audio_stop_fn)dlsym(lib_handle, "audio_stop");
    fn_shutdown   = (audio_shutdown_fn)dlsym(lib_handle, "audio_shutdown");

    if (!fn_init || !fn_write || !fn_stop || !fn_shutdown) {
        return;
    }

    // Initialize AudioTrack: 44100 Hz, 16-bit mono
    if (fn_init(OUT_RATE, OUT_BITS) != 0) {
        return;
    }


    // Zero channels
    memset(channels, 0, sizeof(channels));

    // Start mixer thread
    mixer_running = 1;
    if (pthread_create(&mixer_tid, NULL, mixer_thread, NULL) != 0) {
        mixer_running = 0;
        return;
    }

    // Configure Voodoo Sound: +30dB digital gain (max, default is -12dB)
    int vs_fd = open("/sys/devices/virtual/misc/voodoo_sound/digital_gain", O_WRONLY);
    if (vs_fd >= 0) {
        write(vs_fd, "30000", 5);
        close(vs_fd);
        snd_dbg("voodoo: digital_gain=+30dB\n");
    }
    // Enable speaker tuning
    vs_fd = open("/sys/devices/virtual/misc/voodoo_sound/speaker_tuning", O_WRONLY);
    if (vs_fd >= 0) {
        write(vs_fd, "1", 1);
        close(vs_fd);
        snd_dbg("voodoo: speaker_tuning=1\n");
    }

    initialized = 1;
    snd_dbg("INIT: OK rate=%d bits=%d\n", OUT_RATE, OUT_BITS);

    // Initialize music backend
    I_InitMusic();
}

void I_ShutdownSound(void)
{
    if (!initialized)
        return;

    // Stop mixer thread
    mixer_running = 0;
    pthread_join(mixer_tid, NULL);

    // Stop audio
    if (fn_stop)
        fn_stop();

    // Shutdown music backend
    I_ShutdownMusic();

    // NOTE: don't call fn_shutdown() or dlclose() — NDK libstdc++ allocator
    // is incompatible with device bionic heap. Process exit reclaims memory.
    initialized = 0;
}

void I_SetChannels(void)
{
    // Backend manages its own channel pool. Nothing to do.
}

int I_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    if (sfxinfo->lumpnum >= 0)
        return sfxinfo->lumpnum;

    // DOOM digital sound lump name: "DS" + uppercase sfx name.
    // e.g. "pistol" → "DSPISTOL", "shotgn" → "DSSHOTGN"
    // The DPxxxxx lumps are parameter headers, DSxxxxx are the PCM data.
    char name[10];
    name[0] = 'D';
    name[1] = 'S';
    int i;
    for (i = 0; sfxinfo->name[i] && i < 8; i++) {
        unsigned char ch = (unsigned char)sfxinfo->name[i];
        // Uppercase only ASCII letters; preserve other bytes (control chars, digits)
        if (ch >= 'a' && ch <= 'z')
            ch = ch - 32;
        name[2 + i] = (char)ch;
    }
    name[2 + i] = '\0';

    sfxinfo->lumpnum = W_CheckNumForName(name);
    snd_dbg("GetSfxLump: sfx=%s lump=%s num=%d\n",
            sfxinfo->name, name, sfxinfo->lumpnum);
    return sfxinfo->lumpnum;
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    (void)sep;    // mono output, stereo panning not implemented
    (void)priority; // no channel stealing yet

    if (!initialized || !fn_write || id <= 0 || id >= NUMSFX) {
        snd_dbg("StartSound SKIP: id=%d init=%d fn_write=%p\n", id, initialized, (void*)fn_write);
        return -1;
    }

    sfxinfo_t *sfx = &S_sfx[id];
    snd_dbg("StartSound: id=%d name=%s vol=%d pitch=%d\n", id, sfx->name, vol, pitch);

    // Follow sound links (e.g. chgun → pistol with pitch/volume shift)
    if (sfx->link) {
        sfx = sfx->link;
        if (sfx->pitch)
            pitch = (pitch * sfx->pitch) / 128;
        if (sfx->volume)
            vol = (vol * sfx->volume) / 128;
    }

    // Find a free channel slot
    int slot = -1;
    for (int c = 0; c < MAX_CHANNELS; c++) {
        if (!channels[c].playing) {
            slot = c;
            break;
        }
    }
    if (slot < 0)
        return -1;  // no channels available

    // Load sound data from WAD
    if (sfx->lumpnum < 0)
        I_GetSfxLumpNum(sfx);

    if (sfx->lumpnum < 0) {
        snd_dbg("  lump lookup failed\n");
        return -1;
    }
    snd_dbg("  lumpnum=%d\n", sfx->lumpnum);

    int length = 0;
    unsigned char *data = load_sound_data(sfx->lumpnum, &length);
    if (!data || length <= 0) {
        snd_dbg("  load_sound_data failed len=%d\n", length);
        return -1;
    }
    snd_dbg("  data=%p len=%d slot=%d\n", (void*)data, length, slot);

    // Clamp volume
    if (vol < 0)    vol = 0;
    if (vol > 127)  vol = 127;

    // Set up channel
    channels[slot].data    = data;
    channels[slot].length  = length;
    channels[slot].pos     = 0;
    channels[slot].vol     = vol;
    channels[slot].pitch   = pitch;
    channels[slot].playing = 1;
    snd_dbg("  channel %d armed\n", slot);
    return slot;
}

void I_StopSound(int handle)
{
    if (handle >= 0 && handle < MAX_CHANNELS) {
        channels[handle].playing = 0;
        channels[handle].pos = 0;
    }
}

int I_SoundIsPlaying(int handle)
{
    if (handle >= 0 && handle < MAX_CHANNELS) {
        return channels[handle].playing;
    }
    return 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    (void)sep;
    if (handle >= 0 && handle < MAX_CHANNELS && channels[handle].playing) {
        if (vol < 0)    vol = 0;
        if (vol > 127)  vol = 127;
        channels[handle].vol   = vol;
        channels[handle].pitch = pitch;
    }
}

void I_SetSfxVolume(int volume)
{
    if (volume < 0)    volume = 0;
    if (volume > 127)  volume = 127;
    sfx_volume = volume;
}

// These are called from d_main.c (currently commented out in fbdoom).
// No-op for the push-model mixer thread.
void I_UpdateSound(void) { }
void I_SubmitSound(void) { }
