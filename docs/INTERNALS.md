# Q7 camera internals

Reverse-engineering notes behind q7-cam-mod: what the hardware is, how the stock firmware
boots, and the non-obvious things camd has to do. Useful if you want to change camd or port
the mod to a similar Anyka camera.

## Platform

| | |
|---|---|
| SoC / CPU | Anyka AK37 family (board marking **AK3751B**; the bootloader identifies as `spiboot V16.05.03`, `SUPERS3L`), ARM926EJ-S (ARMv5TE, soft-float), ~260 MHz |
| Kernel | Linux **2.6.32.9-ak37**; the root filesystem is a cpio initramfs built into the kernel image and unpacked to RAM at every boot |
| libc | **glibc 2.10.1**: binaries must not need anything newer than GLIBC_2.4 symbols (see `src/build.sh`) |
| RAM | 32 MB, ~24.8 MB usable, ~5 MB free with camd running |
| Flash | 8 MB SPI NOR (`/dev/mtd0` = all of it) |
| Sensor | GalaxyCore **GC0308** (VGA), run at **320x240** |
| Wi-Fi | Realtek RTL8188EU on the internal USB bus, `wlan0` |
| Audio | on-chip ADC/DAC (`ak37pcm`), mic only (no speaker fitted) |
| Device nodes | `/dev/video0` (V4L2 capture), `/dev/akgpio` (GPIO), `/dev/uio0` (codec registers), `/dev/snd/*` (ALSA), `/dev/input/event0` (the MODE button), `/dev/mmcblk0p1` (SD card) |

## Flash layout

```
0x000000-0x010000  bootloader (Anyka spiboot) - never touched by the mod
0x010000-0x5DE8F8  kernel: ARM zImage = decompressor + gzip(vmlinux with the initramfs)
                   zImage magic 0x016f2818 @+0x24, image size @+0x2C, gzip @+0x36C4,
                   then a 118-byte trailer table that hard-codes the gzip-end and
                   image-end offsets
0x5E0000-0x7FE000  erased (0xFF)
0x7FE000           factory identity: the P2P ID and password printed on the label
0x7FF000           user config written by the vendor app: Wi-Fi SSID/password etc.
```

Because of the trailer table a rebuilt zImage **must be exactly the same size** as stock,
with a gzip stream no longer than the original (zero-padded). `firmware/build_zimage.py`
does a same-length in-place patch and verifies it. A shorter image soft-bricks the camera.

## Boot chain

Stock `/etc/init.d/rcS` ends by starting the vendor's `/test/update.exe`, which:

