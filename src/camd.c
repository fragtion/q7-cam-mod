/* camd — the Q7 camera daemon. Replaces the vendor LinuxIP.exe (and piggy_hw):
 *
 *   capture   own V4L2 bring-up of /dev/video0, replayed from LinuxIP's boot trace;
 *             320x240 I420 (640x480 hangs the driver)
 *   video     hardware JPEG (libakimage) straight from the capture buffer (pmem, no copy),
 *             one encoder, shared ring -> MJPEG stream / snapshot for any number of viewers
 *   analysis  per frame: brightness (mean luma) and motion (changed blocks of a 40x30 grid)
 *   IR        night vision Off / On / Auto (brightness with hold time + minimum dwell)
 *   record    to the SD card (MJPEG .avi): motion clips with pre/post roll, or continuous
 *             (MODE button / /record); oldest deleted when space runs low; works without Wi-Fi
 *   config    /mnt/mmc/q7mod/q7cam.ini (key=value), live-editable via /settings and the web UI
 *   push      JSON events + heartbeat to a Home Assistant webhook
 *   sound     level/trigger computed by audiod, shared via /tmp/.sound(_cfg)
 *   watchdog  reboots the board when the picture freezes (auto_reboot)
 *
 * HTTP (default port 8080):
 *   /  /video  /stream     MJPEG stream          /snapshot  /shot.jpg   single JPEG
 *   /status.json           state + stats          /settings[?k=v&...][&save=0]  get/set settings
 *   /ui                    web UI                 /recordings  /rec/NAME[?delete=1]
 *   /record?mode=continuous|motion|off            /reboot?confirm=1
 *   /fs?path=...           SD card file manager   /play?path=CLIP.avi   play a clip as MJPEG
 *   /night /day /irstate /irled_on /irled_off
 *
 * Build: src/build.sh camd (Bootlin glibc toolchain, links the vendor libakimage + libakuio
 * from src/libs; needs only GLIBC_2.4 on the device).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <linux/input.h>

/* ---- vendor libs ---------------------------------------------------------------------- */
extern int SetImageChipID(int id);
extern int Img_SetCallbackFuns(void *cbs);
extern int Img_YUV2JPEG(void *Y, void *U, void *V, void *out, unsigned *size, int w, int h, int q);
extern int   akuio_pmem_init(void);
extern void *akuio_alloc_pmem(unsigned size);
extern void  akuio_free_pmem(void *p);
extern unsigned akuio_vaddr2paddr(void *p);
extern void *akuio_map_regs(unsigned phys, unsigned size);
extern void  akuio_unmap_regs(void *v, unsigned size);
extern int   akuio_sysreg_write(unsigned a, unsigned b, unsigned c);
extern int   akuio_lock_block(int hw);
extern int   akuio_unlock(int hw);

#define Q7MOD      "/mnt/mmc/q7mod"   /* our files; the card root keeps only boot.sh, wifi.txt, RECORD */
#define REC_DIR    "/mnt/mmc/RECORD"
/* q7cam.ini lives in q7mod/ (falls back to the card root on an old layout; see paths_init) */
static char conf_path[96] = "/mnt/mmc/q7cam.ini";
#define SOUND_FILE "/tmp/.sound"
#define SOUND_CFG  "/tmp/.sound_cfg"
#define VERSION    "camd 1.0"

/* clock_gettime would need GLIBC_2.17 (the camera has 2.10), so use the raw syscall for a
 * monotonic clock (immune to the NTP step at boot) and gettimeofday for wall time. */
#include <sys/syscall.h>
static double now_s(void) { struct timespec t; syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void abs_deadline(struct timespec *ts, double secs) {
    struct timeval tv; gettimeofday(&tv, 0);
    long long ns = (long long)tv.tv_usec * 1000 + (long long)(secs * 1e9);
    ts->tv_sec = tv.tv_sec + ns / 1000000000; ts->tv_nsec = ns % 1000000000;
}
/* debugging switches (environment): CAMD_SYNC=1 fsync every log line (log on the SD card
 * survives a crash), CAMD_NOIR=1 never touch the IR GPIOs, CAMD_NOJPEG=1 never encode */
static int dbg_sync, dbg_noir, dbg_nojpeg;
static void logm(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    struct stat st;                  /* the log is in /tmp (RAM): start over past 128 KB */
    if (!fstat(2, &st) && S_ISREG(st.st_mode) && st.st_size > 128 * 1024 && ftruncate(2, 0)) {}
    time_t t = time(0); struct tm tm; localtime_r(&t, &tm);
    char ts[24]; strftime(ts, sizeof ts, "%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] ", ts); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    if (dbg_sync) fsync(2);
}

/* ========================================================================================
 * Configuration
 * ====================================================================================== */
struct cfg {
    int width, height, quality, stream_fps, idle_fps, capture_fps;
    int sensor_brightness, sensor_contrast;
    char ir_mode[8];                 /* auto | on | off */
    int ir_on_below, ir_off_above, ir_off_margin, ir_hold_s, ir_min_dwell_s, night_mono;
    int motion_enabled, motion_blocks, motion_block_level, motion_frames, motion_hold_s, motion_ignore_light;
    int record_enabled, record_fps, record_pre_s, record_post_s, record_max_s, record_keep_free_mb;
    int sound_enabled, sound_db, sound_hold_s;
    char webhook[192];
    int heartbeat_s;
    char tz[32];
    char ntp_servers[96];
    int port;
    int auto_reboot;
};
static struct cfg C = {
    320, 240, 70, 15, 1, 10,
    3, 3,
    "auto", 25, 80, 40, 15, 60, 1,
    1, 6, 8, 3, 3, 1,
    0, 5, 2, 5, 120, 10,
    1, -20, 5,
    "",
    30,
    "UTC0",
    "pool.ntp.org,time.google.com",
    8080,
    1,
};
static pthread_mutex_t cfg_mu = PTHREAD_MUTEX_INITIALIZER;

enum { T_INT, T_STR };
struct setting {
    const char *key; int type; void *ptr; int min, max, size;
    int restart;                     /* needs a power-cycle to take effect */
    const char *help;
};
#define SI(k, f, lo, hi, r, h) { k, T_INT, &C.f, lo, hi, 0, r, h }
#define SS(k, f, r, h)         { k, T_STR, C.f, 0, 0, sizeof C.f, r, h }
static const struct setting SETTINGS[] = {
    /* 640x480 hangs the kernel camera driver right after STREAMON (tested with captest too) */
    SI("width", width, 160, 320, 1, "Capture width (max 320: VGA freezes the camera driver)"),
    SI("height", height, 120, 240, 1, "Capture height (max 240)"),
    SI("quality", quality, 20, 95, 0, "JPEG quality"),
    SI("stream_fps", stream_fps, 1, 25, 0, "Max MJPEG frame rate while someone is watching"),
    SI("idle_fps", idle_fps, 1, 5, 0, "Snapshot refresh rate when nobody is watching"),
    SI("capture_fps", capture_fps, 2, 25, 0, "Frames analysed per second when nobody is watching (motion/brightness)"),
    SI("sensor_brightness", sensor_brightness, 0, 6, 0, "Sensor brightness (LinuxIP used 3)"),
    SI("sensor_contrast", sensor_contrast, 0, 6, 0, "Sensor contrast (LinuxIP used 3)"),
    SS("ir_mode", ir_mode, 0, "Night vision: auto, on or off"),
    SI("ir_on_below", ir_on_below, 0, 255, 0, "Auto: IR on when brightness stays below this"),
    SI("ir_off_above", ir_off_above, 0, 255, 0, "Auto: IR off when brightness stays above this"),
    SI("ir_hold_s", ir_hold_s, 1, 600, 0, "Auto: seconds the condition must hold"),
    SI("ir_off_margin", ir_off_margin, 0, 255, 0, "Auto: IR off only when this much brighter than the IR-lit picture"),
    SI("ir_min_dwell_s", ir_min_dwell_s, 0, 3600, 0, "Auto: minimum seconds between switches"),
    SI("night_mono", night_mono, 0, 1, 0, "Black-and-white picture while IR is on (IR looks magenta in colour)"),
    SI("motion_enabled", motion_enabled, 0, 1, 0, "Motion detection on/off"),
    SI("motion_blocks", motion_blocks, 1, 600, 0, "Blocks (of 40x30) that must change for motion (higher = less sensitive)"),
    SI("motion_block_level", motion_block_level, 2, 80, 0, "Brightness change that counts a block as changed (higher = less sensitive)"),
    SI("motion_frames", motion_frames, 1, 30, 0, "Consecutive frames with enough changed blocks to trigger"),
    SI("motion_hold_s", motion_hold_s, 0, 60, 0, "Seconds motion stays on after the last trigger"),
    SI("motion_ignore_light", motion_ignore_light, 0, 1, 0, "Ignore sudden whole-picture brightness changes"),
    SI("record_enabled", record_enabled, 0, 1, 0, "Record motion clips to the SD card"),
    SI("record_fps", record_fps, 1, 15, 0, "Recording frame rate"),
    SI("record_pre_s", record_pre_s, 0, 5, 0, "Seconds kept from before the motion"),
    SI("record_post_s", record_post_s, 0, 60, 0, "Seconds recorded after motion ends"),
    SI("record_max_s", record_max_s, 10, 1800, 0, "Maximum clip length"),
    SI("record_keep_free_mb", record_keep_free_mb, 1, 4096, 0, "Delete oldest clips to keep this much free"),
    SI("sound_enabled", sound_enabled, 0, 1, 0, "Sound detection on/off"),
    SI("sound_db", sound_db, -90, 0, 0, "Sound trigger level, dBFS (higher = louder needed)"),
    SI("sound_hold_s", sound_hold_s, 1, 60, 0, "Seconds sound stays on after the last loud peak"),
    SS("webhook", webhook, 0, "Home Assistant webhook URL (empty = no push)"),
    SI("heartbeat_s", heartbeat_s, 5, 3600, 0, "Heartbeat interval to the webhook"),
    SS("tz", tz, 0, "Time zone (POSIX TZ, e.g. SAST-2)"),
    SS("ntp_servers", ntp_servers, 0, "NTP servers, comma-separated (empty = don't sync)"),
    SI("port", port, 1, 65535, 1, "HTTP port"),
    SI("auto_reboot", auto_reboot, 0, 1, 0, "Reboot by itself when the picture freezes or the sensor comes up stuck white (0 while working without the sensor)"),
};
#define NSET (sizeof SETTINGS / sizeof *SETTINGS)

static const struct setting *find_setting(const char *k) {
    for (unsigned i = 0; i < NSET; i++) if (!strcmp(SETTINGS[i].key, k)) return &SETTINGS[i];
    return 0;
}
/* parse+validate one value into the live config; returns 0 ok, -1 bad */
static int set_value(const struct setting *s, const char *v) {
    if (s->type == T_INT) {
        char *end; long x = strtol(v, &end, 10);
        if (end == v || *end || x < s->min || x > s->max) return -1;
        *(int *)s->ptr = (int)x;
    } else {
        if (strlen(v) >= (unsigned)s->size) return -1;
        if (!strcmp(s->key, "ir_mode") && strcmp(v, "auto") && strcmp(v, "on") && strcmp(v, "off")) return -1;
        if (!strcmp(s->key, "ntp_servers"))          /* goes on a command line: hostnames only */
            for (const char *q = v; *q; q++) if (!isalnum((unsigned char)*q) && !strchr(".-,:", *q)) return -1;
        strcpy((char *)s->ptr, v);
    }
    return 0;
}
static void load_config(void) {
    FILE *f = fopen(conf_path, "r");
    if (!f) { logm("no %s, using defaults", conf_path); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '[' || !*p) continue;
        char *eq = strchr(p, '='); if (!eq) continue;
        *eq = 0; char *k = p, *v = eq + 1;
        for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); ) *--e = 0;
        while (isspace((unsigned char)*v)) v++;
        for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); ) *--e = 0;
        const struct setting *s = find_setting(k);
        if (!s) logm("config: unknown key %s", k);
        else if (set_value(s, v)) logm("config: bad value %s=%s", k, v);
    }
    fclose(f);
}
/* Writes q7cam.ini only when its content would change (SD writes kept to a minimum: the
 * file is rendered in memory and compared with what's there first). */
