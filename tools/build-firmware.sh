#!/bin/sh
#
# build-firmware.sh: build Espressif's ESP32-S31 boot firmware (OpenSBI and
# U-Boot, unmodified) with the NetBSD cross toolchain.
#
# Outputs:
#   $W/build-opensbi/platform/generic/firmware/fw_dynamic.bin
#   $W/build-uboot/spl/u-boot-spl-dtb.bin   (flashed at 0x2000)
#   $W/build-uboot/u-boot.itb               (OpenSBI + U-Boot, at 0x100000)
#
# Host requirements: GNU make, bison, flex, python3 with pyelftools,
# OpenSSL headers (U-Boot host tools).
#
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
W=${W:-$REPO/work}
SRC=${SRC:-$W/netbsd-src}
TOOLS=${TOOLS:-$W/tools-rv32}
DEST=${DEST:-$W/dest-rv32}
J=${J:-$(nproc 2>/dev/null || echo 2)}
CROSS=riscv32--netbsd-
PATH=$TOOLS/bin:$PATH
export PATH

echo "=== OpenSBI"
make -C "$W/opensbi-esp" -j"$J" PLATFORM=generic \
    PLATFORM_DEFCONFIG=esp32s31_defconfig PLATFORM_RISCV_XLEN=32 \
    CROSS_COMPILE=$CROSS O="$W/build-opensbi" >/dev/null

# U-Boot is freestanding: it wants gcc's own <stddef.h> etc., while the
# NetBSD compiler's sysroot provides NetBSD's.  Give it gcc's headers and
# undefine __NetBSD__ so nothing picks NetBSD-specific paths.
GI=$W/gcc-include
mkdir -p "$GI"
for h in stddef.h stdarg.h stdbool.h float.h stdint-gcc.h stdnoreturn.h \
    stdalign.h iso646.h; do
	cp "$SRC/external/gpl3/gcc/dist/gcc/ginclude/$h" "$GI/"
done
cp "$GI/stdint-gcc.h" "$GI/stdint.h"

echo "=== U-Boot"
make -C "$W/uboot-esp" O="$W/build-uboot" CROSS_COMPILE=$CROSS \
    espressif_esp32s31_defconfig >/dev/null
make -C "$W/uboot-esp" -j"$J" O="$W/build-uboot" CROSS_COMPILE=$CROSS \
    NOSTDINC_FLAGS="-nostdinc -U__NetBSD__ -isystem $GI" \
    PLATFORM_LIBGCC="$DEST/usr/lib/libgcc.a" \
    OPENSBI="$W/build-opensbi/platform/generic/firmware/fw_dynamic.bin" \
    >/dev/null

ls -l "$W/build-uboot/spl/u-boot-spl-dtb.bin" "$W/build-uboot/u-boot.itb"