1. mounts the SD card at `/mnt/mmc`;
2. if `/mnt/mmc/update/zImage` exists, **writes it to the kernel area of `/dev/mtd0`, then
   `rm -rf /mnt/mmc/update`** (this is the "SD-card updater" - the folder disappears once
   flashed, so the camera doesn't flash it again on the next boot);
3. reads `/mnt/mmc/wifi.txt` (SSID on line 1, password on line 2) and joins that network,
   or starts its own `Q7` access point (password `12345678`, camera at 192.168.1.1);
4. launches `/test/LinuxIP.exe`, the vendor app (capture, its proprietary app protocol and P2P service, SD recording).

The mod changes one line of rcS (in the kernel image) to:

```sh
mkdir /mnt/mmc;(for i in 1 2 3;do mount /dev/mmcblk0p1 /mnt/mmc&&break;sleep 2;done;
[ -f /mnt/mmc/boot.sh ]&&sh /mnt/mmc/boot.sh||/test/update.exe&)&
```

So with no SD card (or no `boot.sh` on it) the camera boots exactly like stock. `boot.sh`
replaces `/test/LinuxIP.exe` (it's in RAM) with a do-nothing stub when `q7mod/use_camd`
exists, then runs `update.exe` as usual - the updater and the Wi-Fi setup keep working -
and 15 s later `q7mod/pg.sh` starts camd and audiod.

## Capture (camd)

camd opens `/dev/video0` itself (standard V4L2, `V4L2_MEMORY_USERPTR` with pmem buffers
from libakuio). What it takes, found the hard way:

- **GPIO 14 is the sensor's power-down line** (high at boot = sensor off). It must be driven
  **low before `/dev/video0` is opened** - the driver configures the GC0308 over I2C on
  open; otherwise STREAMON never produces a frame. camd also power-cycles it (high 300 ms,
  then low) at startup: a reset restarts the SoC but not the sensor, which then ignores the
  driver's setup and gives a washed-out white picture.
- **GPIO 15** must be low too; driving it high stops the sensor.
- LinuxIP's bring-up order and pacing is replayed: a probe open/close of `/dev/video0`,
  IR pins parked, ~0.3 s between setup ioctls (`capture_start()` in camd.c).
- **640x480 hangs the driver** right after STREAMON. 320x240 only.
- **Never open/reconfigure `/dev/video0` while another process is streaming on it** - it
  resets the board.
- The colour-effect control `0x0098091f` uses the driver's own numbering (1 sepia,
  6 negative, 7 mono, 8+ threshold); camd makes its black-and-white night picture by
  encoding with a neutral (128) chroma plane instead.
- Rarely (every few hours to a day) the sensor/controller stops raising interrupts
  (`ak_camera` in `/proc/interrupts` stops counting) and DQBUF returns EAGAIN forever.
  **STREAMOFF then blocks forever** in `videobuf_waiton` (uninterruptible), so a capture
  restart is impossible; camd's watchdog thread reboots instead (setting `auto_reboot`).

## Rebooting

There is no working `reboot` or sysrq. **GPIO 4 high while nobody services the camera
resets the board** within ~10 s. camd's `/reboot` syncs, raises GPIO 4 and stops
servicing capture. From a shell: `gpiodump set 4 1` and stop the capturing process.
Holding GPIO 4 high resets the board even while capture is running (after ~15-40 s).

## GPIO (`/dev/akgpio`)

| Pin | Use |
|---|---|
| 3, 40 | IR-cut filter coil (H-bridge: pulse one polarity for night, the other for day, then release both) |
| 47 | IR LEDs |
| 14 | sensor power-down (active high) |
| 15 | keep low (sensor) |
| 4 | reset arming / status (see above) - leave low |

The red/blue LEDs are not driven directly by these pins; they appear to be run by the
vendor's power/status logic and aren't usable as indicators.

ioctl encoding (from LinuxIP's helpers): a 16-byte argument, pin as u32 at offset 0, the
operand **byte** at an op-specific offset (a wrong offset silently does nothing):

| op | ioctl | operand |
|---|---|---|
| set_as_gpio | `0x40016631` | - |
| set_dir | `0x40016635` | direction @8 and @9 (`0` = output) |
| set_pull | `0x40016632` | pull @11 (`0` = off); **every other byte must be `0x02`** ("leave unchanged") |
| set_level | `0x40016633` | level @10 |
| get_level | `0x40016634` / `0x40016636` | result in the struct |

Keep one `/dev/akgpio` fd open for the process lifetime (as LinuxIP does).

## The MODE button

The kernel's `analog-key` input device, `/dev/input/event0`, reports it as `KEY_MENU`
(139): value 1 press, 2 auto-repeat while held, 0 release. (It is not on a GPIO.)

## Hardware JPEG (libakimage)

`Img_YUV2JPEG(Y, U, V, out, &size, w, h, quality)` after `SetImageChipID(4)` and
`Img_SetCallbackFuns(cb)` with a 27-pointer callback table (byte offsets):

```
0x1c malloc   0x20 free   0x44 stub   0x48 sysreg_write   0x4c alloc_pmem
0x50 free_pmem   0x54 vaddr2paddr   0x58 map_regs   0x5c unmap_regs   0x60 lock_block   0x64 unlock
```

The pmem/UIO functions come from libakuio (`src/libs`). **Never touch the codec registers
through raw `/dev/mem`**: the block is unclocked and the bus hard-hangs; libakuio's UIO
mapping clocks it. The codec is one non-reentrant block, so camd runs a **single encoder**
publishing into a ring that every HTTP client reads.

## Audio (audiod)

Raw ALSA ioctls (no libasound), static musl. Capture `/dev/snd/pcmC0D0c`, **S16_LE mono
~8 kHz** (the ADC really clocks ~8060 Hz). `struct snd_pcm_hw_params` is 604 bytes on this
ARM32. Mixer via `/dev/snd/controlC0`: numid 5 "ADC Capture Route" = 6 (mic) and numid 3
"Mic Capture Volume" = 7, or the mic is silent. audiod serves the stream on port 8081 and
shares its sound level with camd through `/tmp/.sound`.

## Power

`/sys/class/power_supply/battery` (`voltage_now` in mV, `capacity` %, `status`) reads the
battery terminals; `usb/online` and `ac/online` are the charger input. With a bench supply
wired to the battery terminals and no battery, `voltage_now` is the supply voltage and the
percentage means nothing.

## Wi-Fi

Signal level in `/proc/net/wireless`. `wpa_supplicant` and `udhcpc` are started by
`update.exe` from `wifi.txt`. The vendor app's reset handler deletes `/mnt/mmc/wifi.txt`
on a (sometimes spurious) reset-button read, which is why boot.sh keeps a spare copy in
`q7mod/wifi.txt.keep` and restores it.

## ioshim (RE helper)

`src/ioshim.c` is an LD_PRELOAD tracer that logs the vendor app's open/ioctl/mmap calls on
the camera devices (with raw ioctl arguments) and can block chosen GPIO writes
(`IOSHIM_BLOCK=pin,pin`). That's how the GPIO 14 requirement was found. Build with
`bash src/build.sh ioshim`, put `ioshim.so` on the card and preload it into `update.exe`
from boot.sh for one boot.
