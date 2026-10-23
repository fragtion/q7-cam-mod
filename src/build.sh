#!/bin/bash
# Build the camera binaries into src/out/. Run on Linux (or WSL):  bash src/build.sh [target...]
# Targets: camd audiod gpiodump ioshim (default: camd audiod gpiodump).
#
# Two toolchains (override the paths with BTL=... MUSL=...):
#   glibc, for camd (links the vendor JPEG/UIO libs in src/libs) and ioshim (LD_PRELOAD):
#     Bootlin armv5-eabi glibc "stable-2018.11-1" -> binaries need only GLIBC_2.4, which the
#     camera's glibc 2.10 has. (Newer gnueabi toolchains emit GLIBC_2.34 symbols: won't load.)
#     https://toolchains.bootlin.com/  (armv5-eabi, glibc, stable 2018.11-1)
#   musl, static, for audiod and gpiodump: any arm-linux-musleabi cross gcc (e.g. musl.cc).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"; mkdir -p "$OUT"
BTL=${BTL:-/opt/btl/armv5-eabi--glibc--stable-2018.11-1/bin}
MUSL=${MUSL:-/opt/xtool/arm-linux-musleabi-cross/bin}
CFLAGS="-march=armv5te -mfloat-abi=soft -O2 -Wall"

glibc_hw() {   # $1 = name (uses $1.c, links the vendor JPEG/UIO libs from src/libs)
    (cd "$HERE/libs" && ln -sf libakimage.so.0.1.0 libakimage.so && ln -sf libakuio.so.0.1.0 libakuio.so)
    "$BTL/arm-linux-gcc" $CFLAGS -fno-stack-protector -U_FORTIFY_SOURCE \
        "$HERE/$1.c" -L"$HERE/libs" -lakimage -lakuio -lpthread \
        -Wl,--allow-shlib-undefined,-rpath-link="$HERE/libs" -o "$OUT/$1"
    "$BTL/arm-linux-strip" "$OUT/$1"
    echo "$1: $(stat -c %s "$OUT/$1") bytes, needs $("$BTL/arm-linux-readelf" --dyn-syms "$OUT/$1" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
}
glibc_preload() {   # $1 = name -> $1.so
    "$BTL/arm-linux-gcc" $CFLAGS -fno-stack-protector -U_FORTIFY_SOURCE -shared -fPIC "$HERE/$1.c" -ldl -o "$OUT/$1.so"
    "$BTL/arm-linux-strip" "$OUT/$1.so"
    echo "$1.so: $(stat -c %s "$OUT/$1.so") bytes"
}
static_musl() {
    "$MUSL/arm-linux-musleabi-gcc" -static -no-pie -fno-pie $CFLAGS "$HERE/$1.c" -lm -o "$OUT/$1"
    "$MUSL/arm-linux-musleabi-strip" "$OUT/$1"
    echo "$1: $(stat -c %s "$OUT/$1") bytes (static)"
}

targets=("$@"); [ ${#targets[@]} -eq 0 ] && targets=(camd audiod gpiodump)
for t in "${targets[@]}"; do
    [ -f "$HERE/$t.c" ] || { echo "skip $t (no source)"; continue; }
    case $t in camd) glibc_hw "$t" ;; ioshim) glibc_preload "$t" ;; *) static_musl "$t" ;; esac
done
