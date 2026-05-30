// Show zones (bag→pgen mapping) for specific presets
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

int main(int argc, char **argv) {
    FILE *f = fopen("/Users/jack/Downloads/SC-55.SF2", "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(fsize);
    fread(data, 1, fsize, f); fclose(f);

    // Find all chunks
    const unsigned char *phdr=NULL,*pbag=NULL,*pgen=NULL,*shdr=NULL;
    int phdr_sz=0, pbag_sz=0, pgen_sz=0, shdr_sz=0;
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
                    if (!memcmp(sid,"phdr",4)){phdr=data+sp+8;phdr_sz=ssize;}
                    if (!memcmp(sid,"pbag",4)){pbag=data+sp+8;pbag_sz=ssize;}
                    if (!memcmp(sid,"pgen",4)){pgen=data+sp+8;pgen_sz=ssize;}
                    if (!memcmp(sid,"shdr",4)){shdr=data+sp+8;shdr_sz=ssize;}
                    sp += 8+ssize; if(ssize%2) sp++;
                }
            }
        }
        pos += 8+csize; if(csize%2) pos++;
    }

    int n_pbag = pbag_sz / 4;
    int n_pgen = pgen_sz / 4;
    
    // Find bank=0 presets
    typedef struct { int prog, bag_idx, bag_len; } preset_t;
    preset_t presets[128] = {0};
    int seen_bank[128] = {0};
    
    int n_presets = phdr_sz / 38;
    for (int i = 0; i < n_presets; i++) {
        const unsigned char *p = phdr + i*38;
        int pn = read16le(p+20);
        int bank = read16le(p+22);
        if (bank == 0 && pn < 128 && !seen_bank[pn]) {
            seen_bank[pn] = 1;
            presets[pn].prog = pn;
            presets[pn].bag_idx = read16le(p+24);
            presets[pn].bag_len = read16le(p+26);
        }
    }

    // Show zones for DOOM-relevant progs
    int doom_progs[] = {1, 32, 38, 39, 50, 51, 62, 63};
    
    for (int p = 0; p < 8; p++) {
        int prog = doom_progs[p];
        printf("\n=== Prog %d (bag_idx=%d, bag_len=%d) ===\n", 
               prog, presets[prog].bag_idx, presets[prog].bag_len);
        
        int zi = presets[prog].bag_idx;
        int zl = presets[prog].bag_len;
        
        if (!zl) { printf("  NO ZONES!\n"); continue; }
        
        // Show each zone's pgen records
        for (int j = 0; j < zl && zi+j < n_pbag; j++) {
            int gi = read32le(pbag+(zi+j)*4) & 0xFFFF;
            int next_gi = (zi+j+1 < n_pbag) ? (read32le(pbag+(zi+j+1)*4)&0xFFFF) : gi;
            int count = next_gi - gi;
            
            printf("  Zone %d (pgen[%d..%d], %d records):", j, gi, gi+count-1, count);
            for (int k = 0; k < count && gi+k < n_pgen; k++) {
                int op = read16le(pgen+(gi+k)*4);
                int val = read16le(pgen+(gi+k)*4+2);
                if (op == 48) {  // SAMPLE_ID
                    const unsigned char *s = shdr + val*46;
                    char name[17]; memcpy(name, s, 16); name[16]=0;
                    int ok = s[40];  // orig_key at offset 40
                    printf(" SAMPLE[%d](%s,key=%d)", val, name, ok);
                } else if (op == 41) {  // Some SC-55 op
                    printf(" OP41(%d)", val);
                } else if (op < 10) {
                    printf(" OP%d(%d)", op, val);
                }
            }
            printf("\n");
        }
    }
    
    free(data);
    return 0;
}
