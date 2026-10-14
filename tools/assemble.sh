#!/bin/sh
#
# assemble.sh: kernel + root file system + device tree + firmware -> a
# complete 16MiB flash image ($W/out/s31_netbsd_flash.bin), flashable at
# offset 0.
#
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
W=${W:-$REPO/work}
SRC=${SRC:-$W/netbsd-src}
OBJ=${OBJ:-$W/obj-rv32}
TOOLS=${TOOLS:-$W/tools-rv32}
OUT=${OUT:-$W/out}
T=$TOOLS/bin
export TOOLDIR=$TOOLS NETBSDSRCDIR=$SRC

mkdir -p "$OUT"
# The Wi-Fi/Bluetooth firmware (tools/build-espcp.sh), if it has been built.
ESPCP_IMG=${ESPCP_IMG:-$W/build-espcp/espcp.img}
[ -f "$ESPCP_IMG" ] || ESPCP_IMG=
cp "$OBJ/sys/arch/riscv/compile/ESP32S31/netbsd" "$OUT/netbsd-ESP32S31"
cp "$OBJ/distrib/riscv/esp32s31/esp32s31-root.fs" "$OUT/esp32s31-root.fs"
"$T/nbdtc" -I dts -O dtb -o "$OUT/esp32s31.dtb" \
    "$SRC/sys/arch/riscv/espressif/esp32s31.dts"
"$REPO/tools/mkloader.sh" "$OUT/netbsd-ESP32S31" "$OUT/netbsd-esp32s31.img"
"$REPO/tools/mkflash.sh" "$W/build-uboot/spl/u-boot-spl-dtb.bin" \
    "$W/build-uboot/u-boot.itb" "$OUT/esp32s31.dtb" \
    "$OUT/netbsd-esp32s31.img" "$OUT/esp32s31-root.fs" \
    "$OUT/s31_netbsd_flash.bin" ${ESPCP_IMG:+"$ESPCP_IMG"} 2>&1 |
    grep -v "^esptool\|HINT\|SHA digest"
