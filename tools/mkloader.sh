#!/bin/sh
#
# mkloader.sh: wrap a NetBSD/riscv32 ESP32S31 kernel (ELF) in the
# esp32s31-loader, producing a RISC-V "Image" that Espressif's U-Boot can
# booti from the flash XIP window.
#
# usage: mkloader.sh netbsd output.img
#
set -eu

TOOLDIR=${TOOLDIR:?set TOOLDIR to the NetBSD tool directory}
SRC=${NETBSDSRCDIR:?set NETBSDSRCDIR to the NetBSD source tree}
CROSS=${CROSS:-$TOOLDIR/bin/riscv32--netbsd-}
KERNEL=$1
OUT=$2

# file size in bytes (portable: GNU and BSD stat disagree)
fsize() { wc -c < "$1" | tr -d ' '; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

${CROSS}objcopy -O binary "$KERNEL" "$tmp/netbsd.bin"
# Pad to a multiple of 4 bytes.
sz=$(fsize "$tmp/netbsd.bin")
pad=$(( (4 - sz % 4) % 4 ))
[ $pad -ne 0 ] && dd if=/dev/zero bs=1 count=$pad >> "$tmp/netbsd.bin" 2>/dev/null
ksize=$(fsize "$tmp/netbsd.bin")

start=$(${CROSS}nm "$KERNEL" | awk '$3 == "start" { print $1 }')
end=$(${CROSS}nm "$KERNEL" | awk '$3 == "_end" { print $1 }')
memsz=$(( 0x$end - 0x$start ))
load=$(${CROSS}readelf -l "$KERNEL" | awk '$1 == "LOAD" { print $4; exit }')

echo "kernel: load $load, file $ksize bytes, memory $memsz bytes"

${CROSS}gcc -c -o "$tmp/loader.o" -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
    -DKERNEL_LOAD=$load -DKERNEL_SIZE=$ksize -DKERNEL_MEMSZ=$memsz \
    -DKERNEL_BIN="\"$tmp/netbsd.bin\"" \
    "$SRC/sys/arch/riscv/espressif/loader/loader.S"
${CROSS}ld -o "$tmp/loader.elf" -Ttext=0 -e _start --no-relax "$tmp/loader.o"
${CROSS}objcopy -O binary "$tmp/loader.elf" "$OUT"
echo "wrote $OUT ($(fsize "$OUT") bytes)"