static void save_config(void) {
    static char buf[8192], old[8192];
    size_t k = snprintf(buf, sizeof buf, "# Q7 camera settings (camd). Edit here, in the web UI (/ui) or via /settings?key=value.%c"
                        "# Settings marked [restart] need a power-cycle to take effect.%c%c", 10, 10, 10);
    for (unsigned i = 0; i < NSET && k < sizeof buf; i++) {
        const struct setting *s = &SETTINGS[i];
        k += snprintf(buf + k, sizeof buf - k, "# %s%s%c", s->help, s->restart ? " [restart]" : "", 10);
        if (s->type == T_INT) k += snprintf(buf + k, sizeof buf - k, "%s=%d%c", s->key, *(int *)s->ptr, 10);
        else k += snprintf(buf + k, sizeof buf - k, "%s=%s%c", s->key, (char *)s->ptr, 10);
    }
    if (k >= sizeof buf) { logm("config too large"); return; }
    FILE *f = fopen(conf_path, "r");
    if (f) { size_t n = fread(old, 1, sizeof old, f); fclose(f); if (n == k && !memcmp(old, buf, k)) return; }
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.tmp", conf_path);
    f = fopen(tmp, "w");
    if (!f) { logm("can't write %s: %s", tmp, strerror(errno)); return; }
    fwrite(buf, 1, k, f);
    fclose(f);
    rename(tmp, conf_path);
    sync();
}

/* ========================================================================================
 * GPIO: 14 sensor power-down (low = on); IR-cut coil 3/40; IR LEDs 47; 4 and 15 LinuxIP LEDs
 * ====================================================================================== */
#define GAS 0x40016631u
#define GDR 0x40016635u
#define GPE 0x40016632u
#define GLV 0x40016633u
static void akg2(unsigned cmd, int pin, int o1, int v1, int o2, int v2) {
    /* one fd for the life of the process, as LinuxIP does: the driver's release may undo
     * the pin setup, so an open/close per call doesn't leave the pin where we put it */
    static int fd = -1;
    if (fd < 0) fd = open("/dev/akgpio", O_RDWR);
    if (fd < 0) return;
    unsigned char s[16];
    /* the pull/config call takes 0x02 = "leave unchanged" in every field it shouldn't touch
     * (as LinuxIP sends it); zeros there reconfigure the pin's other settings, and doing
     * that froze/reset the board once the camera was streaming */
    memset(s, cmd == GPE ? 2 : 0, 16);
    s[0] = pin; s[1] = pin >> 8;
    if (o1 >= 0) s[o1] = v1;
    if (o2 >= 0) s[o2] = v2;
    ioctl(fd, cmd, s);
}
static void gpio_out(int pin, int lvl) {
    akg2(GAS, pin, -1, 0, -1, 0); akg2(GDR, pin, 8, 0, 9, 0); akg2(GPE, pin, 11, 0, -1, 0); akg2(GLV, pin, 10, lvl ? 1 : 0, -1, 0);
}
static void ircut(int night) { gpio_out(3, night ? 0 : 1); gpio_out(40, night ? 1 : 0); usleep(150000); gpio_out(3, 0); gpio_out(40, 0); }

/* ========================================================================================
 * Shared state
 * ====================================================================================== */
static struct {
    volatile int night;              /* IR currently on */
    volatile int brightness;         /* mean luma 0..255 */
    volatile int motion_score;       /* latest score */
    volatile int motion_peak;        /* max score since the last HTTP status read */
    volatile int push_peak;          /* max score since the last webhook push (own copy: the web
                                        page polls /status.json every second) */
    volatile int motion;             /* motion currently active */
    volatile int sound, sound_peak_db, sound_listening;
    volatile int sound_peak_http, sound_peak_push;  /* loudest since the last status read / push */
    volatile int recording;
    volatile int manual_rec;         /* continuous recording started with the button (or /record) */
    volatile int btn_motion;         /* motion recording armed with the button (or /record) */
    volatile unsigned frames, encodes;
    volatile double fps;
    volatile int viewers;
    char last_clip[64];
    double started;
} S;
static volatile int quit_flag, reboot_req;

/* ---- JPEG ring (one encoder, many readers) ---- */
#define NSLOT 3
#define SLOTMAX (160 * 1024)
struct ring { pthread_mutex_t mu; pthread_cond_t cv; uint32_t seq; uint32_t len[NSLOT]; double t[NSLOT]; uint8_t *slot[NSLOT]; };
static struct ring R = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };
static volatile int snapshot_wanted;

static void ring_publish(const uint8_t *jp, unsigned len) {
    if (len > SLOTMAX) return;
    pthread_mutex_lock(&R.mu);
    uint32_t s = R.seq + 1, i = s % NSLOT;
    memcpy(R.slot[i], jp, len); R.len[i] = len; R.t[i] = now_s();
    R.seq = s;
    pthread_cond_broadcast(&R.cv);
    pthread_mutex_unlock(&R.mu);
}
/* copy the newest frame newer than *last into dst; waits up to timeout_s. returns len or 0 */
static unsigned ring_get(uint32_t *last, uint8_t *dst, double timeout_s, double *age) {
    pthread_mutex_lock(&R.mu);
    double end = now_s() + timeout_s;
    while (R.seq == *last && !quit_flag) {
        double left = end - now_s(); if (left <= 0) break;
        struct timespec ts; abs_deadline(&ts, left);
        pthread_cond_timedwait(&R.cv, &R.mu, &ts);
    }
    unsigned len = 0;
    if (R.seq != *last && R.seq) {
        uint32_t i = R.seq % NSLOT; len = R.len[i];
        memcpy(dst, R.slot[i], len); *last = R.seq;
        if (age) *age = now_s() - R.t[i];
    }
    pthread_mutex_unlock(&R.mu);
    return len;
}

/* ========================================================================================
 * Capture + JPEG
 * ====================================================================================== */
#define NBUF 4
static int vfd = -1;
static uint8_t *cap_buf[NBUF];
static unsigned cap_len;
static uint8_t *jpeg_out;           /* pmem: the encoder writes here by DMA */
#define JPEG_OUT_MAX (192 * 1024)

static void *cb_malloc(unsigned s) { return malloc(s); }
static void *cb_free(void *p) { free(p); return p; }
static int cb_stub(void) { return 0; }
static void *cb_alloc_pmem(unsigned s) { return akuio_alloc_pmem(s); }
static void cb_free_pmem(void *p) { akuio_free_pmem(p); }
static unsigned long cb_v2p(void *v) { return v ? akuio_vaddr2paddr(v) : 0; }
static void *g_cb[27];
static void jpeg_init(void) {
    memset(g_cb, 0, sizeof g_cb);
    g_cb[0x1c / 4] = cb_malloc; g_cb[0x20 / 4] = cb_free; g_cb[0x44 / 4] = cb_stub;
    g_cb[0x4c / 4] = cb_alloc_pmem; g_cb[0x50 / 4] = cb_free_pmem; g_cb[0x54 / 4] = cb_v2p;
    g_cb[0x48 / 4] = akuio_sysreg_write; g_cb[0x58 / 4] = akuio_map_regs; g_cb[0x5c / 4] = akuio_unmap_regs;
    g_cb[0x60 / 4] = akuio_lock_block; g_cb[0x64 / 4] = akuio_unlock;
    SetImageChipID(4);
    if (Img_SetCallbackFuns(g_cb) != 0) logm("Img_SetCallbackFuns warning");
}
static uint8_t *grey_uv;            /* pmem: neutral chroma (128) for mono night frames */
static unsigned jpeg_encode(uint8_t *yuv, int q) {
    unsigned w = C.width, h = C.height, size = JPEG_OUT_MAX;
    uint8_t *u = yuv + w * h, *v = u + w * h / 4;
    if (S.night == 1 && C.night_mono && grey_uv) { u = grey_uv; v = grey_uv + w * h / 4; }
    int r = Img_YUV2JPEG(yuv, u, v, jpeg_out, &size, w, h, q);
    if (r < 0 || size < 4 || size > JPEG_OUT_MAX || jpeg_out[0] != 0xFF || jpeg_out[1] != 0xD8) return 0;
    return size;
}

