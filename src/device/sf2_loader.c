#include "sf2_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// RIFF/SF2 constants
// ---------------------------------------------------------------------------

#define SF2_PHDR_STRIDE  38
#define SF2_PBAG_STRIDE   4
#define SF2_PGEN_STRIDE   4
#define SF2_INST_STRIDE  22
#define SF2_IBAG_STRIDE   4
#define SF2_IGEN_STRIDE   4
#define SF2_SHDR_STRIDE  46

// pgen op codes (SC-55 specific)
#define PGEN_OP_SAMPLE_ID  48  // 0x30
#define PGEN_OP_KEY_RANGE  41  // 0x29

// igen op codes (standard SF2)
#define IGEN_OP_SAMPLE_ID  31  // 0x1F

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

sf2_sample_t *sf2_samples = NULL;
sf2_program_t sf2_programs[128];
int sf2_num_samples = 0;

// ---------------------------------------------------------------------------
// Global file data for on-demand sample loading
// ---------------------------------------------------------------------------

unsigned char *_sf2_fdata = NULL;
int _sf2_smpl_off = 0;

// ---------------------------------------------------------------------------
// RIFF chunk reading helpers
// ---------------------------------------------------------------------------

static uint32_t read32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read16le(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int read_chunk(const unsigned char *data, int pos, int datasize,
                      char *id_out, int *size_out, int *data_off_out)
{
    if (pos + 8 > datasize) return -1;
    memcpy(id_out, data + pos, 4);
    *size_out = read32le(data + pos + 4);
    *data_off_out = pos + 8;
    return 0;
}

// ---------------------------------------------------------------------------
// SF2 loader
// ---------------------------------------------------------------------------

int sf2_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char *fdata = (unsigned char *)malloc(fsize);
    if (!fdata) { fclose(f); return -1; }
    fread(fdata, 1, fsize, f);
    fclose(f);

    // Walk RIFF tree to find LIST pdta and LIST sdta
    int pos = 12;
    unsigned char *pdta_data = NULL;
    int pdta_size = 0;
    int smpl_off = 0;

    while (pos < fsize - 8) {
        char cid[5] = {0};
        int csize, data_off;
        if (read_chunk(fdata, pos, fsize, cid, &csize, &data_off) < 0) break;

        if (memcmp(cid, "LIST", 4) == 0) {
            char list_type[5] = {0};
            memcpy(list_type, fdata + data_off, 4);
            int subpos = data_off + 4;

            if (memcmp(list_type, "pdta", 4) == 0) {
                pdta_data = fdata + subpos;
                pdta_size = csize - 4;
            } else if (memcmp(list_type, "sdta", 4) == 0) {
                // Find smpl subchunk within sdta
                int sp = subpos;
                while (sp < data_off + csize - 8) {
                    char sub_id[5] = {0};
                    int sub_size, sub_data_off;
                    if (read_chunk(fdata, sp, fsize, sub_id, &sub_size, &sub_data_off) < 0) break;
                    if (memcmp(sub_id, "smpl", 4) == 0) {
                        smpl_off = sub_data_off;
                    }
                    sp += 8 + sub_size;
                    if (sub_size % 2) sp++;
                }
            }
        }

        pos += 8 + csize;
        if (csize % 2) pos++;
    }

    if (!pdta_data || !smpl_off) {
        free(fdata);
        return -1;
    }

    // Find subchunks within pdta
    unsigned char *phdr_data = NULL, *pbag_data = NULL, *pgen_data = NULL;
    unsigned char *inst_data = NULL, *ibag_data = NULL, *igen_data = NULL;
    unsigned char *shdr_data = NULL;
    int phdr_size = 0, pbag_size = 0, pgen_size = 0;
    int inst_size = 0, ibag_size = 0, igen_size = 0;
    int shdr_size = 0;

    int sp = 0;
    while (sp < pdta_size - 8) {
        char sub_id[5] = {0};
        int sub_size, sub_data_off;
        if (read_chunk(pdta_data, sp, pdta_size, sub_id, &sub_size, &sub_data_off) < 0) break;

        if (memcmp(sub_id, "phdr", 4) == 0) {
            phdr_data = pdta_data + sub_data_off;
            phdr_size = sub_size;
        } else if (memcmp(sub_id, "pbag", 4) == 0) {
            pbag_data = pdta_data + sub_data_off;
            pbag_size = sub_size;
        } else if (memcmp(sub_id, "pgen", 4) == 0) {
            pgen_data = pdta_data + sub_data_off;
            pgen_size = sub_size;
        } else if (memcmp(sub_id, "inst", 4) == 0) {
            inst_data = pdta_data + sub_data_off;
            inst_size = sub_size;
        } else if (memcmp(sub_id, "ibag", 4) == 0) {
            ibag_data = pdta_data + sub_data_off;
            ibag_size = sub_size;
        } else if (memcmp(sub_id, "igen", 4) == 0) {
            igen_data = pdta_data + sub_data_off;
            igen_size = sub_size;
        } else if (memcmp(sub_id, "shdr", 4) == 0) {
            shdr_data = pdta_data + sub_data_off;
            shdr_size = sub_size;
        }

        sp += 8 + sub_size;
        if (sub_size % 2) sp++;
    }

    if (!phdr_data || !shdr_data || !pbag_data || !pgen_data) {
        free(fdata);
        return -1;
    }

    // -----------------------------------------------------------------------
    // Parse shdr: sample headers
    // -----------------------------------------------------------------------

    int n_shdr = shdr_size / SF2_SHDR_STRIDE;
    sf2_num_samples = n_shdr;
    sf2_samples = (sf2_sample_t *)calloc(n_shdr, sizeof(sf2_sample_t));

    for (int i = 0; i < n_shdr; i++) {
        unsigned char *s = shdr_data + i * SF2_SHDR_STRIDE;
        // Name is first 16 bytes
        char name[17];
        memcpy(name, s, 16);
        name[16] = '\0';

        // SC-55 shdr layout (46 bytes): [0-15]=name [16-19]=pad [20]=start [24]=end
        // [28]=loop_start [32]=loop_end [36]=sampleRate [40]=originalPitch [41]=pitchCorrection
        sf2_samples[i].start = read32le(s + 20);
        sf2_samples[i].end = read32le(s + 24);
        sf2_samples[i].loop_start = read32le(s + 28);
        sf2_samples[i].loop_end = read32le(s + 32);
        sf2_samples[i].sample_rate = read32le(s + 36);
        sf2_samples[i].orig_key = s[40];  // originalPitch at offset 40 in SC-55 layout
        sf2_samples[i].data = NULL;  // loaded on demand
    }

    // -----------------------------------------------------------------------
    // Parse phdr: preset headers (38-byte records)
    // -----------------------------------------------------------------------

    int n_presets = phdr_size / SF2_PHDR_STRIDE;
    int preset_prog[128] = {0};
    int preset_bag_idx[128] = {0};
    int preset_bag_len[128] = {0};

    // Collect bank 0 presets (first occurrence per program)
    int preset_seen[128] = {0};
    for (int i = 0; i < n_presets; i++) {
        unsigned char *p = phdr_data + i * SF2_PHDR_STRIDE;
        int pn = read16le(p + 20);
        int bank = read16le(p + 22);
        int zi = read16le(p + 24);
        int zl = read16le(p + 26);
        if (bank == 0 && pn < 128 && !preset_seen[pn]) {
            preset_seen[pn] = 1;
            preset_prog[pn] = pn;
            preset_bag_idx[pn] = zi;
            preset_bag_len[pn] = zl;
        }
    }

    // -----------------------------------------------------------------------
    // Parse pbag: preset bag indices (4-byte records, count inferred)
    // -----------------------------------------------------------------------

    int n_pbag = pbag_size / SF2_PBAG_STRIDE;
    int *pbag_gi = (int *)malloc((n_pbag + 1) * sizeof(int));
    for (int i = 0; i < n_pbag; i++) {
        pbag_gi[i] = read32le(pbag_data + i * 4) & 0xFFFF;
    }
    if (n_pbag > 0) pbag_gi[n_pbag] = pbag_gi[n_pbag - 1]; // safety

    // -----------------------------------------------------------------------
    // Parse pgen: preset generators (4-byte records)
    // -----------------------------------------------------------------------

    int n_pgen = pgen_size / SF2_PGEN_STRIDE;

    // -----------------------------------------------------------------------
    // Build PER-PRESET zone -> sample mapping using OP41 note assignment.
    // SC-55 alternates SAMPLE_ID (op=48) and OP41 (op=41) pgen records.
    // OP41 value = MIDI note number that this sample plays.
    // E.g., SAMPLE[253] + OP41(39) means sample 253 plays MIDI note 39.
    // This replaces the broken nearest-orig_key approach.

    for (int prog = 0; prog < 128; prog++) {
        sf2_programs[prog].zones = NULL;
        sf2_programs[prog].num_zones = 0;
    }

    for (int prog = 0; prog < 128; prog++) {
        if (preset_bag_len[prog] == 0) continue;

        int zi = preset_bag_idx[prog];
        int zl = preset_bag_len[prog];

        // Parse SAMPLE-OP41 pairs from this preset's zones.
        // OP41 = direct MIDI note assignment. Each zone has SAMPLE[idx] + OP41(note).
        typedef struct { int sample_idx; int midi_note; } note_map_t;
        int max_mappings = zl * 2;  // generous upper bound
        note_map_t *mappings = (note_map_t *)calloc(max_mappings, sizeof(note_map_t));
        if (!mappings) continue;
        int mapping_count = 0;

        int cur_sample = -1;  // track current SAMPLE_ID as we walk pgen records
        for (int j = 0; j < zl && zi + j < n_pbag; j++) {
            int gi = read32le(pbag_data + (zi + j) * 4) & 0xFFFF;
            int next_gi = (zi + j + 1 < n_pbag) ?
                (read32le(pbag_data + (zi + j + 1) * 4) & 0xFFFF) : gi;
            int count = next_gi - gi;
            if (count < 0) count = 0;

            for (int k = 0; k < count && gi + k < n_pgen; k++) {
                int op = read16le(pgen_data + (gi + k) * 4);
                int val = read16le(pgen_data + (gi + k) * 4 + 2);
                if (op == PGEN_OP_SAMPLE_ID && val >= 0 && val < n_shdr) {
                    cur_sample = val;
                } else if (op == PGEN_OP_KEY_RANGE && cur_sample >= 0) {
                    // OP41 = direct MIDI note assignment
                    if (val >= 0 && val < 128 && mapping_count < max_mappings) {
                        mappings[mapping_count].sample_idx = cur_sample;
                        mappings[mapping_count].midi_note = val;
                        mapping_count++;
                    }
                }
            }
        }

        // Build zones from note→sample mappings.
        // If we have OP41 mappings, use them directly.
        // Otherwise fall back to nearest-orig_key.
        if (mapping_count > 0) {
            // Build per-note sample map from OP41 assignments
            int note_to_sample[128];
            for (int n = 0; n < 128; n++) note_to_sample[n] = -1;
            for (int m = 0; m < mapping_count; m++) {
                note_to_sample[mappings[m].midi_note] = mappings[m].sample_idx;
            }

            // Fill unmapped notes: find nearest mapped note
            for (int n = 0; n < 128; n++) {
                if (note_to_sample[n] >= 0) continue;
                int best = -1, best_dist = 9999;
                for (int m = 0; m < mapping_count; m++) {
                    int dist = n - mappings[m].midi_note;
                    if (dist < 0) dist = -dist;
                    if (dist < best_dist) {
                        best_dist = dist;
                        best = m;
                    }
                }
                if (best >= 0) note_to_sample[n] = mappings[best].sample_idx;
            }

            // Compress consecutive notes with same sample into zones
            sf2_zone_t *zones = (sf2_zone_t *)calloc(128, sizeof(sf2_zone_t));
            if (!zones) { free(mappings); continue; }

            int zone_count = 0;
            int first_mapped = 128, last_mapped = 0;
            for (int n = 0; n < 128; n++) {
                if (note_to_sample[n] >= 0) {
                    if (n < first_mapped) first_mapped = n;
                    if (n > last_mapped) last_mapped = n;
                }
            }

            int cur_s = note_to_sample[first_mapped];
            int zone_start = first_mapped;
            for (int n = first_mapped + 1; n <= last_mapped + 1; n++) {
                int this_s = (n < 128) ? note_to_sample[n] : -1;
                if (this_s != cur_s) {
                    zones[zone_count].sample_idx = cur_s;
                    zones[zone_count].key_lo = zone_start;
                    zones[zone_count].key_hi = n - 1;
                    zone_count++;
                    cur_s = this_s;
                    zone_start = n;
                }
            }

            sf2_programs[prog].zones = zones;
            sf2_programs[prog].num_zones = zone_count;
        }
        free(mappings);
    }

    // Fallback: if a program has no zones, point to first program that does
    int fallback_prog = -1;
    for (int prog = 0; prog < 128; prog++) {
        if (sf2_programs[prog].num_zones > 0) { fallback_prog = prog; break; }
    }
    for (int prog = 0; prog < 128; prog++) {
        if (sf2_programs[prog].num_zones == 0 && fallback_prog >= 0) {
            sf2_programs[prog].zones = sf2_programs[fallback_prog].zones;
            sf2_programs[prog].num_zones = sf2_programs[fallback_prog].num_zones;
        }
    }

    free(pbag_gi);

    // Keep fdata around for on-demand sample loading
    // We'll use it in sf2_get_sample_data
    // For now, store a pointer (this leaks if we reload, but fine for our use)
    // Actually, we need to keep fdata for sample data access

    // Store fdata for later sample loading (we'll use the raw file data)
    // The sample data is at smpl_off in fdata
    // We'll store this globally for sf2_get_sample_data to use
    _sf2_fdata = fdata;
    _sf2_smpl_off = smpl_off;

    return 0;
}

