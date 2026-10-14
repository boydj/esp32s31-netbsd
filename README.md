# NetBSD on the Espressif ESP32-S31

A native NetBSD/riscv32 port to the **ESP32-S31**, Espressif's RISC-V
part with a real Sv32 MMU and 16 MiB of PSRAM.  It runs a real kernel
with virtual memory, processes, networking, USB and a multi-user login,
not an emulator.  It is booted by Espressif's own, unmodified boot
chain:

```
mask ROM -> U-Boot SPL -> OpenSBI -> U-Boot (booti) -> esp32s31-loader -> NetBSD
```

This repository holds the build and flash tooling, a QEMU machine model
of the chip and the tests.  The NetBSD side of the port is the
[`esp32s31` branch of boydj/netbsd-src](https://github.com/boydj/netbsd-src/tree/esp32s31).
It is a series of commits on top of NetBSD-current, and
`sys/arch/riscv/espressif/README.esp32s31` there has the technical notes.

## Status

**Boots to a multi-user login, with networking, in two independent
emulators.  It has not been run on silicon yet.**

| | |
|---|---|
| QEMU (`-M esp32s31`, `qemu/` here) | boots, multi-user, all tests pass |
| [esp-emulator](https://github.com/espressif/esp-emulator) v0.49.0, real ESP32-S31 mask ROM | boots, multi-user, all tests pass (it has no USB OTG model) |
| Real hardware | untested |

What works:

* **Core:** the Sv32 MMU with fork/exec and paging out of memory (OOM
  kills are handled), the CLIC and interrupt matrix, the SYSTIMER
  (timecounter, hardclock, `delay(9)`), and warm reboot via SBI.
* **FPU:** the single-precision FPU.  FP state is saved per process,
  lazily when the core traps FP instructions with `sstatus.FS` off and
  eagerly when it doesn't.  `fptest` checks it.
* **Root file system:** read straight out of SPI flash through the XIP
  window, so all of PSRAM is left for the kernel and processes.
* **Consoles:** UART0 (interrupt-driven, tested with a 20 KB input
  flood) and the USB-Serial/JTAG port, the board's "USB" connector,
  which the host sees as a CDC-ACM serial port.  Either can be the
  console, and the other gets a login prompt.
* **Ethernet:** gigabit Ethernet (DesignWare GMAC, RGMII), with clocks,
  resets, pin muxing, the PHY reset GPIO and the factory MAC address from
  eFuse.  `dhcpcd` runs at boot, and the clock is set over SNTP once
  there is a lease, since the chip has no RTC.  Tools: `ping`, `ftp`
  (HTTP), `nc`, `route`, `arp`, `netstat`.
* **USB 2.0 host** (DesignWare HS OTG with its UTMI PHY):
  * USB sticks: `mount -t msdos /dev/sd0e /mnt`
  * USB serial adapters: `cu -l /dev/dtyU0 -s 115200`
  * USB Ethernet adapters, which `dhcpcd` configures when they're plugged in
  * hubs
* **GPIO** through `gpioctl(8)`.
* **Entropy:** the hardware TRNG is the entropy source.

The root file system has `sh`, `vi`, `awk`, `sed`, `grep`, `less`, `ps`,
`vmstat`, `sysctl`, ... and is read-only; `/tmp`, `/var` and `/root` are
tmpfs.  About 13 MiB of RAM is free for the system after the kernel.

Not supported yet:

* the second hart (SMP)
* Wi-Fi and BLE (there is no public programming information)
* SD/MMC
* writing to the flash
* USB device (gadget) mode

```
ESP-ROM:esp32s31-20251218
load:0x2f040000,len:0x94a4
U-Boot SPL 2024.07-gb2ec9ae2
OpenSBI: ESP32-S31 firmware active
U-Boot 2024.07-gb2ec9ae2
Starting kernel ...
esp32s31-loader: copying NetBSD kernel to PSRAM
[   1.0000000] NetBSD 11.99.9 (ESP32S31) #16
[   1.0000000] total memory = 13196 KB
[   1.0000000] cpu0: FPU: single precision (F), eager (FS=Off does not trap) context switching
[   1.0000000] espclic0 at simplebus1: ESP32-S31 CLIC (S-mode ids 16-47)
[   1.0000000] espintmtx0 at simplebus1: ESP32-S31 interrupt matrix
[   1.0000000] espgpio0 at simplebus1: ESP32-S31 GPIO, 62 pins (11 reserved)
[   1.0000000] espsystimer0 at simplebus1: ESP32-S31 system timer, 16.000 MHz, intmtx source 33
[   1.0000000] espuart0 at simplebus1: ESP32 UART, console
[   1.0000000] esptrng0 at simplebus1: ESP32-S31 TRNG
[   1.0000000] awge0: Ethernet address 24:0a:c4:00:00:01
[   1.0000000] espusj0 at simplebus1: ESP32-S31 USB-Serial/JTAG
[   1.0000000] dwctwo0 at simplebus1: DesignWare USB2 OTG
[   1.2496692] root on md0a dumps on md0b
Starting NetBSD on the Espressif ESP32-S31

NetBSD/riscv (esp32s31) (console)

login:
```

(This log is from esp-emulator, which runs Espressif's mask ROM.)

## Flashing a board

You need an ESP32-S31 board with 16 MiB flash and 16 MiB PSRAM, and
esptool 5.x:

```sh
esptool --chip esp32s31 --port /dev/ttyUSB0 --baud 1152000 \
    write-flash 0x0 s31_netbsd_flash.bin
```

The console is UART0 at 115200 8N1.  There is also a login prompt on the
USB-Serial/JTAG port: `/dev/ttyACM0` on Linux, `/dev/cu.usbmodem*` on
macOS.  To make the USB port the console instead, set
`stdout-path = "serial1:115200n8"` in `esp32s31.dts`; the UART then gets
the extra login prompt.  Log in as `root` (no password).

The root file system is read-only, and `/tmp`, `/var` and `/root` are in
memory, so changes are lost at reset.  Plug the Ethernet port into a
network with a DHCP server and the board configures itself.  It then
sets its clock from the DHCP server's NTP servers, `pool.ntp.org` or the
router.

The image uses the flash layout of Espressif's Linux BSP, with the root
file system after the kernel, so it replaces whatever was there:

| flash offset | contents |
|---|---|
| `0x002000` | U-Boot SPL (ESP image, DIO/80 MHz) |
| `0x100000` | `u-boot.itb`: OpenSBI `fw_dynamic` + U-Boot |
| `0x300000` | device tree (`esp32s31.dts`) |
| `0x500000` | `netbsd-esp32s31.img`: loader + kernel (at most 4 MiB) |
| `0x900000` | root file system, FFS (at most 7 MiB) |

## Building

```sh
./build-all.sh          # everything, into ./work
```

Or step by step (`W` selects the work directory, `J` the parallelism):

| script | does |
|---|---|
| `tools/fetch-sources.sh` | clones the NetBSD branch, Espressif's OpenSBI and U-Boot (pinned), and QEMU v11.1.2 + `qemu/patches` |
| `tools/build-netbsd.sh` | the cross toolchain, the few static libraries the root file system needs, the crunched root file system and the `ESP32S31` kernel |
| `tools/build-firmware.sh` | OpenSBI and U-Boot, with the NetBSD toolchain |
| `tools/assemble.sh` | kernel → loader image, then the image and root file system → `work/out/s31_netbsd_flash.bin` |
| `tools/build-qemu.sh` | `qemu-system-riscv32` with the `esp32s31` machine |

Host requirements:

* a C/C++ compiler, git, GNU make 4 or later, bison, flex, swig and
  OpenSSL headers
* Python 3 with `setuptools`, `pyelftools`, `esptool` 5.x, and `pexpect`
  for the tests
* for QEMU: ninja, pkg-config, glib, pixman and libslirp (for
  `-nic user`)

If gitlab.com is unreachable, set `QEMU_WRAP_MIRROR=github` for QEMU's
subprojects.

On **macOS** (Homebrew), the system `make` is GNU make 3.81, too old for
U-Boot.  The system `python3` (3.9) is too old for esptool 5, the first
version with ESP32-S31 support.  The scripts pick up Homebrew's `gmake`
and its keg-only bison, flex and OpenSSL automatically:

```sh
brew install make bison flex swig openssl@3 pkgconf ninja glib pixman libslirp python
$(brew --prefix)/bin/python3 -m venv --clear .venv && . .venv/bin/activate
pip install setuptools pyelftools 'esptool>=5' pexpect
./build-all.sh
```

The toolchain build takes most of the time (30–60 minutes); everything
after it takes a few minutes.

## Testing in an emulator

```sh
# QEMU model
test/boot-test.py --reboots 2 --vi --fptest --net --usb --usj --gpio
test/boot-test.py --machopts ",asid=off" --net --usb   # a core without ASIDs
test/rx-flood.py                                       # UART RX stress

# Espressif's esp-emulator (real mask ROM)
test/boot-test.py --esp-emu ~/.local/bin/esp-emu --reboots 1 --vi --fptest --net --gpio
```

`boot-test.py` boots the image, logs in, runs commands (pass your own as
arguments), then halts.  Options add checks:

| option | checks |
|---|---|
| `--fptest` | runs the FPU test |
| `--net` | DHCP and ping over the emulated Ethernet |
| `--usb` | a USB stick, a USB Ethernet adapter and an FTDI serial adapter (QEMU only) |
| `--usj` | logs in over the USB-Serial/JTAG port (QEMU only) |
| `--gpio` | toggles a GPIO pin |
| `--vi` | edits a file with `vi` over the console |
| `--reboots N` | warm-reboots N times |

To use it interactively:

```sh
work/qemu/build/qemu-system-riscv32 -M esp32s31,flash=work/out/s31_netbsd_flash.bin \
    -nographic -serial mon:stdio -serial pty -monitor none -nic user
esp-emu --chip esp32s31 --firmware work/out/s31_netbsd_flash.bin --net user
```

In QEMU, the second `-serial` is the USB-Serial/JTAG port.  QEMU's
user-mode network is 10.0.2.0/24 and esp-emulator's is 192.168.4.0/24;
in both, the gateway forwards to the host's loopback.

The QEMU machine (`qemu/patches`) models the parts NetBSD and the boot
firmware use:

* CLIC-mode harts: stvec alignment, SPIL in scause, no sie/sip
* the CLIC, interrupt matrix, SYSTIMER, UART0, TRNG and reset register
* the flash XIP window
* the Ethernet MAC: QEMU's NPCM GMAC model, which is the same DesignWare
  core, taught the S31's enhanced descriptors
* the USB OTG controller: QEMU's dwc2 host model, as on the Raspberry Pi
* the USB-Serial/JTAG port and the GPIO registers

QEMU emulates the SPL step itself: it parses `u-boot.itb` and starts
OpenSBI the way the SPL would.  esp-emulator runs the real mask ROM and
SPL instead, and models Espressif's own peripherals.  That makes it the
more independent check of the boot path, the flash image format and the
Ethernet MAC.  It has no USB OTG model: there `dwctwo0` fails to reset
the core, and the rest of the system carries on.

## How it fits together

* **Kernel:** `sys/arch/riscv/espressif` in the NetBSD branch:
  * the CLIC (`espclic`, `espintmtx`) and SYSTIMER (`espsystimer`)
  * the UART (`espuart`) and USB-Serial/JTAG port (`espusj`)
  * the TRNG (`esptrng`), clocks and resets (`espclkrst`), and GPIO and
    pin muxing (`espgpio`)
  * the Ethernet glue for `awge` (`esp_gmac.c`)
  * the USB PHY (`espusbphy`); the controller is the stock `dwctwo`
  * the flash root file system (`espxiproot`)
  * the FDT platform code and `loader/loader.S`.  The loader turns the
    kernel into a RISC-V `Image` that U-Boot can `booti` straight from
    the flash XIP window, then copies it to PSRAM.
* **Generic changes** in the same branch:
  * a soft-float riscv32 userland
  * the Sv32 direct map and other RV32 VM fixes
  * `options RISCV_CLIC`
  * a pluggable timer and `delay(9)`
  * a `.bss` clearing fix in locore
  * FPU support for cores with F but not D
  * D-cache hooks for non-coherent DMA
  * a few driver options (dwc_gmac burst modes and enhanced descriptors,
    the dwc2 reset)

  To check they don't regress riscv64, `GENERIC64` was built and booted
  on QEMU's `virt` machine (2 harts) up to the root device prompt.
* **Firmware:** Espressif's OpenSBI and U-Boot, built with the NetBSD
  toolchain (`tools/build-firmware.sh`).  The firmware code is
  unmodified; `uboot/patches` only fixes the host-side build (SWIG 4.3+,
  macOS, binman without `pkg_resources`).

## Caveats for the first run on silicon

The emulators agree, but these points have only been checked against
Espressif's source code, not against hardware:

* **ASIDs.**  The kernel uses them.  `asid=off` in QEMU checks that it
  still works when the core implements none.
* **Caches.**  I-cache and D-cache maintenance go through OpenSBI's
  vendor extension.  DMA (Ethernet, USB) is treated as non-coherent:
  `bus_dma` writes back and invalidates PSRAM lines around every transfer.
* **Ethernet.**  The pin mux, RGMII clocking and PHY reset follow
  Espressif's Linux device tree for the devkit (Motorcomm YT8531 PHY).
* **USB.**  The PHY power-up sequence and the controller parameters
  follow Espressif's Linux drivers.  VBUS on the host port is whatever
  the board provides.
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