static int vioctl(unsigned long req, void *arg, const char *name) {
    int r = ioctl(vfd, req, arg);
    if (r < 0) logm("%s failed: %s", name, strerror(errno));
    return r;
}
static void sensor_ctrl(unsigned id, int v) {
    struct v4l2_control c = { id, v };
    if (vfd >= 0) ioctl(vfd, VIDIOC_S_CTRL, &c);
}
/* LinuxIP's bring-up, replayed (see header) with its ordering and pacing. The one thing
 * that matters: pin 14 (pulled high at boot) is the sensor's power-down line and must be
 * driven low BEFORE /dev/video0 is opened, because the driver configures the sensor over
 * I2C on open. Without it STREAMON never yields a frame, and if pin 4 is then high the
 * board resets/freezes within seconds. Pin 4 is left low (as LinuxIP starts it).
 * Also as LinuxIP: a probe open/close of /dev/video0, pins parked before capture, and
 * ~0.3 s between setup ioctls.
 * Returns 0 on success. */
#define STEP() usleep(300000)
static int capture_start(void) {
    unsigned w = C.width, h = C.height;
    gpio_out(4, 0);                                  /* LinuxIP's first act: pin 4 an output, low */
    /* power-cycle the sensor: a reset via /reboot (or the pin-4 trick) restarts the SoC but
     * not the sensor, which then ignores the driver's setup (washed-out white picture) */
    gpio_out(14, 1);
    usleep(300000);
    gpio_out(14, 0);                                 /* sensor power-down off: sensor on */
    gpio_out(15, 0);
    usleep(700000);
    int probe = open("/dev/video0", O_RDWR | O_NONBLOCK);
    if (probe >= 0) close(probe);
    usleep(1000000);
    if (!dbg_noir) { gpio_out(40, 0); gpio_out(3, 0); gpio_out(47, 0); }  /* IR-cut idle, IR LEDs off */
    usleep(450000);
    vfd = open("/dev/video0", O_RDWR | O_NONBLOCK);
    if (vfd >= 0 && akuio_pmem_init() < 0) { logm("pmem init failed"); return -1; }
    if (vfd < 0) { logm("open /dev/video0: %s", strerror(errno)); return -1; }
    struct v4l2_capability cap; memset(&cap, 0, sizeof cap);
    if (vioctl(VIDIOC_QUERYCAP, &cap, "QUERYCAP")) return -1;
    STEP();
    struct v4l2_cropcap cc; memset(&cc, 0, sizeof cc); cc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vioctl(VIDIOC_CROPCAP, &cc, "CROPCAP");
    STEP();
    if (cc.bounds.width && (w > cc.bounds.width || h > cc.bounds.height)) { w = C.width = cc.bounds.width; h = C.height = cc.bounds.height; }
    struct v4l2_crop cr; memset(&cr, 0, sizeof cr); cr.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; cr.c.width = w; cr.c.height = h;
    vioctl(VIDIOC_S_CROP, &cr, "S_CROP");
    STEP();
    struct v4l2_format f; memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; f.fmt.pix.width = w; f.fmt.pix.height = h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; f.fmt.pix.field = V4L2_FIELD_INTERLACED;
    if (vioctl(VIDIOC_S_FMT, &f, "S_FMT")) return -1;
    STEP();
    cap_len = ((w * h * 2 + 0xfff) & ~0xfffu) + 0x1000;
    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
    rb.count = NBUF; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_USERPTR;
    if (vioctl(VIDIOC_REQBUFS, &rb, "REQBUFS")) return -1;
    struct v4l2_format pf; memset(&pf, 0, sizeof pf); pf.type = V4L2_BUF_TYPE_PRIVATE; pf.fmt.raw_data[0] = 1;
    pf.fmt.raw_data[36] = 1;                         /* LinuxIP also sets this word */
    vioctl(VIDIOC_S_FMT, &pf, "S_FMT(private)");
    STEP();
    sensor_ctrl(0x0098091f, 0);                      /* colour effect: none */
    sensor_ctrl(0x08000006, 0);                      /* private control, 0 as LinuxIP */
    sensor_ctrl(V4L2_CID_CONTRAST, C.sensor_contrast);
    sensor_ctrl(V4L2_CID_BRIGHTNESS, C.sensor_brightness);
    usleep(20000);
    for (int i = 0; i < NBUF; i++) {
        cap_buf[i] = akuio_alloc_pmem(cap_len);
        if (!cap_buf[i]) { logm("pmem alloc for capture buffer %d failed", i); return -1; }
        struct v4l2_buffer b; memset(&b, 0, sizeof b);
        b.index = i; b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_USERPTR;
        b.m.userptr = (unsigned long)cap_buf[i]; b.length = cap_len;
        if (vioctl(VIDIOC_QBUF, &b, "QBUF")) return -1;
    }
    jpeg_out = akuio_alloc_pmem(JPEG_OUT_MAX);
    grey_uv = akuio_alloc_pmem(w * h / 2);
    if (grey_uv) memset(grey_uv, 128, w * h / 2);
    if (!jpeg_out) { logm("pmem alloc for JPEG output failed"); return -1; }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (vioctl(VIDIOC_STREAMON, &type, "STREAMON")) return -1;
    logm("capture %ux%u, %d buffers of 0x%x (driver %s)", w, h, NBUF, cap_len, cap.driver);
    return 0;
}

/* Capture watchdog. Now and then (after ~1-7 h) the sensor/controller stops raising interrupts
 * and DQBUF returns EAGAIN forever while HTTP keeps serving the last JPEG. Restarting the stream
 * doesn't work: STREAMOFF then waits in the driver (videobuf_waiton, uninterruptible) for the
 * buffer that never completes, and the thread calling it is stuck for good. So the watchdog runs
 * in its own thread and reboots (setting auto_reboot; nothing is written to the SD card). */
static long camera_irqs(void) {                     /* the ak_camera line of /proc/interrupts */
    FILE *f = fopen("/proc/interrupts", "r");
    char line[128]; long n = -1;
    if (!f) return -1;
    while (fgets(line, sizeof line, f))
        if (strstr(line, "ak_camera")) { char *c = strchr(line, ':'); if (c) n = strtol(c + 1, 0, 10); break; }
    fclose(f);
    return n;
}
static volatile double last_frame_t;               /* set by the capture loop */
static volatile long irqs_at_frame = -1;
static volatile unsigned qbuf_fails;
#define STALL_S 10

/* ---- analysis ---- */
/* Motion: the picture is averaged into 40x30 blocks and motion is how many blocks changed, not
 * the average change over the whole picture (a person small in the frame barely moves an average).
 * A block counts when it changed by more than motion_block_level AND by more than 3x its own learned
 * flicker (IR/night picture noise), and only next to another changed block (noise speckle hits single
 * blocks at random; anything moving changes neighbouring blocks). Motion when at least motion_blocks
 * count on motion_frames frames in a row. With motion_ignore_light, a lighting change (most of the
 * picture changing, or the average brightness shifting by > 4 within ~0.3 s: lights, IR switching,
 * headlights through a window) pauses detection for 1.5 s. Same method as the S3's plug-in. */
#define GW 40
#define GH 30
static uint8_t blk_prev[GW * GH];
static float blk_noise[GW * GH];
static int blk_have;
static void analyse(const uint8_t *y) {
    unsigned w = C.width, h = C.height, bw = w / GW, bh = h / GH;
    unsigned sums[GW * GH];
    unsigned long total = 0;
    memset(sums, 0, sizeof sums);
    /* no division per pixel: the ARM9 has no hardware divide */
    for (unsigned by = 0, j = 0; by < GH; by++)
        for (unsigned r = 0; r < bh; r++, j++) {
            const uint8_t *p = y + j * w;
            unsigned *srow = sums + by * GW;
            for (unsigned bx = 0; bx < GW; bx++) {
                unsigned acc = 0;
                for (unsigned i = 0; i < bw; i++) acc += *p++;
                srow[bx] += acc;
            }
        }
    uint8_t cur[GW * GH], flag[GW * GH];
    unsigned area = bw * bh;                         /* 1200 divisions per frame is fine */
    for (int k = 0; k < GW * GH; k++) { cur[k] = sums[k] / area; total += cur[k]; }
    int bright = total / (GW * GH);

    int raw = 0, counted = 0;
    if (blk_have) {
        for (int k = 0; k < GW * GH; k++) {
            int d = abs(cur[k] - blk_prev[k]);
            flag[k] = d > C.motion_block_level && d > blk_noise[k] * 3 + 2;
            if (flag[k]) raw++;
            else blk_noise[k] = blk_noise[k] * 0.95f + d * 0.05f;
        }
        for (int k = 0; k < GW * GH; k++) {
            if (!flag[k]) continue;
            int x = k % GW, yy = k / GW, near = 0;
            for (int dy = -1; dy <= 1 && !near; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = yy + dy;
                    if ((dx || dy) && nx >= 0 && nx < GW && ny >= 0 && ny < GH && flag[ny * GW + nx]) { near = 1; break; }
                }
            counted += near;
        }
    } else for (int k = 0; k < GW * GH; k++) blk_noise[k] = C.motion_block_level;
    memcpy(blk_prev, cur, sizeof cur); blk_have = 1;

    static int means[4], mi = 0, over = 0;
    static double suppress_until, last_trigger;
    double t = now_s();
    int then = means[mi];                            /* average brightness ~4 frames ago */
    means[mi] = bright; mi = (mi + 1) % 4;
    if (C.motion_ignore_light && then > 0 && (abs(bright - then) > 4 || raw > GW * GH / 2)) suppress_until = t + 1.5;
    int level = t < suppress_until ? 0 : counted;
    S.brightness = bright;
    S.motion_score = level;
    if (level > S.motion_peak) S.motion_peak = level;
    if (level > S.push_peak) S.push_peak = level;
    if (!C.motion_enabled || t < suppress_until) over = 0;
    else if (level >= C.motion_blocks) { if (++over >= C.motion_frames) last_trigger = t; }
    else over = 0;
    S.motion = C.motion_enabled && last_trigger > 0 && t - last_trigger <= C.motion_hold_s + 0.0;
}

