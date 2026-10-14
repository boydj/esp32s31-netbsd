#!/bin/sh
#
# build-espcp.sh: build the Wi-Fi/Bluetooth co-processor firmware (espcp/),
# an ESP-IDF application that runs on the ESP32-S31's second hart next to
# NetBSD, and pack it into the image NetBSD's espcp(4) loads (flashed at
# 0xd00000 by mkflash.sh).
#
# Needs the ESP-IDF checkout fetch-sources.sh makes ($W/esp-idf; it gets
# espcp/idf-patches/ applied).  Its compiler and Python environment come
# from IDF's install.sh/export.sh, run here into $W/espressif unless the
# environment already has them (or IDF_ENV names a script that sets them
# up).
#
# Outputs:
#   $W/build-espcp/espcp.elf
#   $W/build-espcp/espcp.img
#
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
W=${W:-$REPO/work}
B=${B:-$W/build-espcp}
IDF_PATH=${IDF_PATH:-$W/esp-idf}
export IDF_PATH

die() { echo "build-espcp.sh: $*" >&2; exit 1; }

[ -d "$IDF_PATH/components" ] || die "no ESP-IDF at $IDF_PATH (fetch-sources.sh)"
if [ -n "${IDF_ENV:-}" ]; then
	. "$IDF_ENV"
elif ! command -v riscv32-esp-elf-gcc >/dev/null 2>&1; then
	# ESP-IDF's own tools (compiler, Python environment), in the work
	# directory rather than ~/.espressif.
	IDF_TOOLS_PATH=${IDF_TOOLS_PATH:-$W/espressif}
	export IDF_TOOLS_PATH
	[ -d "$IDF_TOOLS_PATH/python_env" ] ||
		"$IDF_PATH/install.sh" esp32s31 >/dev/null
	. "$IDF_PATH/export.sh" >/dev/null
fi
command -v idf.py >/dev/null 2>&1 || PATH=$IDF_PATH/tools:$PATH
command -v riscv32-esp-elf-gcc >/dev/null 2>&1 ||
	die "riscv32-esp-elf-gcc not in PATH (run ESP-IDF's export.sh)"

# The co-processor patches, applied once.
for p in "$REPO"/espcp/idf-patches/*.patch; do
	[ -f "$p" ] || continue
	git -C "$IDF_PATH" apply --reverse --check "$p" 2>/dev/null ||
	    git -C "$IDF_PATH" apply "$p"
done

# The protocol header is shared with the kernel; they must agree.
SRC=${SRC:-$W/netbsd-src}
if [ -f "$SRC/sys/arch/riscv/espressif/esp_cpproto.h" ] &&
    ! cmp -s "$SRC/sys/arch/riscv/espressif/esp_cpproto.h" \
	"$REPO/espcp/components/espcp_sys/include/esp_cpproto.h"; then
	die "espcp's esp_cpproto.h differs from the kernel's"
fi

cd "$REPO/espcp"
[ -f "$B/CMakeCache.txt" ] ||
	idf.py --preview -B "$B" -D SDKCONFIG="$B/sdkconfig" set-target esp32s31
idf.py --preview -B "$B" -D SDKCONFIG="$B/sdkconfig" build

IDENT="espcp $(git -C "$REPO" describe --tags --always --dirty 2>/dev/null || echo 1.0)"
python3 "$REPO/espcp/mkimage.py" --ident "$IDENT" "$B/espcp.elf" "$B/espcp.img"
