#!/bin/sh
#
# build-netbsd.sh: build the NetBSD/riscv32 toolchain, the static libraries
# the ramdisk needs, the crunched ramdisk and the ESP32S31 kernel.
#
# This deliberately does not run "build.sh distribution": only a minimal,
# static userland is needed (see ../mk.conf), and building just that is
# much faster.
#
# Environment (all optional):
#   W      work directory            (default: <repo>/work)
#   SRC    NetBSD source tree        (default: $W/netbsd-src)
#   OBJ    object directory          (default: $W/obj-rv32)
#   TOOLS  toolchain directory       (default: $W/tools-rv32)
#   DEST   destination directory     (default: $W/dest-rv32)
#   J      parallel jobs             (default: number of CPUs)
#
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
W=${W:-$REPO/work}
SRC=${SRC:-$W/netbsd-src}
OBJ=${OBJ:-$W/obj-rv32}
TOOLS=${TOOLS:-$W/tools-rv32}
DEST=${DEST:-$W/dest-rv32}
REL=${REL:-$W/rel-rv32}
J=${J:-$(nproc 2>/dev/null || echo 2)}
MAKECONF=$REPO/mk.conf
NB=$OBJ/nbmake-riscv32

mkdir -p "$OBJ"
cd "$SRC"
B="./build.sh -U -u -j$J -m riscv -a riscv32 -O $OBJ -T $TOOLS -D $DEST
    -R $REL -w $NB -V MAKECONF=$MAKECONF"

step() { echo "=== $*"; }

if [ ! -x "$TOOLS/bin/riscv32--netbsd-gcc" ]; then
	step "toolchain"
	$B tools
fi
$B makewrapper >/dev/null

step "distribution directories and headers"
$NB -C "$SRC" do-distrib-dirs >/dev/null
$NB -j"$J" -C "$SRC" includes >/dev/null

# Static libraries, in dependency order.  LIBS in
# distrib/riscv/esp32s31/list must be a subset of these.
for d in \
    lib/csu \
    external/gpl3/gcc/lib/libgcc \
    lib/libc \
    lib/libutil \
    lib/libterminfo \
    lib/libcurses \
    lib/libedit \
    lib/libkvm \
    lib/libcrypt \
    lib/libm \
    lib/libprop \
    lib/libbz2 \
    lib/libz
do
	step "$d"
	(cd "$SRC/$d" && $NB obj >/dev/null && $NB -j"$J" dependall >/dev/null &&
	    $NB install >/dev/null)
done

step "MAKEDEV"
(cd "$SRC/etc" && $NB obj >/dev/null && $NB MAKEDEV >/dev/null)

step "ramdisk"
(cd "$SRC/distrib/riscv/esp32s31" && $NB obj >/dev/null && $NB -j"$J" >/dev/null)

step "kernel"
$B kernel=ESP32S31 >/dev/null

echo "kernel:  $OBJ/sys/arch/riscv/compile/ESP32S31/netbsd"
echo "ramdisk: $OBJ/distrib/riscv/esp32s31/esp32s31-root.fs"