/* Sensor self-test. Now and then (after a reset) the GC0308 misses its I2C setup: the picture is
 * washed-out white and ignores every sensor control. Shortly after startup, if the picture is that
 * bright, step the sensor's brightness to its minimum and maximum and see whether the picture
 * follows; if it doesn't, the sensor isn't configured and only a fresh start fixes it (capture
 * start power-cycles the sensor), so reboot (setting auto_reboot). A genuinely bright scene
 * follows the controls and passes; only a dead sensor could loop, and auto_reboot=0 stops that. */
static void sensor_selftest(void) {
    static int stage = 0; static unsigned at; static int lo;
    if (stage < 0 || stage > 3) return;
    unsigned f = S.frames;
    if (stage == 0) {                                /* ~3 s in: is the picture suspiciously white? */
        if (f < 30) return;
        if (S.brightness < 215) { stage = 4; return; }   /* normal picture: test passed */
        sensor_ctrl(V4L2_CID_BRIGHTNESS, 0); at = f; stage = 1;
    } else if (stage == 1 && f >= at + 15) {
        lo = S.brightness; sensor_ctrl(V4L2_CID_BRIGHTNESS, 6); at = f; stage = 2;
    } else if (stage == 2 && f >= at + 15) {
        int hi = S.brightness;
        sensor_ctrl(V4L2_CID_BRIGHTNESS, C.sensor_brightness);
        stage = 4;
        if (hi - lo >= 6) { logm("sensor self-test: picture white but responds (%d..%d), ok", lo, hi); return; }
        if (!C.auto_reboot) { logm("sensor self-test: picture white and the sensor ignores its controls (%d..%d); auto_reboot is off", lo, hi); return; }
        logm("sensor self-test: picture white and the sensor ignores its controls (%d..%d): rebooting", lo, hi);
        reboot_req = 1;
    }
}

/* ---- IR ---- */
static double ir_last_switch = -1e9;
static int ir_baseline = -1;         /* brightness of the IR-lit picture, learned after IR on */
static void set_night(int n, const char *why) {
    if (n == S.night && ir_last_switch > -1e8) return;
    if (dbg_noir) { S.night = n; ir_last_switch = now_s(); logm("IR %s skipped (CAMD_NOIR)", n ? "on" : "off"); return; }
    if (!dbg_noir) { ircut(n); gpio_out(47, n ? 1 : 0); }
    S.night = n; ir_last_switch = now_s(); ir_baseline = -1;
    logm("IR %s (%s, brightness %d)", n ? "on" : "off", why, S.brightness);
}
static void ir_tick(void) {
    static double cond_since; static int cond_dir;
    if (!strcmp(C.ir_mode, "on")) { if (!S.night) set_night(1, "mode on"); return; }
    if (!strcmp(C.ir_mode, "off")) { if (S.night) set_night(0, "mode off"); return; }
    double t = now_s();
    /* The IR LEDs light the picture themselves, so "bright" at night can't mean daylight.
     * Learn how bright the IR-lit picture is (peak over seconds 2-8 after switching on) and
     * only switch off when it gets clearly brighter than that: real ambient light added. */
    if (S.night && t - ir_last_switch >= 2 && t - ir_last_switch <= 8 && S.brightness > ir_baseline) ir_baseline = S.brightness;
    if (S.night && t - ir_last_switch < 8) { cond_dir = -1; return; }
    int off_at = C.ir_off_above;
    if (ir_baseline >= 0 && ir_baseline + C.ir_off_margin > off_at) off_at = ir_baseline + C.ir_off_margin;
    int want = S.night ? (S.brightness > off_at ? 0 : 1) : (S.brightness < C.ir_on_below ? 1 : 0);
    if (want == S.night) { cond_dir = -1; return; }
    if (cond_dir != want) { cond_dir = want; cond_since = t; return; }
    if (t - cond_since >= C.ir_hold_s && t - ir_last_switch >= C.ir_min_dwell_s) set_night(want, "auto");
}

/* ========================================================================================
 * Sound (from audiod)
 * ====================================================================================== */
static void sound_cfg_write(void) {
    FILE *f = fopen(SOUND_CFG ".tmp", "w");
    if (!f) return;
    fprintf(f, "%d %d %d\n", C.sound_enabled, C.sound_db, C.sound_hold_s);
    fclose(f); rename(SOUND_CFG ".tmp", SOUND_CFG);
}
/* (Re)start busybox ntpd with the configured servers. */
static void ntp_apply(void) {
    char cmd[256]; int k = snprintf(cmd, sizeof cmd, "/tmp/busybox killall ntpd 2>/dev/null; ");
    if (C.ntp_servers[0]) {
        /* on every sync, store UTC in the RTC (the vendor firmware kept local time there, so
         * boots without Wi-Fi started 2 h off); ntpd runs this with its own args, ignored */
        FILE *f = fopen("/tmp/ntp_rtc.sh", "w");
        if (f) { fputs("#!/bin/sh\nexec /tmp/busybox hwclock -w -u\n", f); fclose(f); chmod("/tmp/ntp_rtc.sh", 0755); }
        k += snprintf(cmd + k, sizeof cmd - k, "/tmp/busybox ntpd -S /tmp/ntp_rtc.sh");
        char list[96]; snprintf(list, sizeof list, "%s", C.ntp_servers);
        for (char *t = strtok(list, ","); t; t = strtok(0, ","))
            if (*t) k += snprintf(cmd + k, sizeof cmd - k, " -p %s", t);
    }
    if (system(cmd) != 0) logm("ntpd restart failed");
    logm("ntp servers: %s", C.ntp_servers[0] ? C.ntp_servers : "(none)");
}
static void sound_poll(void) {
    FILE *f = fopen(SOUND_FILE, "r");
    int active = 0, peak = -96, listening = 0;
    if (f) { if (fscanf(f, "%d %d %d", &active, &peak, &listening) < 2) active = 0; fclose(f); }
    S.sound = active && C.sound_enabled; S.sound_peak_db = peak; S.sound_listening = listening;
    if (peak > S.sound_peak_http) S.sound_peak_http = peak;
    if (peak > S.sound_peak_push) S.sound_peak_push = peak;
}

/* ========================================================================================
 * Recording (MJPEG AVI on the SD card)
 * ====================================================================================== */
