/* ioshim — LD_PRELOAD tracer for reverse-engineering LinuxIP's device bring-up.
 * Logs open/close/ioctl/mmap on /dev/video*, /dev/ak*, /dev/uio*, /dev/snd* with
 * the raw ioctl argument bytes (size from _IOC_SIZE) to /mnt/mmc/iotrace.log.
 * Repeated per-frame ioctls (QBUF/DQBUF) are only logged for the first few calls.
 *
 * Build (glibc, so it can be preloaded into LinuxIP): see build.sh target ioshim.
 * Use for ONE boot: in boot.sh, export LD_PRELOAD=/mnt/mmc/ioshim.so before update.exe.
 * Also logs every other open() path and system()/popen() commands, dumps the full
 * 16-byte /dev/akgpio struct (its ioctl numbers claim size 1), and fsyncs each line so
 * the trace survives a hang.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define MAXFD 256
static char watched[MAXFD][24];
static FILE *lg;
static struct timeval t0;
static int seen_cmd[64][2];   /* (cmd, count) for rate-limiting */

static void logf_(const char *fmt, ...) {
    if (!lg) {
        lg = fopen("/mnt/mmc/iotrace.log", "a");
        if (!lg) return;
        setvbuf(lg, NULL, _IOLBF, 0);
        gettimeofday(&t0, NULL);
        fprintf(lg, "=== ioshim in pid %d\n", getpid());
    }
    struct timeval t; gettimeofday(&t, NULL);
    fprintf(lg, "%6.3f ", (t.tv_sec - t0.tv_sec) + (t.tv_usec - t0.tv_usec) / 1e6);
    va_list ap; va_start(ap, fmt); vfprintf(lg, fmt, ap); va_end(ap);
    fflush(lg); fsync(fileno(lg));
}

static int interesting(const char *p) {
    return p && (!strncmp(p, "/dev/video", 10) || !strncmp(p, "/dev/ak", 7) ||
                 !strncmp(p, "/dev/uio", 8) || !strncmp(p, "/dev/snd", 8));
}

static int too_many(unsigned long cmd) {
    for (int i = 0; i < 64; i++) {
        if (seen_cmd[i][0] == (int)cmd || seen_cmd[i][0] == 0) {
            seen_cmd[i][0] = (int)cmd;
            return ++seen_cmd[i][1] > 6;
        }
    }
    return 0;
}

int open(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...);
    if (!real) real = dlsym(RTLD_NEXT, "open");
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    int fd = real(path, flags, mode);
    if (!interesting(path) && path && strncmp(path, "/proc", 5) && !strstr(path, "iotrace"))
        logf_("open(%s, 0x%x) = %d  [other]\n", path, flags, fd);
    if (interesting(path)) {
        logf_("open(%s, 0x%x) = %d\n", path, flags, fd);
        if (fd >= 0 && fd < MAXFD) snprintf(watched[fd], sizeof watched[fd], "%s", path);
    }
    return fd;
}
int open64(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    return open(path, flags, mode);
}

int close(int fd) {
    static int (*real)(int);
    if (!real) real = dlsym(RTLD_NEXT, "close");
    if (fd >= 0 && fd < MAXFD && watched[fd][0]) {
        logf_("close(%d %s)\n", fd, watched[fd]);
        watched[fd][0] = 0;
    }
    return real(fd);
}

int ioctl(int fd, unsigned long cmd, ...) {
    static int (*real)(int, unsigned long, ...);
    if (!real) real = dlsym(RTLD_NEXT, "ioctl");
    va_list ap; va_start(ap, cmd); void *arg = va_arg(ap, void *); va_end(ap);
    int w = fd >= 0 && fd < MAXFD && watched[fd][0];
    unsigned sz = _IOC_SIZE(cmd);
    if (w && !strcmp(watched[fd], "/dev/akgpio")) sz = 16;   /* real struct size */
    unsigned char before[256];
    if (w && arg && sz && sz <= sizeof before) memcpy(before, arg, sz);
    /* IOSHIM_BLOCK=4,40: drop the program's akgpio set-ops on those pins (bisecting
     * which pins the camera actually needs); reads still go through */
    const char *blk = getenv("IOSHIM_BLOCK");
    if (blk && w && arg && !strcmp(watched[fd], "/dev/akgpio") && cmd != 0x40016634 && cmd != 0x40016636) {
        char pin[8]; snprintf(pin, sizeof pin, "%d", ((unsigned char *)arg)[0]);
        size_t n = strlen(pin);
        for (const char *p = blk; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : 0)
            if (!strncmp(p, pin, n) && (p[n] == ',' || !p[n])) {
                logf_("BLOCKED akgpio 0x%08lx pin %s\n", cmd, pin);
                return 0;
            }
    }
    int r = real(fd, cmd, arg);
    if (w && (!strcmp(watched[fd], "/dev/akgpio") || !too_many(cmd))) {
        logf_("ioctl(%d %s, 0x%08lx sz=%u) = %d\n", fd, watched[fd], cmd, sz, r);
        if (arg && sz && sz <= sizeof before) {
            unsigned char *a = arg;
            fprintf(lg, "      in :"); for (unsigned i = 0; i < sz; i++) fprintf(lg, "%s%02x", i % 4 ? "" : " ", before[i]); fprintf(lg, "\n");
            fprintf(lg, "      out:"); for (unsigned i = 0; i < sz; i++) fprintf(lg, "%s%02x", i % 4 ? "" : " ", a[i]); fprintf(lg, "\n");
        }
    }
    return r;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    static void *(*real)(void *, size_t, int, int, int, off_t);
    if (!real) real = dlsym(RTLD_NEXT, "mmap");
    void *p = real(addr, len, prot, flags, fd, off);
    if (fd >= 0 && fd < MAXFD && watched[fd][0])
        logf_("mmap(len=0x%zx prot=%d flags=0x%x fd=%d %s off=0x%lx) = %p\n", len, prot, flags, fd, watched[fd], (long)off, p);
    return p;
}

int system(const char *cmd) {
    static int (*real)(const char *);
    if (!real) real = dlsym(RTLD_NEXT, "system");
    logf_("system(%s)\n", cmd ? cmd : "(null)");
    /* LinuxIP's reset-button handler deletes the Wi-Fi config; never let a test run do that */
    if (cmd && strstr(cmd, "wifi.txt") && strstr(cmd, "rm ")) { logf_("REFUSED (wifi.txt)\n"); return 0; }
    return real(cmd);
}
FILE *popen(const char *cmd, const char *type) {
    static FILE *(*real)(const char *, const char *);
    if (!real) real = dlsym(RTLD_NEXT, "popen");
    logf_("popen(%s)\n", cmd ? cmd : "(null)");
    return real(cmd, type);
}
