// dump_sample.c — dump one SF2 sample to WAV for listening
// gcc -o dump_sample dump_sample.c ../src/device/sf2_loader.c -I../src -lm
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static uint32_t read32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv) {
    const char *sf2_path = "/Users/jack/Downloads/SC-55.SF2";
    if (argc > 1) sf2_path = argv[1];

    FILE *f = fopen(sf2_path, "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *fd = malloc(fsize);
    fread(fd, 1, fsize, f); fclose(f);

    // Find smpl chunk offset
    int smpl_off = 0;
    int pos = 12;
    while (pos < fsize - 8) {
        int cs = read32le(fd + pos + 4);
        int doff = pos + 8;
        if (memcmp(fd+pos, "LIST", 4) == 0 && memcmp(fd+doff, "sdta", 4) == 0) {
            int sp = doff + 4;
            while (sp < doff + cs - 8) {
                int ss = read32le(fd + sp + 4);
                if (memcmp(fd+sp, "smpl", 4) == 0) {
                    smpl_off = sp + 8;
                }
                sp += 8 + ss; if (ss % 2) sp++;
            }
        }
        pos += 8 + cs; if (cs % 2) pos++;
    }

    // Find shdr chunk
    unsigned char *shdr = NULL; int shdr_sz = 0;
    pos = 12;
    while (pos < fsize - 8) {
        int cs = read32le(fd + pos + 4);
        int doff = pos + 8;
        if (memcmp(fd+pos, "LIST", 4) == 0 && memcmp(fd+doff, "pdta", 4) == 0) {
            int sp = doff + 4;
            while (sp < doff + cs - 8) {
                int ss = read32le(fd + sp + 4);
                int sdoff = sp + 8;
                if (memcmp(fd+sp, "shdr", 4) == 0) {
                    shdr = fd + sdoff; shdr_sz = ss;
                }
                sp += 8 + ss; if (ss % 2) sp++;
            }
        }
        pos += 8 + cs; if (cs % 2) pos++;
    }

    int n_shdr = shdr_sz / 46;
    printf("Total samples: %d, smpl offset: 0x%x\n\n", n_shdr, smpl_off);

    // Pick a few interesting samples to dump
    // Sample indices from pgen: 350 (note 0), 372 (note 1), 260 (note 27/bass range)
    int samples_to_dump[] = {260, 350, 372, 464};
    int n_dump = 4;

    for (int d = 0; d < n_dump; d++) {
        int idx = samples_to_dump[d];
        if (idx >= n_shdr) continue;

        unsigned char *s = shdr + idx * 46;
        char name[21]; memcpy(name, s, 20); name[20] = '\0';
        int start = read32le(s + 20);
        int end = read32le(s + 24);
        int loop_start = read32le(s + 28);
        int loop_end = read32le(s + 32);
        int rate = read32le(s + 36);
        int orig_key = s[40];

        int len_bytes = end - start;
        int len_samples = len_bytes / 2;

        printf("Sample %d: '%s' start=0x%x end=0x%x len=%d bytes (%d samples, %.2fs at %dHz) orig_key=%d\n",
               idx, name, start, end, len_bytes, len_samples, (double)len_samples/rate, rate, orig_key);

        // Write WAV
        char wavpath[256];
        snprintf(wavpath, sizeof(wavpath), "/tmp/sf2_sample_%d.wav", idx);

        FILE *wf = fopen(wavpath, "wb");
        if (!wf) { perror("fopen wav"); continue; }

        // RIFF header
        int data_size = len_bytes;
        fwrite("RIFF", 1, 4, wf);
        uint32_t riff_size = 36 + data_size;
        fwrite(&riff_size, 4, 1, wf);
        fwrite("WAVE", 1, 4, wf);
        fwrite("fmt ", 1, 4, wf);
        uint32_t fmt_size = 16;
        fwrite(&fmt_size, 4, 1, wf);
        uint16_t audio_fmt = 1;  // PCM
        fwrite(&audio_fmt, 2, 1, wf);
        uint16_t channels = 1;
        fwrite(&channels, 2, 1, wf);
        uint32_t sample_rate = rate;
        fwrite(&sample_rate, 4, 1, wf);
        uint32_t byte_rate = rate * 2;
        fwrite(&byte_rate, 4, 1, wf);
        uint16_t block_align = 2;
        fwrite(&block_align, 2, 1, wf);
        uint16_t bits_per_sample = 16;
        fwrite(&bits_per_sample, 2, 1, wf);
        fwrite("data", 1, 4, wf);
        fwrite(&data_size, 4, 1, wf);

        // Sample data
        fseek(f, smpl_off + start, SEEK_SET);
        // Re-open for reading sample data
        fclose(wf);

        // Use the already-loaded file data
        wf = fopen(wavpath, "rb+");
        fseek(wf, ftell(f), SEEK_SET);  // position at end of headers
        // Actually let's just rewrite the whole thing properly
        fclose(wf);

        // Simpler approach: write all at once
        FILE *wf2 = fopen(wavpath, "wb");
        unsigned char wav[44 + len_bytes];
        int wpos = 0;
        memcpy(wav+wpos, "RIFF", 4); wpos += 4;
        uint32_t rs = 36 + len_bytes; memcpy(wav+wpos, &rs, 4); wpos += 4;
        memcpy(wav+wpos, "WAVEfmt ", 8); wpos += 8;
        uint32_t fs = 16; memcpy(wav+wpos, &fs, 4); wpos += 4;
        uint16_t af = 1; memcpy(wav+wpos, &af, 2); wpos += 2;
        uint16_t ch = 1; memcpy(wav+wpos, &ch, 2); wpos += 2;
        uint32_t sr = rate; memcpy(wav+wpos, &sr, 4); wpos += 4;
        uint32_t br = rate * 2; memcpy(wav+wpos, &br, 4); wpos += 4;
        uint16_t ba = 2; memcpy(wav+wpos, &ba, 2); wpos += 2;
        uint16_t bps = 16; memcpy(wav+wpos, &bps, 2); wpos += 2;
        memcpy(wav+wpos, "data", 4); wpos += 4;
        uint32_t ds = len_bytes; memcpy(wav+wpos, &ds, 4); wpos += 4;
        memcpy(wav+wpos, fd + smpl_off + start, len_bytes); wpos += len_bytes;

        fwrite(wav, 1, wpos, wf2);
        fclose(wf2);
        printf("  -> wrote %s (%d bytes)\n\n", wavpath, wpos);
    }

    free(fd);
    return 0;
}