struct avi { FILE *f; unsigned frames, movi_pos, max_size; uint32_t *idx; unsigned idx_cap; };
static void put32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v, f); fputc(v >> 8, f); }
static void putcc(FILE *f, const char *s) { fwrite(s, 1, 4, f); }
static int avi_open(struct avi *a, const char *path, int w, int h, int fps) {
    memset(a, 0, sizeof *a);
    a->f = fopen(path, "wb"); if (!a->f) return -1;
    FILE *f = a->f;
    putcc(f, "RIFF"); put32(f, 0); putcc(f, "AVI ");
    putcc(f, "LIST"); put32(f, 4 + 64 + 12 + 56 + 48); putcc(f, "hdrl");
    putcc(f, "avih"); put32(f, 56);
    put32(f, 1000000 / fps); put32(f, 0); put32(f, 0); put32(f, 0x10);       /* usPerFrame, maxBps, pad, flags=HASINDEX */
    put32(f, 0); put32(f, 0); put32(f, 1); put32(f, 0);                       /* totalFrames (patched), initial, streams, suggestedBuf */
    put32(f, w); put32(f, h); put32(f, 0); put32(f, 0); put32(f, 0); put32(f, 0);
    putcc(f, "LIST"); put32(f, 4 + 64 + 48); putcc(f, "strl");
    putcc(f, "strh"); put32(f, 56); putcc(f, "vids"); putcc(f, "MJPG");
    put32(f, 0); put16(f, 0); put16(f, 0); put32(f, 0);
    put32(f, 1); put32(f, fps); put32(f, 0); put32(f, 0);                     /* scale, rate, start, length (patched) */
    put32(f, 0); put32(f, (uint32_t)-1); put32(f, 0); put16(f, 0); put16(f, 0); put16(f, w); put16(f, h);
    putcc(f, "strf"); put32(f, 40);
    put32(f, 40); put32(f, w); put32(f, h); put16(f, 1); put16(f, 24); putcc(f, "MJPG");
    put32(f, w * h * 3); put32(f, 0); put32(f, 0); put32(f, 0); put32(f, 0);
    putcc(f, "LIST"); put32(f, 0); a->movi_pos = ftell(f); putcc(f, "movi");
    return 0;
}
static void avi_frame(struct avi *a, const uint8_t *jp, unsigned len) {
    if (a->frames >= a->idx_cap) {
        a->idx_cap = a->idx_cap ? a->idx_cap * 2 : 256;
        a->idx = realloc(a->idx, a->idx_cap * 2 * sizeof(uint32_t));
    }
    uint32_t off = ftell(a->f) - a->movi_pos;
    putcc(a->f, "00dc"); put32(a->f, len); fwrite(jp, 1, len, a->f); if (len & 1) fputc(0, a->f);
    a->idx[a->frames * 2] = off; a->idx[a->frames * 2 + 1] = len;
    if (len > a->max_size) a->max_size = len;
    a->frames++;
}
static void avi_close(struct avi *a) {
    if (!a->f) return;
    FILE *f = a->f;
    uint32_t movi_end = ftell(f);
    putcc(f, "idx1"); put32(f, a->frames * 16);
    for (unsigned i = 0; i < a->frames; i++) { putcc(f, "00dc"); put32(f, 0x10); put32(f, a->idx[i * 2]); put32(f, a->idx[i * 2 + 1]); }
    uint32_t end = ftell(f);
    fseek(f, 4, SEEK_SET); put32(f, end - 8);
    fseek(f, 32 + 16, SEEK_SET); put32(f, a->frames);                                   /* avih totalFrames */
    fseek(f, 32 + 28, SEEK_SET); put32(f, a->max_size);                                 /* avih suggestedBuffer */
    fseek(f, 12 + 12 + 64 + 12 + 8 + 32, SEEK_SET); put32(f, a->frames);                 /* strh length */
    fseek(f, a->movi_pos - 4, SEEK_SET); put32(f, movi_end - a->movi_pos);
    fclose(f); free(a->idx); a->f = 0;
    sync();
}
static int cmp_names(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }
static void rec_prune(void) {
    for (int guard = 0; guard < 50; guard++) {
        struct statvfs v; if (statvfs(REC_DIR, &v)) return;
        unsigned long free_mb = (unsigned long)((unsigned long long)v.f_bavail * v.f_frsize >> 20);
        if (free_mb >= (unsigned long)C.record_keep_free_mb) return;
        DIR *d = opendir(REC_DIR); if (!d) return;
        char *oldest = 0; struct dirent *e;
        while ((e = readdir(d))) {
            if (!strstr(e->d_name, ".avi")) continue;
            if (!oldest || strcmp(e->d_name, oldest) < 0) { free(oldest); oldest = strdup(e->d_name); }
        }
        closedir(d);
        if (!oldest) return;
        char p[128]; snprintf(p, sizeof p, "%s/%s", REC_DIR, oldest);
        logm("recording: low space (%lu MB), deleting %s", free_mb, oldest);
        unlink(p); free(oldest);
    }
}
static void clip_name(char *out, size_t n) {
    time_t t = time(0); struct tm tm; localtime_r(&t, &tm);
    if (tm.tm_year + 1900 >= 2024) strftime(out, n, "%Y%m%d-%H%M%S.avi", &tm);
    else snprintf(out, n, "notime-%07.0f.avi", now_s());
}
static void *recorder_thread(void *arg) {
    (void)arg;
    struct { uint8_t *d; unsigned len; } pre[5 * 15];
    int npre = 0, prehead = 0;
    uint8_t *buf = malloc(SLOTMAX);
    struct avi a; memset(&a, 0, sizeof a);
    uint32_t last = 0;
    double last_take = 0, clip_start = 0, motion_last = 0;
    int clip_manual = 0;
    while (!quit_flag) {
        int rec_motion = C.record_enabled || S.btn_motion;
        if (!rec_motion && !S.manual_rec) {
            if (a.f) { avi_close(&a); S.recording = 0; }
            for (int i = 0; i < npre; i++) free(pre[(prehead + i) % 75].d);
            npre = 0; sleep(1); continue;
        }
        double age;
        unsigned len = ring_get(&last, buf, 1.0, &age);
        double t = now_s();
        if (S.motion) motion_last = t;
        if (!len || t - last_take < 1.0 / C.record_fps) continue;
        last_take = t;
        int maxpre = C.record_pre_s * C.record_fps; if (maxpre > 75) maxpre = 75;
        if (!a.f) {
            /* keep pre-roll */
            if (maxpre > 0) {
                if (npre == maxpre) { free(pre[prehead].d); prehead = (prehead + 1) % 75; npre--; }
                int slot = (prehead + npre) % 75;
                pre[slot].d = malloc(len); if (pre[slot].d) { memcpy(pre[slot].d, buf, len); pre[slot].len = len; npre++; }
            }
            if (S.manual_rec || (rec_motion && S.motion)) {
                clip_manual = S.manual_rec;
                mkdir(REC_DIR, 0755); rec_prune();
                char name[48], path[96]; clip_name(name, sizeof name);
                snprintf(path, sizeof path, "%s/%s", REC_DIR, name);
                if (avi_open(&a, path, C.width, C.height, C.record_fps) == 0) {
                    for (int i = 0; i < npre; i++) { int s = (prehead + i) % 75; avi_frame(&a, pre[s].d, pre[s].len); free(pre[s].d); }
                    npre = 0; clip_start = t; S.recording = 1;
                    snprintf(S.last_clip, sizeof S.last_clip, "%s", name);
                    logm("recording %s", name);
                } else logm("recording: can't create %s: %s", path, strerror(errno));
            }
        }
        if (a.f) {
            avi_frame(&a, buf, len);
            /* a button recording runs until the button is pressed again, split into record_max_s
             * clips; a motion clip ends record_post_s after the motion */
            int over = clip_manual ? !S.manual_rec : (!S.motion && t - motion_last > C.record_post_s);
            if (over || t - clip_start > C.record_max_s) {
                logm("recording done: %s, %u frames", S.last_clip, a.frames);
                avi_close(&a); S.recording = 0;
            }
        }
    }
    if (a.f) avi_close(&a);
    return 0;
}
/* recording started with the button or /record: 0 off, 1 continuous, 2 motion clips */
static void set_rec_mode(int mode, const char *why) {
    if (S.manual_rec == (mode == 1) && S.btn_motion == (mode == 2)) return;
    S.manual_rec = mode == 1; S.btn_motion = mode == 2;
    logm("recording mode %s (%s)", mode == 1 ? "continuous" : mode == 2 ? "motion" : "off", why);
}
static const char *rec_mode_name(void) { return S.manual_rec ? "continuous" : S.btn_motion ? "motion" : "off"; }

/* ========================================================================================
 * Push to Home Assistant
 * ====================================================================================== */
static pthread_mutex_t push_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t push_cv = PTHREAD_COND_INITIALIZER;
static char push_reason[24];
static void push_event(const char *reason) {
    pthread_mutex_lock(&push_mu);
    snprintf(push_reason, sizeof push_reason, "%s", reason);
    pthread_cond_signal(&push_cv);
    pthread_mutex_unlock(&push_mu);
}
static int status_json(char *out, size_t n, const char *type);
static void http_post(const char *url, const char *body) {
    char host[128], path[192]; int port = 80;
    if (strncmp(url, "http://", 7)) return;
    const char *h = url + 7, *sl = strchr(h, '/');
    size_t hl = sl ? (size_t)(sl - h) : strlen(h); if (hl >= sizeof host) return;
    memcpy(host, h, hl); host[hl] = 0;
    snprintf(path, sizeof path, "%s", sl ? sl : "/");
    char *colon = strchr(host, ':'); if (colon) { *colon = 0; port = atoi(colon + 1); }
    struct addrinfo hints = { 0 }, *ai = 0; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai) || !ai) return;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 3, 0 }; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0) {
        char hdr[512]; int hn = snprintf(hdr, sizeof hdr,
            "POST %s HTTP/1.0\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
            path, host, (unsigned)strlen(body));
        send(s, hdr, hn, MSG_NOSIGNAL); send(s, body, strlen(body), MSG_NOSIGNAL);
        char tmp[256]; recv(s, tmp, sizeof tmp, 0);
    }
    close(s); freeaddrinfo(ai);
}
static void *push_thread(void *arg) {
    (void)arg;
    char body[1024], reason[24];
    double last_beat = 0;
    while (!quit_flag) {
        pthread_mutex_lock(&push_mu);
        if (!push_reason[0]) {
            struct timespec ts; abs_deadline(&ts, 1.0);
            pthread_cond_timedwait(&push_cv, &push_mu, &ts);
        }
        snprintf(reason, sizeof reason, "%s", push_reason); push_reason[0] = 0;
        pthread_mutex_unlock(&push_mu);
        double t = now_s();
        if (!reason[0] && t - last_beat >= C.heartbeat_s) snprintf(reason, sizeof reason, "heartbeat");
        if (!reason[0] || !C.webhook[0]) continue;
        if (!strcmp(reason, "heartbeat")) last_beat = t;
        status_json(body, sizeof body, reason);
        http_post(C.webhook, body);
    }
    return 0;
}

/* ========================================================================================
 * HTTP server
 * ====================================================================================== */
static int sa(int fd, const void *b, int n) { const char *p = b; while (n > 0) { int w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) return -1; p += w; n -= w; } return 0; }
static int sas(int fd, const char *s) { return sa(fd, s, strlen(s)); }
static void reply(int fd, int code, const char *ctype, const char *body, int len) {
    char h[256]; if (len < 0) len = strlen(body);
    int hn = snprintf(h, sizeof h, "HTTP/1.0 %d %s\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\n"
                      "Content-Type: %s\r\nContent-Length: %d\r\n\r\n", code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Error", ctype, len);
    if (sa(fd, h, hn) == 0) sa(fd, body, len);
}
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char hx[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(hx, 0, 16); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}
static void json_str(char *o, size_t n, const char *s) {
    size_t k = 0; if (n < 3) return; o[k++] = '"';
    for (; *s && k + 7 < n; s++) { if (*s == '"' || *s == '\\') { o[k++] = '\\'; o[k++] = *s; } else if ((unsigned char)*s < 32) k += snprintf(o + k, n - k, "\\u%04x", *s); else o[k++] = *s; }
    o[k++] = '"'; o[k] = 0;
}
/* Wi-Fi signal (dBm) from /proc/net/wireless; 0 if unknown */
static int wifi_dbm(void) {
    FILE *f = fopen("/proc/net/wireless", "r"); if (!f) return 0;
    char line[160]; int dbm = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "wlan0:"); if (!p) continue;
        unsigned st; float q, lvl;
        if (sscanf(p + 6, "%x %f %f", &st, &q, &lvl) == 3) dbm = (int)lvl;
        break;
    }
    fclose(f);
    return dbm;
}
/* Power: the board reads the battery terminals (with a bench supply on them: the supply voltage),
 * the driver's charge estimate from that, and whether USB/mains (the charger input) is present. */
