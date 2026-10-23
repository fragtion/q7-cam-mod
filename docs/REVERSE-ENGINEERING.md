# Reverse-engineering log

How q7-cam-mod came about: from a camera running a closed, proprietary vendor app to an
open camera running our own daemon. It's written in roughly the order things happened,
including the dead ends, because the dead ends explain why the mod looks the way it does.
Hardware facts and the final design are summarised in [INTERNALS.md](INTERNALS.md).

All addresses are for the stock firmware in `firmware/zImage-stock`: `/test/LinuxIP.exe`
(an ET_EXEC ARM binary, fixed virtual addresses, file offset = vaddr - 0x8000 for `.text`)
and the on-device `libakuio.so.0.1.0` / `libakimage.so.0.1.0`.

---

## 1. Stock firmware: what we were dealing with

The camera ships with no open services at all: no RTSP, no plain HTTP stream, no telnet.
Everything goes through the vendor app (`LinuxIP.exe`) and its own proprietary protocol,
which only the vendor's phone apps ("9527" / "Plug&Play") and desktop client speak - on
the local network, over its own `Q7` hotspot, or relayed through the vendor's P2P
service. It also calls home to servers in China. Nothing else (Home Assistant, VLC, a
browser) can use the camera.

A full 8 MB SPI flash dump (read with an external SPI flasher) gave:

- **bootloader** (Anyka spiboot V16.05.03, 64 KB at 0x0) - left alone throughout;
- **kernel** at 0x10000: an ARM zImage whose gzip payload (at +0x36C4) contains vmlinux
  **with the whole root filesystem as a built-in cpio initramfs**. So there is no
  writable rootfs: every change to "the firmware" means rebuilding the kernel image;
- two small **config blocks** at the end of flash: the factory identity (the P2P ID and
  password from the label on the back) at 0x7FE000, and the user config (Wi-Fi SSID and
  password, written when you set up Wi-Fi from the app) at 0x7FF000.

Extracting the initramfs gave us the userland: busybox, `/etc/init.d/rcS`, and `/test`
with `update.exe` (boot-time helper), `LinuxIP.exe` (everything else) and a
`wpa_supplicant` config template.

### update.exe

Strings and a quick disassembly showed what it does at boot (in this order):

1. `mkdir /mnt/mmc` and mounts the SD card there;
2. **firmware update:** if `/mnt/mmc/update/zImage` exists it opens `/dev/mtd0`, writes
   the image into the kernel area (`update zimage file start ... end`), then runs
   `chmod 777 -R /mnt/mmc/update` and **`rm -rf /mnt/mmc/update`**. This is the stock
   "SD-card updater", and the reason the update folder vanishes after flashing;
3. **Wi-Fi:** reads `/mnt/mmc/wifi.txt` (SSID line 1, password line 2), and drives
   `wpa_cli set_network ...` / `udhcpc`; with no usable `wifi.txt` and nothing stored, it
   starts the `Q7` access point (WPA password `12345678`, 192.168.1.1);
4. also handles `/mnt/mmc/license.txt` (`sed -i '1d'`) and `/mnt/mmc/DDNS.txt`;
5. launches `/test/LinuxIP.exe`.

The SD-card updater is the key: it lets anyone flash a kernel with nothing but an SD card.

## 2. Getting a shell: the rcS patch

Since rcS lives inside the gzip'd kernel, the plan was: decompress vmlinux, edit rcS in
place, recompress, write it back. Stock rcS ends with some commented-out recorder lines
and the `update.exe` launch:

```
#mount /dev/mmcblock0p1 /mnt
#cd /mnt
#./recorder -w 320 -h 240 -t 10 -p /mnt
grep -n "download=1" /proc/cmdline && /root/producer.sh
/test/update.exe&
```

That block (154 bytes) is our budget.

- **v1** replaced it with a shorter line starting an `inetd` bind shell. The recompressed
  gzip came out shorter, the image shrank, and **the camera soft-bricked** (recovered with
  the SPI flasher). Cause: after the gzip stream the zImage has a 118-byte trailer table
  holding the offsets of the gzip end and the image end; the decompressor uses them.
