// Render E1M2 MUS → WAV using real sf2_loader.c + i_android_music.c rendering logic.
// gcc -O2 -I../src -o render_e1m2 render_e1m2.c ../src/device/sf2_loader.c \
//     ../src/i_mus_convert.cpp ../src/mus2midi.cpp -std=c++03 -fno-exceptions -lm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "device/sf2_loader.h"

extern int convertToMidi(void *musData, void **midiOutput);

typedef struct { int tick,type,channel,param1,param2; } mev_t;
#define MAX_EV 16384
#define MAX_N 128

typedef struct {
    int note,channel,velocity,sid,slen,active,ic;
    float sp,si,eg,es,lp;
    int ls,le,loop;
} nt;

static nt N[MAX_N]; static int na=0;
static int chan_prog[16];
static float chan_vol[16];

// Same ADSR/LPF tables as i_android_music.c
static const float A[14][4]={
    {.002,.02,.5,.05},{.001,.03,.3,.06},{.01,.005,.9,.1},{.003,.015,.7,.04},
    {.005,.015,.7,.08},{.02,.01,.8,.06},{.01,.005,.9,.1},{.015,.01,.8,.06},
    {.005,.01,.8,.05},{.03,.01,.9,.1},{.002,.005,.5,.03},{.005,.01,.7,.05},
    {.001,.005,.2,.02},{.001,.003,.1,.01}};
static const float LP[14]={.3,.4,.5,.35,.15,.25,.45,.2,.6,.15,.4,.3,.5,.4};
#define OUT_RATE 44100

static int read_var_len(const uint8_t *b,int p,int *v){
    int vv=0; unsigned char c;
    do{c=b[p++];vv=(vv<<7)|(c&0x7F);}while(c&0x80);
    *v=vv;return p;
}

static int parse_midi(const uint8_t *m,int len,mev_t *ev,int max){
    if(memcmp(m,"MThd",4)!=0)return 0;
    int div=m[14]|(m[15]<<8),pos=22,tick=0,ne=0,status=0;
    while(pos<len-4&&ne<max){
        int d=0;pos=read_var_len(m,pos,&d);tick+=d;
        if(pos>=len)break;
        int by=m[pos++];
        if(by<0x80){pos--;if(!status)break;goto us;}
        status=by;
us:;     int tp=status&0xF0,ch=status&0x0F;
        if(tp==0x80||tp==0x90){
            if(pos+2>len||ne>=max)break;
            ev[ne].tick=tick;ev[ne].channel=ch;ev[ne].param1=m[pos];ev[ne].param2=m[pos+1];pos+=2;
            ev[ne].type=(tp==0x80||m[pos-2]==0)?0x80:0x90;ne++;
        }else if(tp==0xC0){
            if(pos+1>len||ne>=max)break;
            ev[ne].tick=tick;ev[ne].type=0xC0;ev[ne].channel=ch;ev[ne].param1=m[pos];pos++;ne++;
        }else if(tp==0xB0){
            if(pos+2>len||ne>=max)break;
            ev[ne].tick=tick;ev[ne].type=0xB0;ev[ne].channel=ch;
            ev[ne].param1=m[pos];ev[ne].param2=m[pos+1];pos+=2;ne++;
        }else if(status==0xFF){
            int mt=m[pos++],ml=0;pos=read_var_len(m,pos,&ml);
            if(mt==0x51&&ml==3&&ne<max){ev[ne].tick=tick;ev[ne].type=0xFF;
                ev[ne].param1=(m[pos]<<16)|(m[pos+1]<<8)|m[pos+2];pos+=3;ne++;}
            else if(mt==0x2F){pos+=ml;break;}else pos+=ml;
        }else pos+=2;
    }
    return ne;
}