static int read_int(const char *path, int dflt) {
    FILE *f = fopen(path, "r"); int v = dflt;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = dflt; fclose(f); }
    return v;
}
#define PSU "/sys/class/power_supply/"
static void power_json(char *out, size_t n) {
    char st[24] = "unknown"; FILE *f = fopen(PSU "battery/status", "r");
    if (f) { if (fscanf(f, "%23s", st) != 1) strcpy(st, "unknown"); fclose(f); }
    snprintf(out, n, "\"battery_mv\":%d,\"battery_pct\":%d,\"battery_status\":\"%s\",\"charger\":%s",
             read_int(PSU "battery/voltage_now", -1), read_int(PSU "battery/capacity", -1), st,
             read_int(PSU "usb/online", 0) || read_int(PSU "ac/online", 0) ? "true" : "false");
}
static int status_json(char *out, size_t n, const char *type) {
    struct statvfs v; long sd_free = -1, sd_total = -1;
    if (!statvfs("/mnt/mmc", &v)) { sd_free = (long)((unsigned long long)v.f_bavail * v.f_frsize >> 20); sd_total = (long)((unsigned long long)v.f_blocks * v.f_frsize >> 20); }
    char clip[80]; json_str(clip, sizeof clip, S.last_clip);
    int pushed = strcmp(type, "status") != 0;    /* webhook pushes use their own peak */
    int peak = pushed ? S.push_peak : S.motion_peak;
    if (pushed) S.push_peak = S.motion_score; else S.motion_peak = S.motion_score;
    int speak = pushed ? S.sound_peak_push : S.sound_peak_http;
    if (pushed) S.sound_peak_push = S.sound_peak_db; else S.sound_peak_http = S.sound_peak_db;
    char pw[160]; power_json(pw, sizeof pw);
    return snprintf(out, n,
        "{\"type\":\"%s\",\"motion\":%s,\"motion_score\":%d,\"motion_peak\":%d,\"sound\":%s,\"sound_peak\":%d,\"sound_listening\":%s,"
        "\"brightness\":%d,\"ir_baseline\":%d,\"night\":%s,\"ir_mode\":\"%s\",\"recording\":%s,\"rec_mode\":\"%s\",\"last_clip\":%s,\"viewers\":%d,"
        "\"fps\":%.1f,\"width\":%d,\"height\":%d,\"uptime\":%.0f,\"sd_free_mb\":%ld,\"sd_total_mb\":%ld,\"wifi_dbm\":%d,%s,\"version\":\"%s\"}",
        type, S.motion ? "true" : "false", S.motion_score, peak, S.sound ? "true" : "false", speak,
        S.sound_listening ? "true" : "false", S.brightness, ir_baseline, S.night ? "true" : "false", C.ir_mode,
        S.recording ? "true" : "false", rec_mode_name(), clip, S.viewers, S.fps, C.width, C.height, now_s() - S.started,
        sd_free, sd_total, wifi_dbm(), pw, VERSION);
}
static int settings_json(char *out, size_t n) {
    size_t k = snprintf(out, n, "{");
    for (unsigned i = 0; i < NSET && k < n; i++) {
        const struct setting *s = &SETTINGS[i];
        char v[220];
        if (s->type == T_INT) snprintf(v, sizeof v, "%d", *(int *)s->ptr); else json_str(v, sizeof v, (char *)s->ptr);
        char help[200]; json_str(help, sizeof help, s->help);
        k += snprintf(out + k, n - k, "%s\"%s\":{\"value\":%s,\"type\":\"%s\",\"min\":%d,\"max\":%d,\"restart\":%s,\"help\":%s}",
                      i ? "," : "", s->key, v, s->type == T_INT ? "int" : "str", s->min, s->max, s->restart ? "true" : "false", help);
    }
    if (k < n) k += snprintf(out + k, n - k, "}");
    return k;
}
/* apply "k=v&k2=v2"; writes a JSON result into out. "save=0" applies without writing the
 * SD card: Home Assistant pushes that way (it switches ir_mode often and re-pushes everything
 * after each camera start), so only changes made in the web UI end up in q7cam.ini. */
static void apply_settings(char *q, char *out, size_t n) {
    int changed = 0, bad = 0, sensor = 0, sound = 0, ntp = 0, save = 1;
    size_t k = snprintf(out, n, "{\"ok\":[");
    char errs[512]; size_t ek = 0; errs[0] = 0;
    pthread_mutex_lock(&cfg_mu);
    for (char *tok = strtok(q, "&"); tok; tok = strtok(0, "&")) {
        char *eq = strchr(tok, '='); if (!eq) continue;
        *eq = 0; char *key = tok, *val = eq + 1; url_decode(key); url_decode(val);
        if (!strcmp(key, "save")) { save = strcmp(val, "0") != 0; continue; }
        const struct setting *s = find_setting(key);
        if (!s || set_value(s, val)) { bad++; ek += snprintf(errs + ek, sizeof errs - ek, "%s\"%s\"", ek ? "," : "", key); continue; }
        k += snprintf(out + k, n - k, "%s\"%s\"", changed ? "," : "", key);
        changed++;
        if (!strncmp(key, "sensor_", 7)) sensor = 1;
        if (!strncmp(key, "sound_", 6)) sound = 1;
        if (!strcmp(key, "tz")) { setenv("TZ", C.tz, 1); tzset(); }
        if (!strcmp(key, "ntp_servers")) ntp = 1;
    }
    if (changed && save) save_config();
    pthread_mutex_unlock(&cfg_mu);
    if (sensor) { sensor_ctrl(V4L2_CID_CONTRAST, C.sensor_contrast); sensor_ctrl(V4L2_CID_BRIGHTNESS, C.sensor_brightness); }
    if (sound) sound_cfg_write();
    if (ntp) ntp_apply();
    snprintf(out + k, n - k, "],\"rejected\":[%s]}", errs);
    (void)bad;
    if (changed) push_event("settings");
}

static char ui_path[96] = "/mnt/mmc/q7ui.html";   /* the web UI lives on the SD card (editable, no RAM cost) */
static void serve_ui(int c) {
    FILE *f = fopen(ui_path, "rb");
    if (!f) { reply(c, 200, "text/html", "<p>q7ui.html missing. <a href=/snapshot>snapshot</a> <a href=/settings>settings</a></p>", -1); return; }
    struct stat st; fstat(fileno(f), &st);
    char h[192]; int hn = snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-cache\r\n"
                                   "Content-Type: text/html; charset=utf-8\r\nContent-Length: %ld\r\n\r\n", (long)st.st_size);
    if (sa(c, h, hn) == 0) { char b[4096]; size_t r; while ((r = fread(b, 1, sizeof b, f)) > 0) if (sa(c, b, r)) break; }
    fclose(f);
}
static void serve_mjpeg(int c) {
    uint8_t *buf = malloc(SLOTMAX); if (!buf) return;
    __sync_fetch_and_add(&S.viewers, 1);
    sas(c, "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
           "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n");
    uint32_t last = 0;
    while (!quit_flag) {
        unsigned len = ring_get(&last, buf, 5.0, 0);
        if (!len) continue;
        char p[96]; int pn = snprintf(p, sizeof p, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", len);
        if (sa(c, p, pn) || sa(c, buf, len) || sa(c, "\r\n", 2)) break;
    }
    __sync_fetch_and_sub(&S.viewers, 1);
    free(buf);
}
static void serve_snapshot(int c) {
    uint8_t *buf = malloc(SLOTMAX); if (!buf) { reply(c, 500, "text/plain", "oom", -1); return; }
    uint32_t last = 0; double age = 99;
    unsigned len = ring_get(&last, buf, 0.1, &age);
    if (!len || age > 0.5) { snapshot_wanted = 1; uint32_t l2 = last; unsigned n2 = ring_get(&l2, buf, 2.0, &age); if (n2) len = n2; }
    if (len) reply(c, 200, "image/jpeg", (char *)buf, len); else reply(c, 503, "text/plain", "no frame yet", -1);
    free(buf);
}
static void serve_recordings(int c) {
    DIR *d = opendir(REC_DIR);
    char *names[512]; int n = 0;
    if (d) { struct dirent *e; while ((e = readdir(d)) && n < 512) if (strstr(e->d_name, ".avi")) names[n++] = strdup(e->d_name); closedir(d); }
    qsort(names, n, sizeof *names, cmp_names);
    size_t cap = 64 + n * 96, k = 0; char *out = malloc(cap);
    k += snprintf(out + k, cap - k, "[");
    for (int i = n - 1; i >= 0; i--) {
        char p[128]; struct stat st; snprintf(p, sizeof p, "%s/%s", REC_DIR, names[i]);
        long sz = stat(p, &st) ? 0 : (long)st.st_size;
        k += snprintf(out + k, cap - k, "%s{\"name\":\"%s\",\"size\":%ld}", i == n - 1 ? "" : ",", names[i], sz);
        free(names[i]);
    }
    k += snprintf(out + k, cap - k, "]");
    reply(c, 200, "application/json", out, k); free(out);
}
static void serve_rec_file(int c, char *name, int del) {
    url_decode(name);
    if (strchr(name, '/') || strstr(name, "..") || !strstr(name, ".avi")) { reply(c, 404, "text/plain", "not found", -1); return; }
    char p[128]; snprintf(p, sizeof p, "%s/%s", REC_DIR, name);
    if (del) { int r = unlink(p); sync(); reply(c, r ? 404 : 200, "application/json", r ? "{\"deleted\":false}" : "{\"deleted\":true}", -1); return; }
    FILE *f = fopen(p, "rb"); if (!f) { reply(c, 404, "text/plain", "not found", -1); return; }
    struct stat st; fstat(fileno(f), &st);
    char h[256]; int hn = snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: video/x-msvideo\r\n"
                                   "Content-Disposition: attachment; filename=\"%s\"\r\nContent-Length: %ld\r\n\r\n", name, (long)st.st_size);
    if (sa(c, h, hn) == 0) { char b[8192]; size_t r; while ((r = fread(b, 1, sizeof b, f)) > 0) if (sa(c, b, r)) break; }
    fclose(f);
}
/* ---- file manager (/fs) and clip player (/play): everything under /mnt/mmc ----
 *   GET  /fs?path=DIR            JSON listing [{name,size,dir,mtime}]
 *   GET  /fs?path=FILE[&dl=1]    the file (dl=1: as a download)
 *   GET  /fs?path=P&delete=1     delete a file or an empty folder
 *   POST /fs?path=FILE           body = new content (written to FILE.tmp, then renamed over FILE)
 *   GET  /play?path=CLIP.avi     the clip as an MJPEG stream at its own frame rate (an <img> plays it) */
