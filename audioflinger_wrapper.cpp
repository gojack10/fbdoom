// audioflinger_wrapper.cpp — thin C++ wrapper around android::AudioTrack
// Compiled as .so, loaded via dlopen from doom's C code.
//
// After AudioTrack init, also configures the ALSA codec (WM8994) mixer
// to max volume on speaker path, and sets Voodoo Sound digital gain to 0.
//
// Build:
//   armv7a-linux-androideabi21-clang++ -shared -fPIC -std=c++03 \
//     -stdlib=libstdc++ -fno-exceptions \
//     -target armv7a-linux-androideabi21 \
//     -Wl,--allow-shlib-undefined,-rpath,/system/lib \
//     -L/tmp -lmedia -lutils -lbinder \
//     -o libaudiowrapper.so audioflinger_wrapper.cpp

#include <stdlib.h>
#include <string.h>
#include <new>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

enum audio_stream_type_t { AUDIO_STREAM_MUSIC = 3, AUDIO_STREAM_VOICE_CALL = 0, AUDIO_STREAM_ALARM = 4, AUDIO_STREAM_DEFAULT = 6 };
enum audio_format_t { AUDIO_FORMAT_PCM_16_BIT = 1, AUDIO_FORMAT_PCM_8_BIT = 2 };
enum audio_output_flags_t { AUDIO_OUTPUT_FLAG_NONE = 0 };

namespace android {
class AudioTrack {
public:
    AudioTrack(int streamType, unsigned int sampleRate, int format, int channelCount,
               int bufferSize, unsigned int flags,
               void (*callback)(int, void*, void*), void *user,
               int minBuffer, int notificationInterval);
    ~AudioTrack();

    int  initCheck() const;
    void start();
    void stop();
    int  write(const void* buffer, unsigned int size);
    void setVolume(float left, float right);
};
}

static android::AudioTrack* g_track = NULL;
static int g_initialized = 0;

#define AUDIOTRACK_ALLOC_SIZE 512
static char g_track_mem[AUDIOTRACK_ALLOC_SIZE];

// Set an ALSA mixer control by numid to specified value(s)
static void set_alsa_numid(int ctl_fd, unsigned int numid, long v0, long v1) {
    struct snd_ctl_elem_value val;
    memset(&val, 0, sizeof(val));
    val.id.numid = numid;
    val.value.integer.value[0] = v0;
    val.value.integer.value[1] = v1;
    ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val);
}

extern "C" {

int audio_init(int sample_rate, int bits_per_sample) {
    if (g_initialized) return 0;

    g_track = new(g_track_mem) android::AudioTrack(
        AUDIO_STREAM_DEFAULT,
        (unsigned int)sample_rate,
        (bits_per_sample == 8) ? AUDIO_FORMAT_PCM_8_BIT : AUDIO_FORMAT_PCM_16_BIT,
        1u,
        4096,
        AUDIO_OUTPUT_FLAG_NONE,
        NULL, NULL, 0, 0
    );

    int rc = g_track->initCheck();
    if (rc != 0) {
        g_track = NULL;
        return -1;
    }

    g_track->setVolume(1.0f, 1.0f);
    g_track->start();
    g_initialized = 1;

    // Wait for AudioFlinger HAL to finish initializing the codec
    // (it sets Playback Path to RCV for DEFAULT stream, we override to SPK)
    usleep(200000);  // 200ms

    // Configure ALSA codec mixer for max speaker volume
    int ctl_fd = open("/dev/snd/controlC0", O_RDWR);
    if (ctl_fd >= 0) {
        // Route playback through speaker
        set_alsa_numid(ctl_fd, 5, 2, 0);  // Playback Path = SPK
        // Max out all volume stages
        set_alsa_numid(ctl_fd, 1, 63, 63); // Playback Volume L+R
        set_alsa_numid(ctl_fd, 2, 63, 63); // Playback Spkr Volume L+R
        close(ctl_fd);
    }

    return 0;
}

int audio_write(const void* buffer, unsigned int size) {
    if (!g_track || !g_initialized) return -1;
    return g_track->write(buffer, size);
}

void audio_stop(void) {
    if (g_track && g_initialized) {
        g_track->stop();
        g_initialized = 0;
    }
}

void audio_shutdown(void) {
    if (g_track && g_initialized) {
        g_track->stop();
        g_initialized = 0;
    }
    g_track = NULL;
}

} // extern "C"