- **v2** kept everything the same length: patch padded with spaces to exactly the original
  size, gzip -9 recompressed and zero-padded up to the original gzip length, trailer copied
  verbatim. Validation: decompress both, every differing byte must lie inside the rcS
  block. That booted, with a root shell on tcp/9999. **Every later image used this method**
  (`firmware/build_zimage.py`).
- **v6b/v7** moved all logic onto the SD card: the rcS line only mounts the card and runs
  `/mnt/mmc/boot.sh`, falling back to stock `update.exe` if that fails. v7 failed silently:
  rcS ran before `update.exe` had created `/mnt/mmc`, so the mount had nowhere to go and
  the camera booted stock.
- **v7b** (the released kernel) adds `mkdir /mnt/mmc` and a 3x2 s mount retry:

```sh
mkdir /mnt/mmc;(for i in 1 2 3;do mount /dev/mmcblk0p1 /mnt/mmc&&break;sleep 2;done;
[ -f /mnt/mmc/boot.sh ]&&sh /mnt/mmc/boot.sh||/test/update.exe&)&
```

From then on nothing ever needed reflashing: all development happened on the SD card.

## 3. First approach: piggybacking on LinuxIP

LinuxIP owns `/dev/video0`, and **restarting it corrupts the camera interface (CAMIF)**
(the driver is built in and can't be reloaded), so the first design left it running and
read its frames from the side.

### LinuxIP's capture loop

- capture thread entry `0xd9f8` (pthread created at `0xe01c`);
- `VIDIOC_DQBUF` (`0xc0445611`) at `0xdd38`, then the frame handler `0x142d4(w, h, buf)`,
  the JPEG wrapper `0xe418` (calls `Img_YUV2JPEG` at `0xe4c8`), then `VIDIOC_QBUF`
  (`0xc044560f`) at `0xde8c`;
- globals: V4L2 fd `0x747dc`, width `0x74f90`, height `0x74f94`, JPEG buffer `0x74fa0`;
- `struct v4l2_buffer` is 68 bytes here (`index@0`, `bytesused@8`, `m@52`, `length@56`).

The dequeued buffer index only exists on that thread's stack, so another process can't
cheaply know which buffer is current.

### Reading frames from physical memory

The CAMIF writes 4 rotating I420 320x240 buffers (115200 B) at physical
`0x30001000, 0x30028000, 0x3004f000, 0x30076000` (inside the reserved pmem pool
`0x30000000-0x30400000`). `piggy_hw` mapped them through `/dev/mem` and picked a
**complete** frame by sampling all four three times ~8 ms apart: a buffer that changed and
then went quiet has just finished (the CAMIF never pauses mid-frame), and it checked the
source hadn't changed during the copy. A free DMA-contiguous region
`0x301c0000-0x303f0000` (never written by LinuxIP) held its own buffers.

### Hardware JPEG

`libakimage`'s `Img_YUV2JPEG` drives the SoC's JPEG block (at `0x20040800`) after
`SetImageChipID(4)` and `Img_SetCallbackFuns(cb)`, a 27-pointer callback table we mapped
out by disassembling libakimage's uses of it (offsets in INTERNALS.md). Lessons:

- **Never touch codec registers through raw `/dev/mem`**: the block is unclocked and the
  bus hard-hangs. libakuio's UIO (`/dev/uio0`) mapping clocks it.
- The codec is a single non-reentrant block. A fork-per-connection server that encoded in
  every child produced truncated/garbage JPEGs under load; the fix was **one encoder
  process** writing into a shared ring that all connections read. camd keeps that design.
- `akuio_lock_block()`/`akuio_unlock()` (`akuio_file_lock` at `0x2474` = `fcntl F_WRLCK`)
  are only called by LinuxIP at `0x161c4`/`0x1eef0`, not around capture or JPEG, so there
  was no vendor lock to share.

### Patching LinuxIP itself

Two things in LinuxIP got in the way, so a statically patched copy was installed over
`/test/LinuxIP.exe` (in RAM) by boot.sh before `update.exe` launched it:

| vaddr | file offset | stock | patch | why |
|---|---|---|---|---|
| `0xf740` | `0x7740` | `rec_audio_init` prologue (`00482de9 04b08de2`) | `mov r0,#0; bx lr` | LinuxIP held the ALSA capture device; returning early freed the mic for our audio server |
| `0x1445c` | `0xc45c` | `bl 0x140dc` (`1effffeb`) | `mov r0,r0` (NOP) | the call that burns the date/time OSD into every captured frame |

(`nodate.c` did the OSD NOP on a running LinuxIP via ptrace before the static patch.)

This worked - MJPEG at ~21 fps with hardware JPEG, audio on 8081 - but LinuxIP still used
RAM and CPU, still ran its P2P client and calls home, and its reset-button handler kept deleting
`/mnt/mmc/wifi.txt` (see below). So the next step was to replace it.

## 4. Replacing LinuxIP: camd

The goal: our own process opens `/dev/video0` from boot, with LinuxIP never started.
boot.sh puts a do-nothing stub at `/test/LinuxIP.exe` so `update.exe` still brings up
Wi-Fi but launches the stub.

### Replaying LinuxIP's bring-up (ioshim)

A plain V4L2 open/S_FMT/REQBUFS/STREAMON gave **no frames at all** (`ak_camera` IRQ count
stayed 0), and often the board reset or froze a few seconds later. To see what LinuxIP
does differently we wrote **`ioshim.so`**, an LD_PRELOAD shim injected into `update.exe`
(and so into LinuxIP) for one boot, logging every open/ioctl/mmap on `/dev/video*`,
`/dev/ak*`, `/dev/uio*` and `/dev/snd*` with raw argument bytes, plus `system()`/`popen()`
commands, fsync'd per line so the log survived hangs.

The trace showed LinuxIP's exact sequence: a probe open/close of `/dev/video0`, GPIO setup,
CROPCAP/S_CROP/S_FMT, `REQBUFS` with **`V4L2_MEMORY_USERPTR`** (buffers from libakuio pmem),
a private `S_FMT` (`type=V4L2_BUF_TYPE_PRIVATE`, words 0 and 36 = 1), a couple of private
controls (`0x0098091f` colour effect = 0, `0x08000006` = 0), contrast/brightness, QBUF x4,
STREAMON - with ~0.3 s between setup steps. Replaying that still didn't give frames.

### The GPIOs

`/dev/akgpio` ioctls claim an argument size of 1 but really take a 16-byte struct; the
operand byte offset differs per operation, and a wrong offset silently does nothing
(table in INTERNALS.md, found in LinuxIP's helpers `0x1f3ac` set_as_gpio, `0x1f5dc`
set_dir, `0x1f514` set_pull, `0x1f450` set_level). set_pull must carry `0x02` ("leave
unchanged") in every byte except the pull byte, or it reconfigures the pin.

The pins LinuxIP touches are 3, 4, 14, 15, 16, 40 and 47. To find which one mattered,
ioshim gained `IOSHIM_BLOCK=<pins>`: it swallows LinuxIP's writes to the listed pins.
Blocking them one at a time (one boot each) showed:

- **pin 14 is the sensor's power-down line.** It's high at boot (sensor off) and must be
  driven low **before** `/dev/video0` is opened, because the driver configures the GC0308
  over I2C during open. Blocked, LinuxIP gets no frames either. That was the missing step.
- **pin 4** is what made the board reset: high while nobody services the camera resets
  the board within ~5-10 s (and holding it high resets the board after ~15-40 s even while
  capturing). Our reading of the analysis (`wdfeed.c` experiments): it arms an external
  watchdog that the vendor app fed by toggling pin 40 (also part of the IR-cut H-bridge).
  The red status LED lights while pin 4 is high. camd keeps pin 4 low - and uses it on
  purpose to reboot, since the kernel has no working `reboot` or sysrq.
- **3/40** pulse the IR-cut filter coil (one polarity for night, the other for day, then
  both released), **47** switches the IR LEDs (LinuxIP IR functions: LED-on `0x2004c`,
  LED-off `0x200a0`, IR-cut toggle `0x1ff40`).
- **15** must be low as well (high stops the sensor); **16** is read by LinuxIP's
  reset-button handler - which runs `rm -rf /mnt/mmc/wifi.txt`, so a spurious read leaves
  the camera off the network. boot.sh keeps a spare copy and restores it; ioshim also
  learned to refuse that command while LinuxIP was still in use.

Other findings along the way:

- the akgpio fd should stay open for the process lifetime, as LinuxIP does;
- **640x480 hangs the driver** right after STREAMON (LinuxIP only uses 320x240);
- never open or reconfigure `/dev/video0` while another process streams on it, and never
  kill a streaming LinuxIP: both reset the board;
- after a soft reset (pin 4) the SoC restarts but **the sensor doesn't**, and it then
  ignores the driver's setup: a washed-out white picture. camd power-cycles pin 14 at
  startup, and a self-test (step the sensor brightness to 0 and 6, check the picture
  follows) reboots if it still comes up stuck.

With pin 14 low first, camd got frames: 320x240 I420 into its own pmem buffers, encoded
by the hardware JPEG block, ~10 fps analysed while idle and up to 15 fps while streaming.

### Living with the capture driver

- The driver's `poll()` never signals: camd polls `DQBUF` non-blocking.
- After hours of running, the sensor/controller can stop raising interrupts (`ak_camera`
  in `/proc/interrupts` stops counting). DQBUF returns EAGAIN forever, and **STREAMOFF
  then blocks forever in `videobuf_waiton`** (uninterruptible), so the stream can't be
  restarted. camd's watchdog runs in its own thread and reboots after 10 s without
  frames (setting `auto_reboot`).
- The colour-effect control has its own numbering (1 sepia, 6 negative, 7 mono, 8+
  threshold); camd makes the night picture black-and-white by encoding with a neutral
  chroma plane instead, as the IR picture looks magenta in colour.

## 5. Audio

`audiod` uses raw ALSA ioctls (static musl, no libasound). The mic was silent until two
mixer controls were set through `/dev/snd/controlC0`: numid 5 "ADC Capture Route" = 6
(mic) and numid 3 "Mic Capture Volume" = 7. `struct snd_pcm_hw_params` is 604 bytes on
this 32-bit ARM (`fifo_size` is a 4-byte `unsigned long`). The ADC really runs at ~8060 Hz
rather than 8000, which is why the web UI's player resamples adaptively.

## 6. Motion detection

A first motion score (sum of absolute luma differences over a subsampled grid) couldn't
tell a person from noise in the dark IR picture. camd now averages each frame into a
40x30 grid and counts **changed blocks**: a block counts when its change exceeds both
`motion_block_level` and 3x that block's own learned flicker (an exponential average);
only blocks with a changed neighbour count (IR speckle is isolated); `motion_frames`
consecutive frames are needed; and frames whose overall brightness shifts by more than 4
(lights switching, IR toggling) are skipped while the noise model relearns.

## 7. Other discoveries

- **No reboot command.** `reboot` and sysrq don't work on this kernel; pin 4 (above) does.
- **Wi-Fi credentials end up in flash.** When Wi-Fi is set up, update.exe stores SSID and
  password in the config block at 0x7FF000 - that's why the published stock image has the
  config blocks erased.
- **SD card trouble.** An unclean power cut corrupted the FAT once; `fsck.fat` (dosfstools,
  built static) now ships in `q7mod/` for repairs from the shell. The rcS mount only waits
  3x2 s, so a very slow card can miss it (then the camera simply boots stock).
- **The MODE button** is not on a GPIO: it's the kernel's `analog-key` input device
  (`/dev/input/event0`, `KEY_MENU`).
- **LEDs.** Driving pin 4 lights red (and resets the board); the blue LED shows during
  boot and Wi-Fi activity. Both flash on their own at times; no GPIO we found drives
  them directly, so the mod doesn't use them.
- **Power.** `/sys/class/power_supply/battery` reports the voltage on the battery
  terminals and the driver's charge estimate; `usb`/`ac` report the charger input.
- **Time.** There's an RTC but the vendor stored local time in it; camd runs busybox
  `ntpd` with a hook that writes UTC to the RTC (`hwclock -w -u`) on every sync.
- **SD wear.** camd only writes the card for recordings and for settings changed in the
  web UI; Home Assistant's pushes use `save=0` (RAM only), and boot.sh remounts the card
  `noatime`.
