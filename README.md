# NetBSD on the Espressif ESP32-S31

A native NetBSD/riscv32 port to the **ESP32-S31**, Espressif's RISC-V
part with a real Sv32 MMU and 16 MiB of PSRAM.  It runs a real kernel
with virtual memory, processes and a multi-user login, not an emulator.
It is booted by Espressif's own, unmodified boot chain:

```
mask ROM -> U-Boot SPL -> OpenSBI -> U-Boot (booti) -> esp32s31-loader -> NetBSD
```

This repository holds the build and flash tooling, a QEMU machine model
of the chip and the tests.  The NetBSD side of the port is the
[`esp32s31` branch of boydj/netbsd-src](https://github.com/boydj/netbsd-src/tree/esp32s31).
It is a short series of commits on top of NetBSD-current, and
`sys/arch/riscv/espressif/README.esp32s31` there has the technical notes.

## Status

**Boots to a multi-user login in two independent emulators.  It has not
been run on silicon yet.**

| | |
|---|---|
| QEMU (`-M esp32s31`, `qemu/` here) | boots, multi-user, all tests pass |
| [esp-emulator](https://github.com/espressif/esp-emulator) v0.49.0, real ESP32-S31 mask ROM | boots, multi-user, all tests pass |
| Real hardware | untested |

What works: the Sv32 MMU with fork/exec and paging out of memory (OOM
kills are handled), the CLIC and interrupt matrix, the SYSTIMER
(timecounter, hardclock, `delay(9)`), the UART0 console (interrupt-driven
RX/TX, tested with a 20 KB input flood), the hardware TRNG as an entropy
source, and warm reboot via SBI.  The ramdisk has `sh`, `vi`, `awk`,
`sed`, `grep`, `less`, `ps`, `vmstat`, `sysctl`, ... with tmpfs on
`/tmp` and `/var`.

Not supported yet: the second hart (SMP), the FPU (userland is
soft-float), Wi-Fi/BLE, Ethernet, SD/MMC, using the flash as a disk,
GPIO, the USB Serial/JTAG console, and an RTC.

```
ESP-ROM:esp32s31-20251218
load:0x2f040000,len:0x94a4
U-Boot SPL 2024.07-gb2ec9ae2
OpenSBI: ESP32-S31 firmware active
U-Boot 2024.07-gb2ec9ae2
Starting kernel ...
esp32s31-loader: copying NetBSD kernel to PSRAM
[   1.0000000] NetBSD 11.99.9 (ESP32S31) #4
[   1.0000000] total memory = 11012 KB
[   1.0000000] espclic0 at simplebus1: ESP32-S31 CLIC (S-mode ids 16-47)
[   1.0000000] espintmtx0 at simplebus1: ESP32-S31 interrupt matrix
[   1.0000000] espsystimer0 at simplebus1: ESP32-S31 system timer, 16.000 MHz, intmtx source 33
[   1.0000000] espuart0 at simplebus1: ESP32 UART, console
[   1.0000000] esptrng0 at simplebus1: ESP32-S31 TRNG
[   1.0000000] entropy: ready
[   1.1896716] root on md0a dumps on md0b
Starting NetBSD on the Espressif ESP32-S31

NetBSD/riscv (esp32s31) (console)

login:
```

(This log is from esp-emulator, running Espressif's mask ROM.)

## Flashing a board

You need an ESP32-S31 board with 16 MiB flash and 16 MiB PSRAM, and
esptool 5.x:

```sh
esptool --chip esp32s31 --port /dev/ttyUSB0 --baud 1152000 \
    write-flash 0x0 s31_netbsd_flash.bin
```

The console is UART0 at 115200 8N1.  Log in as `root` (no password).
The root file system is a memory disk: changes are lost at reset.

The image uses the flash layout of Espressif's Linux BSP, so it replaces
whatever was there:

| flash offset | contents |
|---|---|
| `0x002000` | U-Boot SPL (ESP image, DIO/80 MHz) |
| `0x100000` | `u-boot.itb`: OpenSBI `fw_dynamic` + U-Boot |
| `0x300000` | device tree (`esp32s31.dts`) |
| `0x500000` | `netbsd-esp32s31.img`: loader + kernel + 3 MiB ramdisk |

## Building

```sh
./build-all.sh          # everything, into ./work
```

or step by step (`W` selects the work directory, `J` the parallelism):

| script | does |
|---|---|
| `tools/fetch-sources.sh` | clones the NetBSD branch, Espressif OpenSBI/U-Boot (pinned), QEMU v11.1.2 + `qemu/patches` |
| `tools/build-netbsd.sh` | cross toolchain, the few static libraries the ramdisk needs, the crunched ramdisk, the `ESP32S31` kernel |
| `tools/build-firmware.sh` | OpenSBI and U-Boot, with the NetBSD toolchain |
| `tools/assemble.sh` | kernel + ramdisk → loader image → `work/out/s31_netbsd_flash.bin` |
| `tools/build-qemu.sh` | `qemu-system-riscv32` with the `esp32s31` machine |

Host requirements: a C/C++ compiler, git, GNU make 4 or later, bison,
flex, swig, OpenSSL headers, Python 3 with `setuptools`, `pyelftools`,
`esptool` 5.x and `pexpect` (tests), and for QEMU: ninja, pkg-config,
glib and pixman.  If gitlab.com is unreachable, set
`QEMU_WRAP_MIRROR=github` for QEMU's subprojects.

On **macOS** (Homebrew), the system `make` is GNU make 3.81, too old for
U-Boot, and the system `python3` (3.9) is too old for esptool 5, which is
the first with ESP32-S31 support.  The scripts pick up Homebrew's `gmake`
and its keg-only bison, flex and OpenSSL automatically:

```sh
brew install make bison flex swig openssl@3 pkgconf ninja glib pixman python
$(brew --prefix)/bin/python3 -m venv --clear .venv && . .venv/bin/activate
pip install setuptools pyelftools 'esptool>=5' pexpect
./build-all.sh
```

The toolchain build dominates (30–60 minutes).  Everything after that
takes a few minutes.

## Testing in an emulator

```sh
# QEMU model
test/boot-test.py --reboots 2 --vi
test/boot-test.py --machopts ",asid=off"     # a core without ASIDs
test/rx-flood.py                             # UART RX stress

# Espressif's esp-emulator (real mask ROM)
test/boot-test.py --esp-emu ~/.local/bin/esp-emu --reboots 1 --vi
```

`boot-test.py` boots the image, logs in, runs commands (pass your own as
arguments), optionally edits a file with `vi` over the console and
warm-reboots, then halts.  To play with it interactively:

```sh
work/qemu/build/qemu-system-riscv32 -M esp32s31,flash=work/out/s31_netbsd_flash.bin \
    -nographic -serial mon:stdio -monitor none
esp-emu --chip esp32s31 --firmware work/out/s31_netbsd_flash.bin --net user
```

The QEMU machine (`qemu/patches`) models the parts NetBSD and the boot
firmware use: CLIC-mode harts (stvec alignment, SPIL in scause, no
sie/sip), the CLIC, interrupt matrix, SYSTIMER, UART0, TRNG, the reset
register and the flash XIP window.  It emulates the SPL step itself: it
parses `u-boot.itb` and starts OpenSBI the way the SPL would.  esp-emulator
runs the real mask ROM and SPL instead.  That makes it the more
independent check of the boot path and the flash image format.

## How it fits together

* **Kernel**: `sys/arch/riscv/espressif` in the NetBSD branch.  CLIC
  (`espclic`, `espintmtx`), SYSTIMER (`espsystimer`), UART (`espuart`),
  TRNG (`esptrng`), the FDT platform code, and `loader/loader.S`.  The
  loader turns the kernel into a RISC-V `Image` that U-Boot can `booti`
  straight from the flash XIP window, then copies it to PSRAM.
* **Generic riscv changes** in the same branch: soft-float riscv32
  userland, Sv32 direct map and other RV32 VM fixes, `options RISCV_CLIC`,
  pluggable timer/`delay(9)`, a `.bss` clearing fix in locore.  To check
  they don't regress riscv64, `GENERIC64` was built and booted on QEMU's
  `virt` machine (2 harts) up to the root device prompt.
* **Firmware**: Espressif's OpenSBI and U-Boot, built with the NetBSD
  toolchain (`tools/build-firmware.sh`).  The firmware code is unmodified.
  `uboot/patches` only fixes U-Boot's host-side Python libfdt binding for
  SWIG 4.3 and later.

## Caveats for the first run on silicon

The emulators agree, but these have only been checked against Espressif's
source code, not against hardware:

* **ASIDs.**  The kernel uses them.  `asid=off` in QEMU checks that it
  still works when the core implements none.
* **Caches.**  I-cache maintenance goes through OpenSBI's vendor
  extension; there is no D-cache maintenance beyond that.
* **PMA/APM.**  These are left as OpenSBI and U-Boot configure them.
* **TRNG.**  Entropy is credited at 1 bit per byte, after health tests.
* **Console.**  The UART divisor is left as the ROM and U-Boot set it
  (115200).

If it doesn't come up, the first line to look for after U-Boot's
`Starting kernel ...` is `esp32s31-loader: copying NetBSD kernel to PSRAM`.

## License

The scripts and tests here are under the BSD 2-clause license (`LICENSE`).
The QEMU patches are GPL-2.0-or-later, like QEMU.  The NetBSD code is
under NetBSD's usual BSD license.
