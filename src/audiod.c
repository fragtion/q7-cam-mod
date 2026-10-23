/* audiod — live audio from the Q7 mic over HTTP.
 * Captures /dev/snd/pcmC0D0c (S16_LE mono 8000 Hz) continuously and streams it as
 * a WAV to any number of HTTP clients. No libasound (raw SNDRV_PCM ioctls), static.
 *
 *   GET /          live WAV stream  (audio/wav; point a browser or HA at it)
 *   GET /raw       raw S16LE 8kHz mono PCM stream
 * Sound detection (for camd): every 250 ms the peak level (dBFS) is compared with the
 * trigger level from /tmp/.sound_cfg ("enabled db hold_s", written by camd) and the
 * result is written to /tmp/.sound as "active peak_db enabled".
 * Usage: audiod [-p 8081]
 * Build: arm-linux-musleabi-gcc -static -no-pie -fno-pie -march=armv5te -O2 -o audiod audiod.c
 * Safe to run alongside piggy; it only touches the (now free) audio device.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct snd_interval { unsigned int min, max; unsigned int openmin:1,openmax:1,integer:1,empty:1; };
struct snd_mask { uint32_t bits[8]; };
struct snd_pcm_hw_params {
    unsigned int flags; struct snd_mask masks[3]; struct snd_mask mres[5];
    struct snd_interval intervals[12]; struct snd_interval ires[9];
    unsigned int rmask,cmask,info,msbits,rate_num,rate_den; unsigned long fifo_size;
    unsigned char reserved[64];
};
struct snd_pcm_sw_params {
    int tstamp_mode; unsigned int period_step, sleep_min;
    unsigned long avail_min,xfer_align,start_threshold,stop_threshold,silence_threshold,silence_size,boundary;
    unsigned int proto,tstamp_type; unsigned char reserved[56];
};
struct snd_xferi { long result; void *buf; unsigned long frames; };
#define SNDRV_PCM_IOCTL_HW_PARAMS   _IOWR('A',0x11,struct snd_pcm_hw_params)
#define SNDRV_PCM_IOCTL_SW_PARAMS   _IOWR('A',0x13,struct snd_pcm_sw_params)
#define SNDRV_PCM_IOCTL_PREPARE     _IO('A',0x40)
#define SNDRV_PCM_IOCTL_START       _IO('A',0x42)
#define SNDRV_PCM_IOCTL_READI_FRAMES _IOR('A',0x51,struct snd_xferi)
#define IV(x) ((x)-8)
static void mask_set(struct snd_mask*m,unsigned v){ memset(m,0,sizeof*m); m->bits[v>>5]|=(1u<<(v&31)); }
static void iv_set(struct snd_interval*i,unsigned v){ i->min=i->max=v; i->integer=1; i->openmin=i->openmax=i->empty=0; }
static void iv_any(struct snd_interval*i){ i->min=0; i->max=~0u; i->integer=0; }

static unsigned g_rate=8000, g_ch=1;

/* configure the ADDA codec mixer so the ADC actually hears the mic. These controls
 * are normally set by the vendor app; we set them directly on /dev/snd/controlC0.
 *   numid 5 "ADC Capture Route" -> 6 (mic),  numid 3 "Mic Capture Volume" -> 7 (max) */
struct snd_ctl_elem_id2 { unsigned int numid; int iface; unsigned int device,subdevice; unsigned char name[44]; unsigned int index; };
struct snd_ctl_elem_value2 {
    struct snd_ctl_elem_id2 id; unsigned int indirect:1;
    union { long integer_value[128]; long long i64[64]; unsigned int en[128]; unsigned char data[512]; } value;
    long tstamp[2]; unsigned char reserved[120];
};
#define CTL_ELEM_WRITE _IOWR('U',0x13,struct snd_ctl_elem_value2)
static void mixer_setup(void){
    int fd=open("/dev/snd/controlC0",O_RDWR); if(fd<0){perror("controlC0");return;}
    unsigned numid[2]={5,3}; long val[2]={6,7};
    for(int k=0;k<2;k++){
        struct snd_ctl_elem_value2 ev; memset(&ev,0,sizeof ev);
        ev.id.numid=numid[k]; ev.value.integer_value[0]=val[k];
        if(ioctl(fd,CTL_ELEM_WRITE,&ev)<0) fprintf(stderr,"mixer numid %u set fail\n",numid[k]);
        else fprintf(stderr,"mixer numid %u = %ld\n",numid[k],val[k]);
    }
    close(fd);
}

