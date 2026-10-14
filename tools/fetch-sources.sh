#!/bin/sh
#
# fetch-sources.sh: check out everything needed to build the image.
#
#   netbsd-src   NetBSD with the ESP32-S31 port (esp32s31 branch)
#   opensbi-esp  Espressif OpenSBI, ESP32-S31 integration branch (pinned)
#   uboot-esp    Espressif U-Boot,  ESP32-S31 integration branch (pinned)
#   qemu         QEMU v11.1.2 + the esp32s31 machine (qemu/patches)
#   esp-idf      ESP-IDF (pinned), for the Wi-Fi/Bluetooth co-processor
#                firmware (espcp/); NO_ESPCP=1 skips it
#
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
W=${W:-$REPO/work}

NETBSD_URL=${NETBSD_URL:-https://github.com/boydj/netbsd-src.git}
NETBSD_BRANCH=${NETBSD_BRANCH:-esp32s31}
OPENSBI_URL=https://github.com/espressif/opensbi.git
OPENSBI_BRANCH=integration/v1.6-esp32s31
OPENSBI_REV=5395e03e1cb295257142cfdeaade6334f1543bcb
UBOOT_URL=https://github.com/espressif/u-boot.git
UBOOT_BRANCH=integration/v2024.07-esp32s31
UBOOT_REV=b2ec9ae21622f92d35462f5ae3bf053466998cc6
QEMU_URL=${QEMU_URL:-https://github.com/qemu/qemu.git}
QEMU_TAG=v11.1.2
IDF_URL=https://github.com/espressif/esp-idf.git
IDF_REV=ab95e70748f2eddac6f87217b0cf62533684a6ba

mkdir -p "$W"
cd "$W"

[ -d netbsd-src ] ||
	git clone --depth 1 -b "$NETBSD_BRANCH" "$NETBSD_URL" netbsd-src

pinned() {	# dir url rev	(the revision tested with; see *_BRANCH)
	if [ ! -d "$1" ]; then
		git init -q "$1"
		git -C "$1" remote add origin "$2"
	fi
	git -C "$1" fetch -q --depth 1 origin "$3"
	git -C "$1" checkout -q FETCH_HEAD
}
pinned opensbi-esp "$OPENSBI_URL" "$OPENSBI_REV"
pinned uboot-esp "$UBOOT_URL" "$UBOOT_REV"

if [ ! -d qemu ]; then
	git clone --depth 1 -b "$QEMU_TAG" "$QEMU_URL" qemu
	git -C qemu -c user.name=esp32s31-netbsd -c user.email=nobody@invalid \
	    am -q "$REPO"/qemu/patches/*.patch
	# QEMU's subprojects come from gitlab.com by default; set
	# QEMU_WRAP_MIRROR=github where only GitHub is reachable.
	if [ "${QEMU_WRAP_MIRROR:-}" = github ]; then
		for f in qemu/subprojects/*.wrap; do
			sed 's|https://gitlab.com/qemu-project/|https://github.com/qemu/|' \
			    "$f" > "$f.tmp" && mv "$f.tmp" "$f"
		done
	fi
fi
if [ "${NO_ESPCP:-0}" != 1 ]; then
	pinned esp-idf "$IDF_URL" "$IDF_REV"
	git -C esp-idf submodule update -q --init --recursive --depth 1
fi
echo "sources in $W"
