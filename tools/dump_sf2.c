// Dump all SF2 presets with zone→sample mappings
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
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(fsize);
    fread(data, 1, fsize, f); fclose(f);

    // Find all chunks in pdta
    const unsigned char *phdr=NULL, *pbag=NULL, *pgen=NULL, *shdr=NULL;
    int phdr_sz=0, pbag_sz=0, pgen_sz=0, shdr_sz=0;
    int pos = 12;
    while (pos < fsize - 8) {
        char cid[5]; memcpy(cid, data+pos, 4); cid[4]=0;
        int csize = read32le(data+pos+4); int doff = pos+8;
        if (memcmp(cid,"LIST",4)==0) {
            char lt[5]; memcpy(lt, data+doff, 4); lt[4]=0;
            int sp = doff+4;
            if (memcmp(lt,"pdta",4)==0) {
                while (sp < doff+csize-8) {
                    char sid[5]; memcpy(sid, data+sp, 4); sid[4]=0;
                    int ssize = read32le(data+sp+4);
                    int sdata = sp+8;
                    if (!memcmp(sid,"phdr",4)){phdr=data+sdata;phdr_sz=ssize;}
                    if (!memcmp(sid,"pbag",4)){pbag=data+sdata;pbag_sz=ssize;}
                    if (!memcmp(sid,"pgen",4)){pgen=data+sdata;pgen_sz=ssize;}
                    if (!memcmp(sid,"shdr",4)){shdr=data+sdata;shdr_sz=ssize;}
                    sp += 8+ssize; if(ssize%2) sp++;
                }
            }
        }
        pos += 8+csize; if(csize%2) pos++;
    }

    int n_shdr = shdr_sz / 46;
    int n_pbag = pbag_sz / 4;
    int n_pgen = pgen_sz / 4;
    
    // Parse phdr: find bank=0 presets per program
    typedef struct { int prog, bag_idx, bag_len; } preset_t;
    preset_t presets[128] = {0};
    
    int n_presets = phdr_sz / 38;
    for (int i = 0; i < n_presets; i++) {
        const unsigned char *p = phdr + i*38;
        int pn = read16le(p+20);
        int bank = read16le(p+22);
        if (bank == 0 && pn < 128 && !presets[pn].prog && !presets[pn].bag_len) {
            presets[pn].prog = pn;
            presets[pn].bag_idx = read16le(p+24);
            presets[pn].bag_len = read16le(p+26);
        }
    }

    // For each preset, show zones and their sample assignments
    for (int prog = 0; prog < 128; prog++) {
        if (!presets[prog].bag_len) continue;
        
        int zi = presets[prog].bag_idx;
        int zl = presets[prog].bag_len;
        
        // Collect unique sample indices from this preset
        typedef struct { int idx, orig_key; } se_t;
        se_t samples[64] = {0};
        int sc = 0;
        
        for (int j = 0; j < zl && zi+j < n_pbag; j++) {
            int gi = read32le(pbag+(zi+j)*4) & 0xFFFF;
            int next_gi = (zi+j+1 < n_pbag) ? (read32le(pbag+(zi+j+1)*4)&0xFFFF) : gi;
            int count = next_gi - gi;
            for (int k = 0; k < count && gi+k < n_pgen; k++) {
                int op = read16le(pgen+(gi+k)*4);
                int val = read16le(pgen+(gi+k)*4+2);
                if (op == 48 && val >= 0 && val < n_shdr) {
                    int dup = 0;
                    for (int d=0; d<sc; d++) if(samples[d].idx==val){dup=1;break;}
                    if (!dup && sc < 64) {
                        const unsigned char *s = shdr + val*46;
                        samples[sc].idx = val;
                        samples[sc].orig_key = s[40];
                        sc++;
                    }
                }
            }
        }
        
        if (!sc) continue;
        
        // Build note→sample map (nearest orig_key)
        printf("Prog %2d: ", prog);
        for (int s=0; s<sc; s++) {
            if (s) printf("       ");
            printf("sample %3d (key=%2d)", samples[s].idx, samples[s].orig_key);
            // Show sample name
            const unsigned char *sn = shdr + samples[s].idx*46;
            char name[17]; memcpy(name, sn, 16); name[16]=0;
            printf(" \"%s\"", name);
        }
        printf("\n");
    }
    
    free(data);
    return 0;
}