static void note_on(int ch,int mn,int vel){
    // H5: FluidSynth-style polyphony — don't kill old notes, let CC7 fade them
    // Only kill on explicit vel=0 (handled separately)
    if(na>=MAX_N){
        // Voice stealing: kill the quietest note (lowest CC7 volume)
        int quietest=0;
        for(int i=1;i<na;i++){
            float v1 = N[i].channel<16 ? chan_vol[N[i].channel] : 1.0f;
            float v0 = N[quietest].channel<16 ? chan_vol[N[quietest].channel] : 1.0f;
            if(v1 < v0) quietest=i;
        }
        if(N[quietest].channel==ch && chan_vol[ch]<0.01f){
            N[quietest]=N[na-1];na--;return; // steal silent voice on same channel
        }
        N[na-1].active=0;na--;return; // fallback: drop oldest
    }
    int kl,kh;int si=sf2_get_sample(chan_prog[ch],mn,&kl,&kh);
    if(si<0)return;
    int16_t *d=sf2_get_sample_data(si);if(!d)return;
    nt*n=&N[na++];sf2_sample_t*s=&sf2_samples[si];
    n->note=mn;n->channel=ch;n->sid=si;n->slen=s->end-s->start;n->sp=0;
    n->si=powf(2,(mn-s->orig_key)/12.0f)*((float)s->sample_rate/OUT_RATE);
    n->ls=s->loop_start-s->start;n->le=s->loop_end-s->start;
    n->loop=(n->le>n->ls);
    n->eg=1.0f;n->ic=chan_prog[ch]%14;n->es=0;n->lp=0;n->active=1;
}
static void note_off(int ch,int mn){
    // Kill specific note on specific channel (FluidSynth-style)
    for(int i=0;i<na;i++){
        nt*n=&N[i];
        if(n->channel==ch&&n->note==mn&&n->active){
            n->active=0;break;
        }
    }
}