static int pcm_open(unsigned period){
    int fd=open("/dev/snd/pcmC0D0c",O_RDWR); if(fd<0){perror("open pcm");return -1;}
    struct snd_pcm_hw_params hw; memset(&hw,0,sizeof hw);
    for(int i=0;i<3;i++){hw.masks[i].bits[0]=~0u;hw.masks[i].bits[1]=~0u;}
    for(int i=0;i<12;i++) iv_any(&hw.intervals[i]);
    hw.rmask=~0u; hw.info=~0u;
    mask_set(&hw.masks[0],3);              /* ACCESS_RW_INTERLEAVED */
    mask_set(&hw.masks[1],2);              /* FORMAT_S16_LE */
    iv_set(&hw.intervals[IV(10)],g_ch);    /* CHANNELS */
    iv_set(&hw.intervals[IV(11)],g_rate);  /* RATE */
    iv_set(&hw.intervals[IV(13)],period);  /* PERIOD_SIZE */
    iv_set(&hw.intervals[IV(15)],2);       /* PERIODS (2 = lower latency) */
    if(ioctl(fd,SNDRV_PCM_IOCTL_HW_PARAMS,&hw)<0){perror("HW_PARAMS");close(fd);return -1;}
    g_rate=hw.intervals[IV(11)].min; g_ch=hw.intervals[IV(10)].min;
    struct snd_pcm_sw_params sw; memset(&sw,0,sizeof sw);
    sw.avail_min=period; sw.start_threshold=1; sw.stop_threshold=~0ul; sw.boundary=0x7fffffff; sw.xfer_align=1;
    ioctl(fd,SNDRV_PCM_IOCTL_SW_PARAMS,&sw);
    if(ioctl(fd,SNDRV_PCM_IOCTL_PREPARE,0)<0){perror("PREPARE");close(fd);return -1;}
    ioctl(fd,SNDRV_PCM_IOCTL_START,0);
    fprintf(stderr,"pcm: %u Hz, %u ch, period %u\n",g_rate,g_ch,period);
    return fd;
}

static void wav_hdr(unsigned char*h,unsigned rate,unsigned ch){
    unsigned br=rate*ch*2, data=0x7fffffff, riff=0x7fffffff;
    memset(h,0,44); memcpy(h,"RIFF",4); h[4]=riff;h[5]=riff>>8;h[6]=riff>>16;h[7]=riff>>24;
    memcpy(h+8,"WAVEfmt ",8); h[16]=16; h[20]=1; h[22]=ch; h[24]=rate;h[25]=rate>>8;h[26]=rate>>16;
    h[28]=br;h[29]=br>>8;h[30]=br>>16; h[32]=ch*2; h[34]=16; memcpy(h+36,"data",4);
    h[40]=data;h[41]=data>>8;h[42]=data>>16;h[43]=data>>24;
}

