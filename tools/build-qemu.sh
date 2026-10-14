#!/bin/sh
#
# build-qemu.sh: build qemu-system-riscv32 with the "esp32s31" machine
# (QEMU v11.1.2 + qemu/patches from this repository, applied by
# fetch-sources.sh).
#
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
W=${W:-$REPO/work}
J=${J:-$(nproc 2>/dev/null || echo 2)}

mkdir -p "$W/qemu/build"
cd "$W/qemu/build"
[ -f build.ninja ] || ../configure --target-list=riscv32-softmmu \
    --disable-docs --disable-werror --enable-slirp --disable-gtk \
    --disable-sdl --disable-vnc --disable-opengl --disable-xen \
    --disable-kvm --disable-tools --disable-guest-agent
ninja -j"$J" qemu-system-riscv32
ls -l "$W/qemu/build/qemu-system-riscv32"
