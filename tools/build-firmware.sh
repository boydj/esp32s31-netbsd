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
# Host requirements: GNU make >= 4, bison, flex, swig, OpenSSL headers,
# python3 with setuptools and pyelftools (U-Boot's binman builds its own
# libfdt Python module).  On macOS (Homebrew):
#   brew install make bison flex swig openssl@3 pkgconf python
#   $(brew --prefix)/bin/python3 -m venv .venv && . .venv/bin/activate &&
#       pip install setuptools pyelftools esptool pexpect
#
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
W=${W:-$REPO/work}
SRC=${SRC:-$W/netbsd-src}
TOOLS=${TOOLS:-$W/tools-rv32}
DEST=${DEST:-$W/dest-rv32}
J=${J:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
CROSS=riscv32--netbsd-

die() { echo "build-firmware.sh: $*" >&2; exit 1; }

# U-Boot needs GNU make >= 4 (macOS ships 3.81 as "make"; Homebrew's
# GNU make is "gmake").
GMAKE=
for m in ${GMAKE_CMD:-} gmake make; do
	command -v "$m" >/dev/null 2>&1 || continue
	v=$("$m" --version 2>/dev/null | sed -n '1s/^GNU Make \([0-9]*\).*/\1/p')
	if [ -n "$v" ] && [ "$v" -ge 4 ]; then
		GMAKE=$m
		break
	fi
done
[ -n "$GMAKE" ] || die "GNU make >= 4 not found (macOS: brew install make)"

# Homebrew keeps bison, flex and OpenSSL out of the default paths.
if [ "$(uname -s)" = Darwin ] && command -v brew >/dev/null 2>&1; then
	for f in bison flex; do
		p=$(brew --prefix "$f" 2>/dev/null || true)
		[ -n "$p" ] && [ -d "$p/bin" ] && PATH=$p/bin:$PATH
	done
	p=$(brew --prefix openssl@3 2>/dev/null || true)
	if [ -n "$p" ] && [ -d "$p/include/openssl" ]; then
		HOSTCFLAGS="${HOSTCFLAGS:-} -I$p/include"
		HOSTLDFLAGS="${HOSTLDFLAGS:-} -L$p/lib"
		PKG_CONFIG_PATH=$p/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
		export HOSTCFLAGS HOSTLDFLAGS PKG_CONFIG_PATH
	fi
fi

missing=
for t in bison flex swig python3; do
	command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
python3 -c 'import setuptools, elftools' 2>/dev/null ||
	missing="$missing python3-setuptools/pyelftools"
[ -z "$missing" ] || die "missing host tools:$missing (see the top of this script)"

PATH=$TOOLS/bin:$PATH
export PATH

echo "=== OpenSBI"
$GMAKE -C "$W/opensbi-esp" -j"$J" PLATFORM=generic \
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
# Host-side build fixes (uboot/patches), applied once.
for p in "$REPO"/uboot/patches/*.patch; do
	[ -f "$p" ] || continue
	git -C "$W/uboot-esp" apply --reverse --check "$p" 2>/dev/null ||
	    git -C "$W/uboot-esp" apply "$p"
done
$GMAKE -C "$W/uboot-esp" O="$W/build-uboot" CROSS_COMPILE=$CROSS \
    espressif_esp32s31_defconfig >/dev/null
$GMAKE -C "$W/uboot-esp" -j"$J" O="$W/build-uboot" CROSS_COMPILE=$CROSS \
    NOSTDINC_FLAGS="-nostdinc -U__NetBSD__ -isystem $GI" \
    PLATFORM_LIBGCC="$DEST/usr/lib/libgcc.a" \
    OPENSBI="$W/build-opensbi/platform/generic/firmware/fw_dynamic.bin" \
    >/dev/null

ls -l "$W/build-uboot/spl/u-boot-spl-dtb.bin" "$W/build-uboot/u-boot.itb"
