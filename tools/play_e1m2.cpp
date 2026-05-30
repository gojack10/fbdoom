// play_e1m2.cpp — Read E1M2 MUS, convert to MIDI, render via SF2 → WAV
// g++ -o play_e1m2 play_e1m2.cpp ../src/device/sf2_loader.c ../src/i_mus_convert.cpp ../src/mus2midi.cpp -I../src -std=c++03 -fno-exceptions -lm
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>

#include "device/sf2_loader.h"

extern "C" { bool convertToMidi(void *musData, void **midiOutput); }

typedef struct {
    int tick, type, channel, param1, param2;
} mev_t;

static int read_var_len(const uint8_t *b, int p, int *v) {
    int vv=0; unsigned char c;
    do { c=b[p++]; vv=(vv<<7)|(c&0x7F); } while(c&0x80);
    *v=vv; return p;
}

static int parse_midi(const uint8_t *m, int len, mev_t *ev, int max) {
    if(memcmp(m,"MThd",4)!=0) return 0;
    int div=m[14]|(m[15]<<8), pos=22, tick=0, ne=0, status=0;
    while(pos<len-4 && ne<max) {
        int d=0; pos=read_var_len(m,pos,&d); tick+=d;
        if(pos>=len) break;
        int by=m[pos++];
        if(by<0x80){pos--;if(!status)break;goto us;}
        status=by;
us:;     int tp=status&0xF0, ch=status&0x0F;
        if(tp==0x80||tp==0x90){
            if(pos+2>len||ne>=max)break;
            ev[ne].tick=tick;ev[ne].channel=ch;ev[ne].param1=m[pos];ev[ne].param2=m[pos+1];pos+=2;
            ev[ne].type=(tp==0x80||m[pos-2]==0)?0x80:0x90; ne++;
        } else if(tp==0xC0){
            if(pos+1>len||ne>=max)break;
            ev[ne].tick=tick;ev[ne].type=0xC0;ev[ne].channel=ch;ev[ne].param1=m[pos];pos++;ne++;
        } else if(status==0xFF){
            int mt=m[pos++], ml=0; pos=read_var_len(m,pos,&ml);
            if(mt==0x51&&ml==3&&ne<max){ev[ne].tick=tick;ev[ne].type=0xFF;ev[ne].param1=(m[pos]<<16)|(m[pos+1]<<8)|m[pos+2];pos+=3;ne++;}
            else if(mt==0x2F){pos+=ml;break;}
            else pos+=ml;
        } else if(tp==0xB0){
            if(pos+2>len)break;
            if(ne<max){ev[ne].tick=tick;ev[ne].type=0xB0;ev[ne].channel=ch;ev[ne].param1=m[pos];ev[ne].param2=m[pos+1];ne++;}
            pos+=2;
        } else pos+=2;
    }
    return ne;
}

#define MAX_N 64
typedef struct{int n,sid,slen,ls,le,loop,active,ic,ch;float sp,si,eg,es,lp;} nt;
static nt N[MAX_N]; static int na=0, chan_prog[16]={0};
static float chan_vol[16];
static const float A[14][4]={{.002,.02,.5,.05},{.001,.03,.3,.06},{.01,.005,.9,.1},{.003,.015,.7,.04},
    {.005,.015,.7,.08},{.02,.01,.8,.06},{.01,.005,.9,.1},{.015,.01,.8,.06},
    {.005,.01,.8,.05},{.03,.01,.9,.1},{.002,.005,.5,.03},{.005,.01,.7,.05},
    {.001,.005,.2,.02},{.001,.003,.1,.01}};
static const float LP[14]={.3,.4,.5,.35,.15,.25,.45,.2,.6,.15,.4,.3,.5,.4};

static void non(int ch,int mn,int v){
    if(na>=MAX_N)return;
    int kl,kh;int si=sf2_get_sample(chan_prog[ch],mn,&kl,&kh);
    if(si<0)return;int16_t*d=sf2_get_sample_data(si);if(!d)return;
    nt*n=&N[na++];sf2_sample_t*s=&sf2_samples[si];
    n->n=mn;n->sid=si;n->slen=s->end-s->start;n->sp=0;
    n->si=powf(2,(mn-s->orig_key)/12.0f)*(s->sample_rate/44100.0f);
    n->ls=s->loop_start-s->start;n->le=s->loop_end-s->start;
    n->loop=(n->le>n->ls);n->eg=0;n->ic=chan_prog[ch]%14;n->ch=ch;
    n->es=1.0f/(A[n->ic][0]*44100);n->lp=0;n->active=1;
}
static void noff(int mn){
    for(int i=0;i<na;i++)if(N[i].n==mn&&N[i].active){N[i].active=0;break;}
}

