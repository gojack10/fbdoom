// Show ALL presets by bank+program, not just bank 0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t read32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint16_t read16le(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1]<<8);
}

int main() {
    FILE *f = fopen("/Users/jack/Downloads/SC-55.SF2", "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(fsize);
    fread(data, 1, fsize, f); fclose(f);

    // Find phdr
    const unsigned char *phdr = NULL; int phdr_sz = 0;
    int pos = 12;
    while (pos < fsize - 8) {
        char cid[5]; memcpy(cid, data+pos, 4);
        int csize = read32le(data+pos+4); int doff = pos+8;
        if (!memcmp(cid,"LIST",4)) {
            char lt[5]; memcpy(lt, data+doff, 4);
            int sp = doff+4;
            if (!memcmp(lt,"pdta",4)) {
                while (sp < doff+csize-8) {
                    char sid[5]; memcpy(sid, data+sp, 4);
                    int ssize = read32le(data+sp+4);
                    if (!memcmp(sid,"phdr",4)) { phdr=data+sp+8; phdr_sz=ssize; break; }
                    sp += 8+ssize; if(ssize%2) sp++;
                }
            }
        }
        pos += 8+csize; if(csize%2) pos++;
        if (phdr) break;
    }

    int n = phdr_sz / 38;
    printf("Total presets in phdr: %d\n\n", n);
    
    // Group by bank, show first occurrence per (bank, prog)
    typedef struct { int bank, prog, bag_idx, bag_len; char name[23]; } preset_t;
    preset_t seen[256][128] = {0};
    int count[256] = {0};
    
    for (int i = 0; i < n; i++) {
        const unsigned char *p = phdr + i*38;
        int pn = read16le(p+20);
        int bank = read16le(p+22);
        if (bank > 255 || pn > 127) continue;
        if (!seen[bank][pn].bag_len && !(seen[bank][pn].bank == bank && seen[bank][pn].prog == pn)) {
            seen[bank][pn].bank = bank;
            seen[bank][pn].prog = pn;
            seen[bank][pn].bag_idx = read16le(p+24);
            seen[bank][pn].bag_len = read16le(p+26);
            memcpy(seen[bank][pn].name, p, 20);
            seen[bank][pn].name[20] = '\0';
            count[bank]++;
        }
    }
    
    // Show which banks have presets
    for (int bank = 0; bank < 256; bank++) {
        if (!count[bank]) continue;
        printf("Bank %d: %d presets\n", bank, count[bank]);
        // Show progs 0-64 for this bank (DOOM-relevant range)
        for (int pn = 0; pn < 64; pn++) {
            if (seen[bank][pn].bank == bank && seen[bank][pn].prog == pn) {
                printf("  prog %2d (bag=%d len=%d): \"%.20s\"\n",
                       pn, seen[bank][pn].bag_idx, seen[bank][pn].bag_len,
                       seen[bank][pn].name);
            }
        }
        if (count[bank] > 64) printf("  ... (%d more progs)\n", count[bank] - 64);
        printf("\n");
    }
    
    free(data);
    return 0;
}