int sf2_get_sample(int program, int note, int *key_lo, int *key_hi) {
    if (program < 0 || program >= 128) return -1;
    sf2_program_t *prog = &sf2_programs[program];
    if (!prog->zones || prog->num_zones == 0) return -1;

    // Find matching zone for this note
    for (int i = 0; i < prog->num_zones; i++) {
        sf2_zone_t *z = &prog->zones[i];
        if (note >= z->key_lo && note <= z->key_hi) {
            if (key_lo) *key_lo = z->key_lo;
            if (key_hi) *key_hi = z->key_hi;
            return z->sample_idx;
        }
    }
    // Fall back to first zone
    if (key_lo) *key_lo = prog->zones[0].key_lo;
    if (key_hi) *key_hi = prog->zones[0].key_hi;
    return prog->zones[0].sample_idx;
}

int16_t *sf2_get_sample_data(int sample_idx) {
    if (!_sf2_fdata || sample_idx < 0 || sample_idx >= sf2_num_samples) return NULL;

    sf2_sample_t *s = &sf2_samples[sample_idx];
    if (s->data) return s->data;  // already loaded

    // Load sample data
    // shdr start/end/loop_start/loop_end are SAMPLE COUNTS (standard SF2).
    // 16-bit PCM → multiply by 2 for byte offsets into smpl chunk.
    int start = s->start;
    int end = s->end;
    int length_samples = end - start;
    if (length_samples <= 0) return NULL;

    int off = _sf2_smpl_off + start * 2;
    int length_bytes = length_samples * 2;

    s->data = (int16_t *)malloc(length_bytes);
    if (!s->data) return NULL;

    memcpy(s->data, _sf2_fdata + off, length_bytes);
    return s->data;
}

void sf2_free(void) {
    if (sf2_samples) {
        for (int i = 0; i < sf2_num_samples; i++) {
            if (sf2_samples[i].data) free(sf2_samples[i].data);
        }
        free(sf2_samples);
        sf2_samples = NULL;
    }
    // Free per-preset zone arrays — collect unique pointers first to avoid
    // double-free when fallback sharing makes multiple programs point to same array.
    sf2_zone_t *freed[128];
    int n_freed = 0;
    for (int i = 0; i < 128; i++) {
        if (!sf2_programs[i].zones) continue;
        int dup = 0;
        for (int j = 0; j < n_freed; j++) {
            if (freed[j] == sf2_programs[i].zones) { dup = 1; break; }
        }
        if (!dup) {
            freed[n_freed++] = sf2_programs[i].zones;
            free(sf2_programs[i].zones);
        }
        sf2_programs[i].zones = NULL;
    }
    if (_sf2_fdata) free(_sf2_fdata);
    _sf2_fdata = NULL;
    sf2_num_samples = 0;
}