int main(){
    // Load MUS
    FILE*mf=fopen("/tmp/e1m2.mus","rb");if(!mf){perror("mus");return 1;}
    fseek(mf,0,SEEK_END);int msize=ftell(mf);fseek(mf,0,SEEK_SET);
    uint8_t*mus=(uint8_t*)malloc(msize);fread(mus,1,msize,mf);fclose(mf);
    printf("MUS: %d bytes\n",msize);

    // Load SF2
    if(sf2_load("/Users/jack/Downloads/SC-55.SF2")){printf("SF2 fail\n");return 1;}
    printf("SF2: %d samples\n",sf2_num_samples);

    // MUS -> MIDI
    void*mbuf=0;if(!convertToMidi(mus,&mbuf)){printf("convert fail\n");return 1;}free(mus);
    uint8_t*m=(uint8_t*)mbuf;
    int mlen=(m[18]<<24)|(m[19]<<16)|(m[20]<<8)|m[21];
    int div=(m[12]<<8)|m[13];
    printf("MIDI: %d bytes div=%d\n",mlen,div);

    // Parse events
    mev_t ev[8192];int nev=parse_midi(m,22+mlen,ev,8192);free(mbuf);
    printf("Events: %d\n",nev);

    int last_tick=0;for(int i=0;i<nev;i++)if(ev[i].tick>last_tick)last_tick=ev[i].tick;
    printf("Ticks: %d\n",last_tick);

    // Render to WAV
    int rate=44100;int maxs=last_tick*2000;
    int16_t*wav=(int16_t*)malloc(maxs*2);int wp=0;
    float tempo=500000,tf=0;int ce=0,ct=0;
    for(int i=0;i<16;i++) chan_vol[i]=1.0f;

    while(ce<nev&&wp<maxs){
        while(ce<nev&&ev[ce].tick==ct){
            if(ev[ce].type==0x90)non(ev[ce].channel,ev[ce].param1,ev[ce].param2);
            else if(ev[ce].type==0x80)noff(ev[ce].param1);
            else if(ev[ce].type==0xC0)chan_prog[ev[ce].channel]=ev[ce].param1;
            else if(ev[ce].type==0xFF)tempo=ev[ce].param1;
            else if(ev[ce].type==0xB0&&ev[ce].param1==7)chan_vol[ev[ce].channel]=ev[ce].param2/127.0f;
            ce++;
        }
        float spb=(tempo/div)*(rate/1e6);
        float tsf=spb+tf;int ts=(int)(tsf+.5f);tf=tsf-ts;if(ts<1)ts=1;
        for(int s=0;s<ts&&wp<maxs;s++){
            int32_t sum=0;
            for(int i=0;i<na;i++){
                nt*n=&N[i];if(!n->active)continue;int ic=n->ic;
                // NO ADSR, NO LPF — raw sample playback to isolate the problem
                int sm=0;
                if(n->sid>=0&&n->slen>0){
                    int16_t*d=sf2_get_sample_data(n->sid);if(d){
                        float p=n->sp;int ix=(int)p;float fr=p-ix;
                        if(n->loop&&p>=n->le){p=n->ls+(int)p-n->le;ix=(int)p;fr=p-ix;}
                        if(ix>=n->slen-1){if(n->loop)ix=n->ls;else{n->active=0;break;}}
                        int nx=ix+1;if(nx>=n->slen)nx=n->slen-1;
                        sm=(int)(d[ix]+(d[nx]-d[ix])*fr);n->sp+=n->si;
                    }
                }
                // Raw sample, no filter: fl = sm
                // No envelope: eg = 1.0 always
                float cv=n->ch<16?chan_vol[n->ch]:1.0f;
                sum+=(int)(sm*cv*.3f);
            }
            float x=(float)sum*.25f;float ax=x<0?-x:x;
            if(ax>32767)ax=ax/(1+ax/32767*.5f);
            wav[wp++]=(int16_t)(x<0?-ax:ax);
        }
        int j=na-1;for(int i=0;i<=j;i++)if(!N[i].active){if(i!=j)N[i]=N[j];j--;i--;}
        na=j+1;ct++;
    }
    printf("Rendered %d samples (%.1fs)\n",wp,(double)wp/rate);

    // Write WAV
    FILE*wf=fopen("/tmp/e1m2_sf2.wav","wb");if(!wf){perror("wav");return 1;}
    int ds=wp*2;fwrite("RIFF",4,1,wf);uint32_t r=36+ds;fwrite(&r,4,1,wf);
    fwrite("WAVEfmt ",8,1,wf);uint32_t fs=16;fwrite(&fs,4,1,wf);
    uint16_t af=1;fwrite(&af,2,1,wf);uint16_t ch=1;fwrite(&ch,2,1,wf);
    uint32_t sr=rate;fwrite(&sr,4,1,wf);uint32_t br=rate*2;fwrite(&br,4,1,wf);
    uint16_t ba=2;fwrite(&ba,2,1,wf);uint16_t bps=16;fwrite(&bps,2,1,wf);
    fwrite("data",4,1,wf);fwrite(&ds,4,1,wf);fwrite(wav,2,wp,wf);fclose(wf);
    printf("Wrote /tmp/e1m2_sf2.wav\n");
    system("afplay /tmp/e1m2_sf2.wav");
    free(wav);sf2_free();return 0;
}