/* ---- sound level / trigger ---- */
static int snd_enabled=1, snd_db=-30, snd_hold=5;
static void sound_cfg_read(void){
    FILE*f=fopen("/tmp/.sound_cfg","r"); if(!f) return;
    int e,d,h; if(fscanf(f,"%d %d %d",&e,&d,&h)==3){snd_enabled=e;snd_db=d;snd_hold=h;}
    fclose(f);
}
static void sound_update(const short*b,long n){
    static int peak=0; static long acc=0; static time_t last_loud=0; static int cfg_tick=0;
    for(long i=0;i<n;i++){int v=b[i]<0?-b[i]:b[i]; if(v>peak)peak=v;}
    acc+=n; if(acc<(long)g_rate/4) return;                     /* every 250 ms */
    acc=0;
    if(++cfg_tick>=8){cfg_tick=0;sound_cfg_read();}             /* re-read settings every 2 s */
    int db=peak>0?(int)lround(20*log10(peak/32768.0)):-96;
    peak=0;
    time_t now=time(0);
    if(snd_enabled&&db>=snd_db) last_loud=now;
    int active=snd_enabled&&last_loud&&now-last_loud<snd_hold;
    FILE*f=fopen("/tmp/.sound.tmp","w");
    if(f){fprintf(f,"%d %d %d\n",active,db,snd_enabled);fclose(f);rename("/tmp/.sound.tmp","/tmp/.sound");}
}

#define MAXC 16
int main(int argc,char**argv){
    int port=8081, period=256;
    for(int i=1;i<argc;i++) if(!strcmp(argv[i],"-p")&&i+1<argc) port=atoi(argv[++i]);
    signal(SIGPIPE,SIG_IGN);
    mixer_setup();                         /* route ADC to mic + set gain */
    sound_cfg_read();
    int pcm=pcm_open(period); if(pcm<0) return 1;

    int s=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons(port);
    if(bind(s,(void*)&a,sizeof a)<0){perror("bind");return 1;} listen(s,8);
    fprintf(stderr,"audiod: http://0.0.0.0:%d/ (WAV) and /raw (PCM)\n",port);

    int cfd[MAXC]; char craw[MAXC]; for(int i=0;i<MAXC;i++) cfd[i]=-1;
    short *buf=malloc(period*2*2);
    unsigned char wh[44]; wav_hdr(wh,g_rate,g_ch);

    for(;;){
        /* capture one period (blocks ~period/rate seconds) */
        struct snd_xferi x; x.buf=buf; x.frames=period; x.result=0;
        if(ioctl(pcm,SNDRV_PCM_IOCTL_READI_FRAMES,&x)<0){
            if(errno==EPIPE||errno==ESTRPIPE){ ioctl(pcm,SNDRV_PCM_IOCTL_PREPARE,0); ioctl(pcm,SNDRV_PCM_IOCTL_START,0); continue; }
            perror("READI"); usleep(5000); continue;
        }
        long n=x.result>0?x.result:period; int bytes=n*g_ch*2;
        sound_update(buf,n*g_ch);
        for(int i=0;i<MAXC;i++) if(cfd[i]>=0){
            if(send(cfd[i],buf,bytes,MSG_NOSIGNAL|MSG_DONTWAIT)<0 && errno!=EAGAIN){ close(cfd[i]); cfd[i]=-1; }
        }
        /* accept new clients (non-blocking) */
        struct pollfd p={s,POLLIN,0};
        if(poll(&p,1,0)>0){
            int c=accept(s,0,0);
            if(c>=0){
                char req[512]; int r=recv(c,req,sizeof req-1,0); if(r>0)req[r]=0; else req[0]=0;
                int raw=strstr(req,"GET /raw")!=0;
                const char*hdr=raw?
                    "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/octet-stream\r\n\r\n":
                    "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: audio/wav\r\n\r\n";
                /* modest send buffer: enough to ride out bursty readers without dropping
                 * periods (the client caps its own latency), but not a multi-second backlog */
                int sb=16384; setsockopt(c,SOL_SOCKET,SO_SNDBUF,&sb,sizeof sb);
                send(c,hdr,strlen(hdr),MSG_NOSIGNAL);
                if(!raw) send(c,wh,44,MSG_NOSIGNAL);
                int slot=-1; for(int i=0;i<MAXC;i++) if(cfd[i]<0){slot=i;break;}
                if(slot<0){ close(c); } else { fcntl(c,F_SETFL,O_NONBLOCK); cfd[slot]=c; craw[slot]=raw; }
            }
        }
    }
}