int main(){
    // Load MUS
    FILE*mf=fopen("/tmp/e1m2.mus","rb");if(!mf){perror("mus");return 1;}
    fseek(mf,0,SEEK_END);int msize=ftell(mf);fseek(mf,0,SEEK_SET);
    uint8_t*mus=(uint8_t*)malloc(msize);fread(mus,1,msize,mf);fclose(mf);

    // Read MUS header instrument assignments for channel initialization
    // Header: magic(4) + songlen(2) + songstart(2) + numchans(2) + numsec(2) + numinst(2) + instruments[numinst*2]
    uint16_t mus_numchans = mus[8]|(mus[9]<<8);
    uint16_t mus_numinst = mus[12]|(mus[13]<<8);
    // Init channel programs from MUS header instrument table
    for(int i=0;i<16;i++){chan_prog[i]=0;chan_vol[i]=1.0f;}
    for(int i=0;i<mus_numinst&&i<16;i++){
        uint16_t inst = mus[16+i*2]|(mus[17+i*2]<<8);
        // DOOM instrument numbers: if >127, strip bank bit (128=MSB bank)
        if(inst > 127) inst &= 0x7F;
        // MUS channel -> MIDI channel mapping (same as convertToMidi)
        int midi_ch = i;
        if(midi_ch == 15) midi_ch = 9;
        else if(midi_ch >= 9) midi_ch++;
        chan_prog[midi_ch] = inst;
    }
    printf("MUS header: %d chans, %d instruments\n", mus_numchans, mus_numinst);
    // Channel 9 (MUS ch15) has no header entry — DOOM uses it for the main lead.
    // Assign a synth lead instrument (prog 73 = Lead 5/square in GM).
    if(!chan_prog[9]) chan_prog[9] = 73;
    printf("Channel programs: ");
    for(int i=0;i<16;i++) if(chan_prog[i]>0||i<mus_numchans) printf("ch%d=%d ",i,chan_prog[i]);
    printf("\n");

    // Load SF2
    if(sf2_load("/Users/jack/Downloads/SC-55.SF2")){printf("SF2 fail\n");return 1;}
    printf("SF2: %d samples\n",sf2_num_samples);

    // MUS -> MIDI
    void*mbuf=0;if(!convertToMidi(mus,&mbuf)){printf("convert fail\n");return 1;}free(mus);
    uint8_t*m=(uint8_t*)mbuf;
    int mlen=(m[18]<<24)|(m[19]<<16)|(m[20]<<8)|m[21];
    int div=(m[12]<<8)|m[13];

    // Parse events
    mev_t ev[MAX_EV];int nev=parse_midi(m,22+mlen,ev,MAX_EV);free(mbuf);
    printf("Events: %d\n",nev);

    // === PROBE A: dump first 200 MIDI events ===
    printf("\n=== FIRST 200 EVENTS ===\n");
    int note_on_count=0,note_off_count=0,pgm_count=0,cc_count=0;
    int ch_events[16]={0};
    for(int i=0;i<nev&&i<200;i++){
        const mev_t*e=&ev[i];
        if(e->type==0x90){printf("[%4d] ch%d NOTEON  note=%3d vel=%3d\n",e->tick,e->channel,e->param1,e->param2);note_on_count++;}
        else if(e->type==0x80){printf("[%4d] ch%d NOTEOFF note=%3d vel=%3d\n",e->tick,e->channel,e->param1,e->param2);note_off_count++;}
        else if(e->type==0xC0){printf("[%4d] ch%d PGM     prog=%3d\n",e->tick,e->channel,e->param1);pgm_count++;}
        else if(e->type==0xB0){printf("[%4d] ch%d CC      ctrl=%3d val=%3d\n",e->tick,e->channel,e->param1,e->param2);cc_count++;}
        else if(e->type==0xFF){printf("[%4d] TEMPO   %u us/qn\n",e->tick,e->param1);}
        if(e->channel<16) ch_events[e->channel]++;
    }
    printf("\n--- Summary (first 200) ---\n");
    printf("NOTEON: %d, NOTEOFF: %d, PGM: %d, CC: %d\n",note_on_count,note_off_count,pgm_count,cc_count);
    printf("Per-channel: ");
    for(int c=0;c<16;c++) if(ch_events[c]) printf("ch%d=%d ",c,ch_events[c]);
    printf("\n");

    // Full counts over ALL events
    note_on_count=note_off_count=pgm_count=cc_count=0;
    memset(ch_events,0,sizeof(ch_events));
    for(int i=0;i<nev;i++){
        if(ev[i].type==0x90) note_on_count++;
        else if(ev[i].type==0x80) note_off_count++;
        else if(ev[i].type==0xC0) pgm_count++;
        else if(ev[i].type==0xB0) cc_count++;
        if(ev[i].channel<16) ch_events[ev[i].channel]++;
    }
    printf("\n--- Summary (ALL %d events) ---\n",nev);
    printf("NOTEON: %d, NOTEOFF: %d, PGM: %d, CC: %d\n",note_on_count,note_off_count,pgm_count,cc_count);
    printf("Per-channel: ");
    for(int c=0;c<16;c++) if(ch_events[c]) printf("ch%d=%d ",c,ch_events[c]);
    printf("\n");
    // ==========================================


    int last_tick=0;for(int i=0;i<nev;i++)if(ev[i].tick>last_tick)last_tick=ev[i].tick;
    printf("Ticks: %d\n",last_tick);

    // Channel programs already set from MUS header above

    // Render
    int rate=OUT_RATE;int maxs=last_tick*2000+rate*5;
    int32_t*buf=(int32_t*)calloc(maxs,4);int wp=0;
    float tempo=500000,tf=0;int ce=0,ct=0;

    while(ce<nev&&wp<maxs){
        // Process events at current tick
        while(ce<nev&&ev[ce].tick==ct){
            if(ev[ce].type==0x90){
                if(ev[ce].param2==0) note_off(ev[ce].channel,ev[ce].param1);  // vel=0 = note-off
                else note_on(ev[ce].channel,ev[ce].param1,ev[ce].param2);
            }
            else if(ev[ce].type==0x80)note_off(ev[ce].channel,ev[ce].param1);
            else if(ev[ce].type==0xC0){
                // Try: map PGM changes through MUS header channel order
                // MUS header instruments: [6,37,48,51,81] for channels 0-4
                // MIDI PGM events: ch0→48, ch1→51, ch2→37, ch3→81, ch4→6
                // The PGM values match header instruments but on wrong channels.
                // Reverse-map: find which header slot this PGM value belongs to
                int hdr_inst[] = {6,37,48,51,81};
                int hdr_ch = -1;
                for(int h=0;h<5;h++) if(hdr_inst[h]==ev[ce].param1) hdr_ch=h;
                if(hdr_ch>=0) chan_prog[hdr_ch] = ev[ce].param1;
                else chan_prog[ev[ce].channel] = ev[ce].param1;
            }
            else if(ev[ce].type==0xFF)tempo=ev[ce].param1;
            else if(ev[ce].type==0xB0&&ev[ce].param1==7)
                chan_vol[ev[ce].channel]=ev[ce].param2/127.0f;
            ce++;
        }

        float spb=(tempo/div)*(rate/1e6);
        float tsf=spb+tf;int ts=(int)(tsf+.5f);tf=tsf-ts;if(ts<1)ts=1;
        if(wp+ts>maxs)ts=maxs-wp;

        for(int s=0;s<ts&&wp<maxs;s++){
            float sum=0;
            for(int i=0;i<na;i++){
                nt*n=&N[i];if(!n->active)continue;int ic=n->ic;
                // H1: ADSR disabled — DOOM uses CC7 for envelope, eg stays at 1.0

                // Sample playback with interpolation + looping
                int sm=0;
                if(n->sid>=0&&n->slen>0){
                    int16_t*d=sf2_get_sample_data(n->sid);if(d){
                        float p=n->sp;int ix=(int)p;float fr=p-ix;
                        if(n->loop&&p>=n->le){p=n->ls+(int)p-n->le;ix=(int)p;fr=p-ix;}
                        if(ix>=n->slen-1){if(n->loop)ix=n->ls;else{n->active=0;continue;}}
                        int nx=ix+1;if(nx>=n->slen)nx=n->slen-1;
                        sm=(int)(d[ix]+(d[nx]-d[ix])*fr);n->sp+=n->si;
                    }
                }

                // LPF + channel volume + envelope + master gain
                float fl=n->lp+LP[ic]*((float)sm-n->lp);n->lp=fl;
                float cv=n->channel<16?chan_vol[n->channel]:1.0f;
                sum+=fl*n->eg*cv*0.5f;  // match test_scale gain
            }
            // Soft-clip (matching i_android_sound.c)
            float ax=sum<0?-sum:sum;
            if(ax>32767)ax=ax/(1+ax/32767*.5f);
            // Hard-clamp to int16 range (production code does this, prevents wrap)
            if(ax>32767) ax=32767;
            buf[wp++]=(sum<0)?-ax:ax;
        }

        // Cleanup dead notes
        int j=na-1;for(int i=0;i<=j;i++){if(!N[i].active){if(i!=j)N[i]=N[j];j--;i--;}}
        na=j+1;ct++;
    }
    printf("Rendered %d samples (%.1fs)\n",wp,(double)wp/rate);

    // Write WAV
    FILE*wf=fopen("/tmp/e1m2_rendered.wav","wb");if(!wf){perror("wav");return 1;}
    int ds=wp*4; // int32 -> will convert to int16
    // Write as 16-bit WAV
    int16_t *wav16=(int16_t*)malloc(wp*2);
    for(int i=0;i<wp;i++) wav16[i]=(int16_t)buf[i];
    
    int dsz=wp*2,wsz=44+dsz;
    unsigned char*h=(unsigned char*)malloc(wsz);
    memcpy(h,"RIFF",4);*(uint32_t*)(h+4)=wsz-8;memcpy(h+8,"WAVE",4);
    memcpy(h+12,"fmt ",4);*(uint32_t*)(h+16)=16;*(uint16_t*)(h+20)=1;*(uint16_t*)(h+22)=1;
    *(uint32_t*)(h+24)=rate;*(uint32_t*)(h+28)=rate*2;*(uint16_t*)(h+32)=2;*(uint16_t*)(h+34)=16;
    memcpy(h+36,"data",4);*(uint32_t*)(h+40)=dsz;memcpy(h+44,wav16,dsz);
    fwrite(h,1,wsz,wf);fclose(wf);free(h);free(wav16);free(buf);
    printf("Wrote /tmp/e1m2_rendered.wav\n");
    sf2_free();return 0;
}
