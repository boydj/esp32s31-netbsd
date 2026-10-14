#!/bin/sh
#
# build-all.sh: fetch the sources and build everything:
#
#   work/out/s31_netbsd_flash.bin          complete flash image (offset 0)
#   work/qemu/build/qemu-system-riscv32    emulator with -M esp32s31
#
# Takes a while: the NetBSD cross toolchain alone is ~30-60 minutes.
# Set W to use a different work directory, J for parallelism.
#
set -eu

REPO=$(cd "$(dirname "$0")" && pwd)
export W=${W:-$REPO/work}

"$REPO/tools/fetch-sources.sh"
"$REPO/tools/build-netbsd.sh"
"$REPO/tools/build-firmware.sh"
"$REPO/tools/assemble.sh"
"$REPO/tools/build-qemu.sh"

cat <<EOF

Flash image: $W/out/s31_netbsd_flash.bin

Try it:   $REPO/test/boot-test.py --qemu $W/qemu/build/qemu-system-riscv32 \\
              --flash $W/out/s31_netbsd_flash.bin
Flash it: esptool --chip esp32s31 --port PORT --baud 1152000 \\
              write-flash 0x0 $W/out/s31_netbsd_flash.bin
EOF