#define FS_ROOT "/mnt/mmc"
static int fs_path(const char *query, char *out, size_t n) {
    const char *p = query ? strstr(query, "path=") : 0;
    if (!p) { snprintf(out, n, FS_ROOT); return 0; }
    p += 5; size_t k = strcspn(p, "&");
    if (k >= n) return -1;
    memcpy(out, p, k); out[k] = 0; url_decode(out);
    size_t l = strlen(out); while (l > 1 && out[l - 1] == '/') out[--l] = 0;
    if (strncmp(out, FS_ROOT, 8) || (out[8] && out[8] != '/') || strstr(out, "..")) return -1;
    return 0;
}
static int cmp_fs(const void *a, const void *b) {        /* folders first, then by name */
    const char *x = *(char **)a, *y = *(char **)b;
    if (*x != *y) return *x == 'd' ? -1 : 1;
    return strcasecmp(x + 1, y + 1);
}
static void fs_list(int c, const char *dir) {
    DIR *d = opendir(dir); if (!d) { reply(c, 404, "text/plain", "not found", -1); return; }
    char *names[400]; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < 400) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[512]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st)) continue;
        char *s = malloc(strlen(e->d_name) + 2); if (!s) break;
        s[0] = S_ISDIR(st.st_mode) ? 'd' : 'f'; strcpy(s + 1, e->d_name); names[n++] = s;
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_fs);
    size_t cap = 64 + (size_t)n * 400, k = 0; char *out = malloc(cap);
    if (!out) { for (int i = 0; i < n; i++) free(names[i]); reply(c, 500, "text/plain", "oom", -1); return; }
    k += snprintf(out + k, cap - k, "[");
    for (int i = 0; i < n; i++) {
        char p[512], js[300]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, names[i] + 1);
        if (stat(p, &st)) memset(&st, 0, sizeof st);
        json_str(js, sizeof js, names[i] + 1);
        k += snprintf(out + k, cap - k, "%s{\"name\":%s,\"size\":%ld,\"dir\":%s,\"mtime\":%ld}", i ? "," : "", js,
                      (long)st.st_size, names[i][0] == 'd' ? "true" : "false", (long)st.st_mtime);
        free(names[i]);
    }
    k += snprintf(out + k, cap - k, "]");
    reply(c, 200, "application/json", out, k); free(out);
}
static void fs_get(int c, const char *path, int dl) {
    FILE *f = fopen(path, "rb"); if (!f) { reply(c, 404, "text/plain", "not found", -1); return; }
    struct stat st; fstat(fileno(f), &st);
    const char *name = strrchr(path, '/') + 1;
    char h[400]; int hn = snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-cache\r\n"
                                   "Content-Type: %s\r\n%s%s%sContent-Length: %ld\r\n\r\n",
                                   strstr(name, ".avi") ? "video/x-msvideo" : "application/octet-stream",
                                   dl ? "Content-Disposition: attachment; filename=\"" : "", dl ? name : "", dl ? "\"\r\n" : "",
                                   (long)st.st_size);
    if (sa(c, h, hn) == 0) { char b[8192]; size_t r; while ((r = fread(b, 1, sizeof b, f)) > 0) if (sa(c, b, r)) break; }
    fclose(f);
}
#define FS_MAX_UPLOAD (16 * 1024 * 1024)
static void fs_put(int c, const char *path, const char *req, int n) {
    const char *cl = strcasestr(req, "\r\nContent-Length:");
    const char *body = strstr(req, "\r\n\r\n");
    if (!cl || !body) { reply(c, 400, "text/plain", "need Content-Length", -1); return; }
    long len = strtol(cl + 17, 0, 10); body += 4;
    if (len < 0 || len > FS_MAX_UPLOAD) { reply(c, 400, "text/plain", "too large (max 16 MB)", -1); return; }
    struct stat st; if (!stat(path, &st) && S_ISDIR(st.st_mode)) { reply(c, 400, "text/plain", "is a folder", -1); return; }
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) { reply(c, 500, "text/plain", strerror(errno), -1); return; }
    long have = n - (body - req); if (have > len) have = len;
    long done = 0; int ok = 1;
    if (have > 0) { ok = fwrite(body, 1, have, f) == (size_t)have; done = have; }
    char b[8192];
    while (ok && done < len) {
        int r = recv(c, b, len - done < (long)sizeof b ? len - done : (long)sizeof b, 0);
        if (r <= 0) { ok = 0; break; }
        ok = fwrite(b, 1, r, f) == (size_t)r; done += r;
    }
    if (fclose(f) || !ok || rename(tmp, path)) { unlink(tmp); sync(); reply(c, 500, "text/plain", "write failed", -1); return; }
    sync();
    if (!stat(path, &st)) chmod(path, 0755);
    logm("fs: wrote %s (%ld bytes)", path, done);
    char j[64]; snprintf(j, sizeof j, "{\"ok\":true,\"size\":%ld}", done); reply(c, 200, "application/json", j, -1);
}
static void serve_fs(int c, const char *method, const char *query, const char *req, int n) {
    char path[300];
    if (fs_path(query, path, sizeof path)) { reply(c, 400, "text/plain", "path must be under " FS_ROOT, -1); return; }
    if (!strcmp(method, "POST")) { fs_put(c, path, req, n); return; }
    struct stat st; if (stat(path, &st)) { reply(c, 404, "text/plain", "not found", -1); return; }
    if (query && strstr(query, "delete=1")) {
        if (!strcmp(path, FS_ROOT)) { reply(c, 400, "text/plain", "no", -1); return; }
        int r = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path); sync();
        if (!r) logm("fs: deleted %s", path);
        reply(c, r ? 500 : 200, "application/json", r ? "{\"deleted\":false}" : "{\"deleted\":true}", -1);
    } else if (S_ISDIR(st.st_mode)) fs_list(c, path);
    else fs_get(c, path, query && strstr(query, "dl=1"));
}
/* Our AVIs are RIFF/AVI with an MJPEG '00dc' chunk per frame inside LIST movi; a clip still being
 * recorded has zero sizes in its headers, so walk the chunks rather than trusting idx1. */
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void serve_play(int c, const char *query) {
    char path[300];
    if (fs_path(query, path, sizeof path)) { reply(c, 400, "text/plain", "bad path", -1); return; }
    FILE *f = fopen(path, "rb"); if (!f) { reply(c, 404, "text/plain", "not found", -1); return; }
    uint8_t h[64];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4)) { fclose(f); reply(c, 400, "text/plain", "not an AVI", -1); return; }
    unsigned us = 0;
    uint8_t *buf = malloc(SLOTMAX); if (!buf) { fclose(f); reply(c, 500, "text/plain", "oom", -1); return; }
    sas(c, "HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-cache\r\n"
           "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n");
    double next = now_s();
    while (!quit_flag && fread(h, 1, 8, f) == 8) {
        uint32_t sz = rd32(h + 4);
        if (!memcmp(h, "LIST", 4)) {
            if (fread(h + 8, 1, 4, f) != 4) break;
            if (!memcmp(h + 8, "movi", 4) || !memcmp(h + 8, "hdrl", 4) || !memcmp(h + 8, "strl", 4)) continue;   /* descend */
            fseek(f, sz - 4 + (sz & 1), SEEK_CUR);
        } else if (!memcmp(h, "avih", 4) && sz >= 4) {
            if (fread(h + 8, 1, 4, f) != 4) break;
            us = rd32(h + 8); fseek(f, sz - 4, SEEK_CUR);
        } else if (!memcmp(h, "00dc", 4)) {
            if (sz == 0 || sz > SLOTMAX || fread(buf, 1, sz, f) != sz) break;
            if (sz & 1) fgetc(f);
            double wait = next - now_s(); if (wait > 0) usleep((useconds_t)(wait * 1e6));
            next += (us >= 20000 && us <= 2000000 ? us : 200000) / 1e6;
            char p[96]; int pn = snprintf(p, sizeof p, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", sz);
            if (sa(c, p, pn) || sa(c, buf, sz) || sa(c, "\r\n", 2)) break;
        } else if (!memcmp(h, "idx1", 4)) break;
        else fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    free(buf); fclose(f);
}
static void *conn_thread(void *arg) {
    int c = (int)(long)arg;
    char req[2048]; int n = 0;
    struct timeval tv = { 5, 0 }; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (n < (int)sizeof req - 1) { int r = recv(c, req + n, sizeof req - 1 - n, 0); if (r <= 0) break; n += r; req[n] = 0; if (strstr(req, "\r\n\r\n")) break; }
    req[n] = 0;
    char method[8] = "", target[1024] = "";
    sscanf(req, "%7s %1023s", method, target);
    char *query = strchr(target, '?'); if (query) *query++ = 0;
    char *path = target;
    static char big[8192];   /* only used under big_mu */
    static pthread_mutex_t big_mu = PTHREAD_MUTEX_INITIALIZER;

    if (!strcmp(path, "/") || !strcmp(path, "/video") || !strcmp(path, "/stream") || !strcmp(path, "/mjpeg")) serve_mjpeg(c);
    else if (!strcmp(path, "/snapshot") || !strcmp(path, "/shot.jpg") || !strcmp(path, "/photo.jpg")) serve_snapshot(c);
    else if (!strcmp(path, "/status.json") || !strcmp(path, "/status")) {
        char b[1024]; int l = status_json(b, sizeof b, "status"); reply(c, 200, "application/json", b, l);
    } else if (!strcmp(path, "/settings") || !strcmp(path, "/settings.json")) {
        pthread_mutex_lock(&big_mu);
        if (query && *query) { char q[1024]; snprintf(q, sizeof q, "%s", query); apply_settings(q, big, sizeof big); }
        else settings_json(big, sizeof big);
        reply(c, 200, "application/json", big, -1);
        pthread_mutex_unlock(&big_mu);
    } else if (!strcmp(path, "/ui") || !strcmp(path, "/index.html")) serve_ui(c);
    else if (!strcmp(path, "/recordings")) serve_recordings(c);
    else if (!strcmp(path, "/fs")) serve_fs(c, method, query, req, n);
    else if (!strcmp(path, "/play")) serve_play(c, query);
    else if (!strncmp(path, "/rec/", 5)) serve_rec_file(c, path + 5, query && strstr(query, "delete=1"));
    else if (!strcmp(path, "/night") || !strcmp(path, "/day")) {
        char q[32]; snprintf(q, sizeof q, "ir_mode=%s", path[1] == 'n' ? "on" : "off");
        pthread_mutex_lock(&big_mu); apply_settings(q, big, sizeof big); pthread_mutex_unlock(&big_mu);
        reply(c, 200, "text/plain", path + 1, -1);
    } else if (!strcmp(path, "/reboot")) {
        /* GET /reboot?confirm=1. There is no working reboot()/sysrq on this kernel; the board
         * resets by itself ~10 s after pin 4 goes high while nobody services the camera. */
        if (query && strstr(query, "confirm=1")) { reply(c, 200, "text/plain", "rebooting in ~10 s\n", -1); reboot_req = 1; }
        else reply(c, 400, "text/plain", "add ?confirm=1\n", -1);
    } else if (!strcmp(path, "/record")) {          /* GET /record?mode=continuous|motion|off: as the button */
        const char *m = query ? strstr(query, "mode=") : 0;
        if (m) set_rec_mode(!strncmp(m + 5, "continuous", 10) ? 1 : !strncmp(m + 5, "motion", 6) ? 2 : 0, "http");
        char j[48]; snprintf(j, sizeof j, "{\"rec_mode\":\"%s\"}", rec_mode_name());
        reply(c, 200, "application/json", j, -1);
    } else if (!strcmp(path, "/irstate")) reply(c, 200, "text/plain", S.night ? "1" : "0", 1);
    else if (!strcmp(path, "/irled_on") || !strcmp(path, "/irled_off")) { gpio_out(47, path[7] == 'o' && path[8] == 'n'); reply(c, 200, "text/plain", "ok", -1); }
    else reply(c, 404, "text/plain", "not found", -1);
    close(c);
    return 0;
}
static void *http_thread(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(C.port);
    for (int tries = 0; bind(s, (void *)&a, sizeof a) < 0; tries++) { if (tries > 30) { logm("bind port %d failed", C.port); return 0; } sleep(1); }
    listen(s, 16);
    logm("http on port %d", C.port);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 96 * 1024); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    while (!quit_flag) {
        int c = accept(s, 0, 0); if (c < 0) continue;
        int nd = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof nd);
        pthread_t th; if (pthread_create(&th, &at, conn_thread, (void *)(long)c)) close(c);
    }
    return 0;
}

