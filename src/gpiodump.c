/* gpiodump — read (and optionally set) /dev/akgpio pins, to compare GPIO state with
 * and without LinuxIP running.
 *
 *   gpiodump                 print raw get_level results for the pins LinuxIP touches
 *   gpiodump set PIN LEVEL   make PIN an output at LEVEL (same calls as piggy_hw's gpio_out)
 *
 * The akgpio ioctls take a 16-byte struct with the pin as u32 at offset 0; the operand
 * byte offset differs per op (see INTERNALS.md). For get_level we print the whole
 * struct afterwards, since the result offset isn't documented.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GAS 0x40016631u
#define GDR 0x40016635u
#define GPE 0x40016632u
#define GLV 0x40016633u
#define GGET 0x40016634u
#define GGET2 0x40016636u

static int fd;

static void op(unsigned cmd, int pin, int off, int val, unsigned char *out) {
    unsigned char s[16]; memset(s, cmd == GPE ? 2 : 0, 16);   /* 0x02 = leave unchanged (as LinuxIP) */
    s[0] = pin; s[1] = pin >> 8;
    if (off >= 0) s[off] = val;
    int r = ioctl(fd, cmd, s);
    if (out) { memcpy(out, s, 16); out[15] = r < 0 ? 0xee : out[15]; }
}

int main(int argc, char **argv) {
    fd = open("/dev/akgpio", O_RDWR);
    if (fd < 0) { perror("/dev/akgpio"); return 1; }
    if (argc == 4 && !strcmp(argv[1], "set")) {
        int pin = atoi(argv[2]), lvl = atoi(argv[3]);
        op(GAS, pin, -1, 0, 0); op(GDR, pin, 8, 0, 0); op(GPE, pin, 11, 0, 0); op(GLV, pin, 10, lvl, 0);
        printf("pin %d -> %d\n", pin, lvl);
        return 0;
    }
    int pins[] = { 3, 4, 14, 15, 16, 40, 47 };
    for (unsigned i = 0; i < sizeof pins / sizeof *pins; i++) {
        unsigned char a[16], b[16];
        op(GGET, pins[i], -1, 0, a);
        op(GGET2, pins[i], -1, 0, b);
        printf("pin %2d get:", pins[i]); for (int k = 0; k < 16; k++) printf(" %02x", a[k]);
        printf("  get2:"); for (int k = 0; k < 16; k++) printf(" %02x", b[k]); printf("\n");
    }
    return 0;
}
