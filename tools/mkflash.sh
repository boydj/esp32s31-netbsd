#!/bin/sh
#
# mkflash.sh: assemble a complete 16MiB-flash image for the ESP32-S31 using
# Espressif's BSP layout (esp-linux-bsp configs/esp32s31-layout.cfg):
#
#   0x002000  SPL app (U-Boot SPL in Espressif image format)
#   0x100000  u-boot.itb (OpenSBI fw_dynamic + U-Boot + U-Boot dtb)
#   0x300000  device tree for the kernel  (XIP 0x40200000)
#   0x500000  kernel Image                (XIP 0x40400000, booti), <= 4MiB
#   0x900000  root file system (FFS)      (XIP 0x40800000), <= 7MiB
#
# usage: mkflash.sh u-boot-spl-dtb.bin u-boot.itb kernel.dtb kernel.img \
#            rootfs.img out.bin
#
set -eu

SPL=$1 ITB=$2 DTB=$3 IMG=$4 ROOTFS=$5 OUT=$6
TOOLDIR=${TOOLDIR:?set TOOLDIR to the NetBSD tool directory}
CROSS=${CROSS:-$TOOLDIR/bin/riscv32--netbsd-}
ESPTOOL=${ESPTOOL:-esptool}
# Needs an esptool that knows the ESP32-S31 (5.x, which needs Python 3.10+).
if ! "$ESPTOOL" --chip esp32s31 elf2image --help >/dev/null 2>&1; then
	echo "mkflash.sh: $ESPTOOL missing or without ESP32-S31 support;" \
	    "install esptool 5.x (pip install 'esptool>=5', Python 3.10+)" >&2
	exit 1
fi
SPL_SRAM_ADDR=0x2F040000
# DIO/80m is mandatory: elf2image's QIO/40m default makes the ROM read
# garbage before the SPL's own QIO bring-up.
FLASHARGS="--flash-mode dio --flash-freq 80m --flash-size 16MB"

# file size in bytes (portable: GNU and BSD stat disagree)
fsize() { wc -c < "$1" | tr -d ' '; }

toobig() {	# file limit what
	if [ "$(fsize "$1")" -gt "$2" ]; then
		echo "mkflash.sh: $3 too big ($(fsize "$1") > $2 bytes)" >&2
		exit 1
	fi
}
toobig "$IMG" $((0x900000 - 0x500000)) "kernel image"
toobig "$ROOTFS" $((0x1000000 - 0x900000)) "root file system"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Wrap the flat SPL (with its appended control dtb) in a single-segment
# ELF at the address the ROM loads it to, then make an ESP image of it.
${CROSS}objcopy -I binary -O elf32-littleriscv -B riscv "$SPL" "$tmp/spl1.elf"
${CROSS}objcopy --change-section-address ".data=$SPL_SRAM_ADDR" \
    --rename-section .data=.text,alloc,load,readonly,code,contents \
    --set-start "$SPL_SRAM_ADDR" "$tmp/spl1.elf" "$tmp/spl.elf"
$ESPTOOL --chip esp32s31 elf2image $FLASHARGS -o "$tmp/spl_app.bin" \
    "$tmp/spl.elf"

$ESPTOOL --chip esp32s31 merge-bin -o "$OUT" --format raw $FLASHARGS \
    0x002000 "$tmp/spl_app.bin" \
    0x100000 "$ITB" \
    0x300000 "$DTB" \
    0x500000 "$IMG" \
    0x900000 "$ROOTFS"
echo "wrote $OUT ($(fsize "$OUT") bytes)"