/* ========================================================================================
 * Main loop
 * ====================================================================================== */
static void on_signal(int sig) { (void)sig; quit_flag = 1; }

/* Reset the board: sync, then pin 4 high with nobody servicing the camera. Safe from any thread;
 * the capture loop sees reboot_req and stops servicing too (if it isn't stuck in the driver). */
static void reboot_now(const char *why) {
    logm("%s: syncing, then arming the reset (pin 4) and idling capture", why);
    reboot_req = 1;
    sync(); usleep(500000); sync();
    gpio_out(4, 1);
    for (;;) pause();
}
static void paths_init(void) {
    struct stat st;
    if (stat(Q7MOD, &st) || !S_ISDIR(st.st_mode)) return;          /* old layout: everything at the root */
    snprintf(conf_path, sizeof conf_path, Q7MOD "/q7cam.ini");
    snprintf(ui_path, sizeof ui_path, Q7MOD "/q7ui.html");
}
/* The little button is the kernel's "analog-key" input (/dev/input/event0), KEY_MENU (139):
 * press (1), auto-repeat while held (2), release (0).
 *   short tap         continuous recording (until stopped)
 *   hold ~3 s         motion recording (clips while there's motion, until stopped)
 *   any press while either is on stops it */
#define LONG_PRESS_S 3.0
static void *button_thread(void *arg) {
    (void)arg;
    int fd = open("/dev/input/event0", O_RDONLY);
    if (fd < 0) { logm("button: /dev/input/event0: %s", strerror(errno)); return 0; }
    struct input_event ev;
    double down = 0; int consumed = 1;
    while (read(fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type != EV_KEY || ev.code != KEY_MENU) continue;
        double t = now_s();
        if (ev.value == 1) {
            down = t; consumed = 0;
            if (S.manual_rec || S.btn_motion) { set_rec_mode(0, "button"); consumed = 1; }
        } else if (!consumed && t - down >= LONG_PRESS_S) {        /* held long enough (repeat or release) */
            set_rec_mode(2, "button, held"); consumed = 1;
        } else if (ev.value == 0 && !consumed) {
            set_rec_mode(1, "button"); consumed = 1;
        }
    }
    logm("button: read failed: %s", strerror(errno));
    return 0;
}
/* Independent of the capture loop, which can be stuck inside the driver (see camera_irqs). */
static void *watchdog_thread(void *arg) {
    (void)arg;
    int stalled = 0; double req_at = 0;
    for (;;) {
        sleep(1);
        double stall = now_s() - last_frame_t;
        if (stall >= STALL_S && !stalled) {
            stalled = 1;
            logm("no frames for %.0f s (camera irqs %ld now, %ld near the last frame, %u buffers failed to requeue)",
                 stall, camera_irqs(), irqs_at_frame, qbuf_fails);
            if (C.auto_reboot) reboot_now("picture frozen");
            logm("auto_reboot is off: not rebooting");
        } else if (stall < STALL_S && stalled) {
            stalled = 0; logm("frames are back");
        }
        /* /reboot or the sensor self-test asked, but the capture loop didn't act on it */
        if (!reboot_req) req_at = 0;
        else if (!req_at) req_at = now_s();
        else if (now_s() - req_at > 5) reboot_now("reboot requested, capture loop unresponsive");
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, on_signal); signal(SIGINT, on_signal);
    S.started = now_s();
    dbg_sync = !!getenv("CAMD_SYNC"); dbg_noir = !!getenv("CAMD_NOIR"); dbg_nojpeg = !!getenv("CAMD_NOJPEG");
    logm("%s starting", VERSION);
    paths_init();
    load_config();
    save_config();                                  /* writes defaults / normalises the file */
    setenv("TZ", C.tz, 1); tzset();
    ntp_apply();
    sound_cfg_write();
    for (int i = 0; i < NSLOT; i++) R.slot[i] = malloc(SLOTMAX);

    logm("capture start");
    if (capture_start()) { logm("capture start failed"); return 1; }
    logm("jpeg init");
    if (!dbg_nojpeg) jpeg_init();
    S.night = 0; ir_last_switch = now_s();          /* capture_start parked the IR in day mode */
    if (!strcmp(C.ir_mode, "on")) set_night(1, "startup");

    pthread_t th;
    pthread_create(&th, 0, http_thread, 0);
    pthread_create(&th, 0, push_thread, 0);
    pthread_create(&th, 0, recorder_thread, 0);
    last_frame_t = now_s() + 10;                     /* 10 s grace for the first frame */
    pthread_create(&th, 0, watchdog_thread, 0);
    pthread_create(&th, 0, button_thread, 0);
    push_event("startup");

    double last_enc = 0, fps_t0 = now_s(), last_sound = 0, last_status = 0;
    unsigned fps_n = 0, eagain = 0;
    int was_motion = 0, was_sound = 0, was_night = S.night, was_rec = 0, last_bright_sent = -100;
    /* Service the driver from the moment pin 4 is high: with pin 4 high and nobody
     * dequeuing/requeuing buffers, the board resets within seconds. */
    double last_dq = 0;
    while (!quit_flag) {
        if (reboot_req) reboot_now("reboot requested");  /* stops servicing the camera: board resets */
        /* pull only as many frames as needed: capture_fps for motion/brightness, more while
         * someone watches or a clip is recording */
        int pull = C.capture_fps;
        if (S.viewers > 0 && C.stream_fps > pull) pull = C.stream_fps;
        if (S.recording && C.record_fps > pull) pull = C.record_fps;
        double gap = now_s() - last_dq;
        if (gap < 1.0 / pull) usleep((useconds_t)((1.0 / pull - gap) * 1e6));
        struct v4l2_buffer b; memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_USERPTR;
        last_dq = now_s();
        if (ioctl(vfd, VIDIOC_DQBUF, &b) < 0) {     /* the driver's poll() never signals: retry */
            if (errno != EAGAIN && !eagain++) logm("DQBUF: %s", strerror(errno));
            usleep(20000); continue;                 /* a lasting stall is the watchdog thread's job */
        }
        eagain = 0; last_frame_t = now_s();
        uint8_t *y = (uint8_t *)b.m.userptr;
        double t = now_s();
        S.frames++; fps_n++;
        if (t - fps_t0 >= 2) {
            S.fps = fps_n / (t - fps_t0); fps_n = 0; fps_t0 = t;
            irqs_at_frame = camera_irqs();
            if (dbg_sync) logm("alive: %u frames, %u jpegs, %.1f fps, bright %d, score %d", S.frames, S.encodes, S.fps, S.brightness, S.motion_score);
        }
        if (S.frames == 1) {
            logm("first frame: idx %u bytesused %u", b.index, b.bytesused);
        }

        analyse(y);
        sensor_selftest();
        ir_tick();

        /* encode when someone needs a frame: viewers at stream_fps, recording at record_fps,
         * otherwise idle_fps so /snapshot is always fresh */
        double want = C.idle_fps;
        if (S.viewers > 0 && C.stream_fps > want) want = C.stream_fps;
        if ((C.record_enabled || S.manual_rec || S.btn_motion) && C.record_fps > want) want = C.record_fps;
        if (!dbg_nojpeg && (snapshot_wanted || t - last_enc >= 1.0 / want)) {
            unsigned len = jpeg_encode(y, C.quality);
            if (len) { ring_publish(jpeg_out, len); S.encodes++; }
            last_enc = t; snapshot_wanted = 0;
        }
        if (ioctl(vfd, VIDIOC_QBUF, &b) < 0 && qbuf_fails++ < 20)  /* a lost buffer starves the driver */
            logm("QBUF index %u failed: %s", b.index, strerror(errno));

        if (t - last_sound >= 0.2) { sound_poll(); last_sound = t; }
        if (S.motion != was_motion) { was_motion = S.motion; push_event("motion"); }
        if (S.sound != was_sound) { was_sound = S.sound; push_event("sound"); }
        if (S.night != was_night) { was_night = S.night; push_event("ir"); }
        if (S.recording != was_rec) { was_rec = S.recording; push_event("recording"); }
        if (abs(S.brightness - last_bright_sent) > 8 && t - last_status > 10) { last_bright_sent = S.brightness; last_status = t; push_event("brightness"); }
    }
    logm("stopping");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(vfd, VIDIOC_STREAMOFF, &type);
    usleep(300000);
    return 0;
}
