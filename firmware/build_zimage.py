#!/usr/bin/env python3
"""Build the q7-cam-mod kernel (zImage) from a stock Q7 kernel.

    python build_zimage.py <stock input> [output zImage]

<stock input> is either a stock zImage (e.g. zImage-stock from this folder) or a full
8 MB SPI flash dump of your own camera (the kernel is taken from offset 0x10000).
The output defaults to zImage-q7mod next to the input.

What it changes: one line of /etc/init.d/rcS inside the kernel's built-in initramfs.
Stock rcS ends with commented-out SD-card recorder lines and `/test/update.exe&`;
that block is replaced, in place and at the same length, by:

    mkdir /mnt/mmc; (try 3x: mount /dev/mmcblk0p1 /mnt/mmc);
    [ -f /mnt/mmc/boot.sh ] && sh /mnt/mmc/boot.sh || /test/update.exe &

so the camera runs boot.sh from the SD card if there is one, and boots exactly as
stock if there isn't. Everything else (the mod itself) lives on the SD card.

Why "at the same length": after the gzip stream the zImage carries a small trailer
table with hard-coded offsets of the gzip end and the image end. A rebuilt image must
therefore be byte-for-byte the same size, with the gzip no longer than the original
(padded with zeros). A shorter image soft-bricks the camera.
"""
import gzip
import hashlib
import io
import os
import struct
import sys
import zlib

ZIMAGE_MAGIC = 0x016F2818       # at +0x24 of an ARM zImage; image size at +0x2C
GZ_OFFSET = 0x36C4              # gzip(vmlinux) starts here in this camera's zImage
FLASH_SIZE = 0x800000           # 8 MB SPI NOR; kernel partition starts at 0x10000

STOCK_BLOCK = (b'#mount /dev/mmcblock0p1 /mnt\n#cd /mnt\n#./recorder -w 320 -h 240 -t 10 -p /mnt\n'
               b'grep -n "download=1" /proc/cmdline && /root/producer.sh\n/test/update.exe&')
INJECT = (b"mkdir /mnt/mmc;(for i in 1 2 3;do mount /dev/mmcblk0p1 /mnt/mmc&&break;sleep 2;done;"
          b"[ -f /mnt/mmc/boot.sh ]&&sh /mnt/mmc/boot.sh||/test/update.exe&)&")


def load_zimage(path):
    data = open(path, 'rb').read()
    base = 0x10000 if len(data) == FLASH_SIZE else 0
    if struct.unpack('<I', data[base + 0x24:base + 0x28])[0] != ZIMAGE_MAGIC:
        sys.exit('not a zImage (or an 8 MB flash dump with one at 0x10000): ' + path)
    end = struct.unpack('<I', data[base + 0x2C:base + 0x30])[0]
    return data[base:base + end]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(src)), 'zImage-q7mod')

    zimg = load_zimage(src)
    if zimg[GZ_OFFSET:GZ_OFFSET + 3] != b'\x1f\x8b\x08':
        sys.exit('no gzip at 0x%x: a different firmware than the one this was made for' % GZ_OFFSET)
    d = zlib.decompressobj(31)
    vm = bytearray(d.decompress(zimg[GZ_OFFSET:]))
    vm += d.flush()
    gz_used = len(zimg) - GZ_OFFSET - len(d.unused_data)
    trailer = zimg[GZ_OFFSET + gz_used:]

    if vm.count(INJECT):
        sys.exit('this kernel already has the q7-cam-mod hook')
    if vm.count(STOCK_BLOCK) != 1:
        sys.exit('stock rcS block not found exactly once: a different firmware than the one this was made for')
    patch = INJECT + b' ' * (len(STOCK_BLOCK) - len(INJECT))
    assert len(patch) == len(STOCK_BLOCK)
    off = vm.find(STOCK_BLOCK)
    vm[off:off + len(patch)] = patch
    print('[+] patched rcS at vmlinux+0x%x' % off)

    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0) as g:
        g.write(bytes(vm))
    newgz = buf.getvalue()
    if len(newgz) > gz_used:
        sys.exit('recompressed kernel is larger than the original (%d > %d): cannot keep the size' % (len(newgz), gz_used))
    newgz += b'\x00' * (gz_used - len(newgz))
    newz = zimg[:GZ_OFFSET] + newgz + trailer
    assert len(newz) == len(zimg)

    # check: only the rcS bytes differ after decompression
    chk = zlib.decompressobj(31).decompress(newz[GZ_OFFSET:])
    orig = zlib.decompressobj(31).decompress(zimg[GZ_OFFSET:GZ_OFFSET + gz_used])
    diff = [i for i in range(len(chk)) if chk[i] != orig[i]]
    assert len(chk) == len(orig) and diff and min(diff) >= off and max(diff) < off + len(patch)
    print('[+] same size as stock (%d bytes); %d bytes differ, all inside the rcS line' % (len(newz), len(diff)))

    open(out, 'wb').write(newz)
    print('[+] wrote %s  md5 %s' % (out, hashlib.md5(newz).hexdigest()))


if __name__ == '__main__':
    main()
