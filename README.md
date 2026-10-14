# NetBSD on the Espressif ESP32-S31

A native NetBSD/riscv32 port to the **ESP32-S31**, Espressif's RISC-V
part with a real Sv32 MMU and 16 MiB of PSRAM.  It runs a real kernel
with virtual memory, processes, Ethernet, Wi-Fi, Bluetooth, USB, an SD
card and a multi-user login, not an emulator.  It is booted by
Espressif's own boot chain:

```
mask ROM -> U-Boot SPL -> OpenSBI -> U-Boot (booti) -> esp32s31-loader -> NetBSD (hart 0)
                                                                          \-> ESP-IDF Wi-Fi/Bluetooth firmware (hart 1)
```

The chip's radio has no public programming information, so NetBSD runs
Espressif's own Wi-Fi and Bluetooth stacks, an ESP-IDF application, on
the chip's second hart, and talks to them as to a radio module (see
[Wi-Fi and Bluetooth](#wi-fi-and-bluetooth)).

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
| QEMU (`-M esp32s31`, `qemu/` here) | boots, multi-user, all tests pass; Wi-Fi and Bluetooth through a model of the co-processor |
| [esp-emulator](https://github.com/espressif/esp-emulator) v0.49.0, real ESP32-S31 mask ROM | boots, multi-user, all tests pass, including Wi-Fi with the real ESP-IDF firmware on hart 1 (it has no USB OTG or SD model, and can't emulate Bluetooth on hart 1) |
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
* **SD card** (DesignWare MSHC, 4-bit, up to 50 MHz) as `ld0`:
  `mount -t msdos /dev/ld0e /mnt`, `newfs_msdos`, `fdisk`.
* **Wi-Fi** (station; WPA2 and WPA3): `espwifictl scan`,
  `espwifictl join SSID`, and `dhcpcd` does the rest.
* **Bluetooth** (BR/EDR and LE controller) under NetBSD's `bluetooth(4)`
  stack: `btconfig espbt0 up`, `btconfig espbt0 inquiry`, `sdpquery`,
  `rfcomm_sppd`.
* **GPIO** through `gpioctl(8)`.
* **Entropy:** the hardware TRNG is the entropy source.

The root file system has `sh`, `vi`, `awk`, `sed`, `grep`, `less`, `ps`,
`vmstat`, `sysctl`, ... and is read-only; `/tmp`, `/var` and `/root` are
tmpfs.  About 12 MiB of RAM is free for the system after the kernel and
the radio firmware's share.

Not supported yet:

* SMP: the second hart runs the radio firmware
* Wi-Fi access point mode, and Bluetooth LE from NetBSD (its stack is
  BR/EDR only)
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
[   1.0000000] NetBSD 11.99.9 (ESP32S31) #22
[   1.0000000] total memory = 13044 KB
[   1.0000000] cpu0: FPU: single precision (F), eager (FS=Off does not trap) context switching
[   1.0000000] espclic0 at simplebus1: ESP32-S31 CLIC (S-mode ids 16-47)
[   1.0000000] espintmtx0 at simplebus1: ESP32-S31 interrupt matrix
[   1.0000000] espgpio0 at simplebus1: ESP32-S31 GPIO, 62 pins (11 reserved)
[   1.0000000] espsystimer0 at simplebus1: ESP32-S31 system timer, 16.000 MHz, intmtx source 33
[   1.0000000] espuart0 at simplebus1: ESP32 UART, console
[   1.0000000] esptrng0 at simplebus1: ESP32-S31 TRNG
[   1.0000000] espxiproot0 at simplebus1: 4096 KB file system in flash
[   1.0000000] espcp0 at simplebus1: radio co-processor (hart 1)
[   1.0000000] awge0: Ethernet address 24:0a:c4:00:00:04
[   1.0000000] espusj0 at simplebus1: ESP32-S31 USB-Serial/JTAG
[   1.0000000] dwcmmc0 at simplebus1: DesignWare SD/MMC
[   1.0000000] dwctwo0 at simplebus1: DesignWare USB2 OTG
[   1.2596627] espcp0: espcp 1.1, 1169 KiB, 1024 KiB PSRAM at 0x503a7000
[   1.2996404] espcp0: espcp 1.1, ESP-IDF ab95e707 running, interrupting on intmtx source 67
[   1.2996404] espwifi0 at espcp0: Wi-Fi station, address 24:0a:c4:00:00:01
[   1.2996404] espbt0 at espcp0: Bluetooth BR/EDR + LE controller, address 24:0a:c4:00:00:02
[   1.2996404] root on md0a dumps on md0b
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
network with a DHCP server, or join a Wi-Fi network, and the board
configures itself.  It then sets its clock from the DHCP server's NTP
servers, `pool.ntp.org` or the router.

The image uses the flash layout of Espressif's Linux BSP, with the root
file system after the kernel, so it replaces whatever was there:

| flash offset | contents |
|---|---|
| `0x002000` | U-Boot SPL (ESP image, DIO/80 MHz) |
| `0x100000` | `u-boot.itb`: OpenSBI `fw_dynamic` + U-Boot |
| `0x300000` | device tree (`esp32s31.dts`) |
| `0x500000` | `netbsd-esp32s31.img`: loader + kernel (at most 4 MiB) |
| `0x900000` | root file system, FFS (at most 4 MiB) |
| `0xd00000` | `espcp.img`: Wi-Fi/Bluetooth firmware for hart 1 (at most 3 MiB) |

## Wi-Fi and Bluetooth

Wi-Fi is the `espwifi0` interface.  `espwifictl` finds and joins
networks; the link comes up once associated, and `dhcpcd` (already
running) takes it from there:

```
# espwifictl scan
SSID                              BSSID               CH  RSSI  SECURITY
myssid                            26:0a:c4:00:00:01    1   -30  WPA2/WPA3
# espwifictl join myssid            (asks for the passphrase; "-" for none)
Passphrase (- for none):
# espwifictl wait
espwifi0: associated with "myssid" (26:0a:c4:00:00:01), channel 1, -30 dBm, WPA3
# espwifictl status
# espwifictl leave
```

`espwifictl join SSID PASSPHRASE` takes the passphrase on the command
line, `-b BSSID` picks one access point and `-a WPA3` refuses anything
weaker.  The firmware keeps reconnecting until told to `leave`, and the
network is joined again when the interface comes back up.  To join at
boot, put `ssid="..."` and `psk="..."` in `/etc/espwifi.conf` (in the
image: `distrib/riscv/esp32s31/etc`), since the root file system is
read-only.

Bluetooth is `espbt0` in NetBSD's Bluetooth stack:

```
# btconfig espbt0 up
# btconfig espbt0 inquiry
# sdpquery -a 11:22:33:44:55:66 search SP
# rfcomm_sppd -a 11:22:33:44:55:66 -s SP      (stdin/stdout over RFCOMM)
```

The controller is off until `btconfig ... up`.  `bthcid` runs from boot
and answers pairing requests: give it a PIN for a device with
`btpin -d espbt0 -a 11:22:33:44:55:66 -p 1234`.  Link keys live in
`/var/db/bthcid.keys`, so they are forgotten at reset.

The firmware's log is in `dmesg` (lines starting `espcp0:`), and
`vmstat -e | grep espcp` counts its messages.

## Building

```sh
./build-all.sh          # everything, into ./work
```

Or step by step (`W` selects the work directory, `J` the parallelism):

| script | does |
|---|---|
| `tools/fetch-sources.sh` | clones the NetBSD branch, Espressif's OpenSBI, U-Boot and ESP-IDF (pinned), and QEMU v11.1.2 + `qemu/patches` |
| `tools/build-netbsd.sh` | the cross toolchain, the few static libraries the root file system needs, the crunched root file system and the `ESP32S31` kernel |
| `tools/build-firmware.sh` | OpenSBI and U-Boot, with the NetBSD toolchain |
| `tools/build-espcp.sh` | the Wi-Fi/Bluetooth firmware (`espcp/`) with ESP-IDF: installs IDF's toolchain into `work/espressif`, applies `espcp/idf-patches`, builds and packs `espcp.img` |
| `tools/assemble.sh` | kernel → loader image, then the image, root file system and radio firmware → `work/out/s31_netbsd_flash.bin` |
| `tools/build-qemu.sh` | `qemu-system-riscv32` with the `esp32s31` machine |

Host requirements:

* a C/C++ compiler, git, GNU make 4 or later, bison, flex, swig and
  OpenSSL headers
* Python 3 with `setuptools`, `pyelftools`, `esptool` 5.x, and `pexpect`
  for the tests
* for QEMU: ninja, pkg-config, glib, pixman and libslirp (for
  `-nic user`)
* for the radio firmware: what ESP-IDF's `install.sh` needs (Python 3.10+
  with venv, CMake, ninja); it fetches ESP-IDF's own RISC-V compiler.
  `NO_ESPCP=1 ./build-all.sh` leaves Wi-Fi and Bluetooth out.

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
test/boot-test.py --reboots 2 --vi --fptest --net --usb --usj --gpio --sd
test/boot-test.py --wifi --bt --bumble-python .venv-bumble/bin/python
test/boot-test.py --machopts ",asid=off" --net --usb   # a core without ASIDs
test/rx-flood.py                                       # UART RX stress

# Espressif's esp-emulator (real mask ROM)
test/boot-test.py --esp-emu ~/.local/bin/esp-emu --reboots 1 --vi --fptest --net --gpio
test/boot-test.py --esp-emu ~/.local/bin/esp-emu --wifi
```

`--wifi` downloads a file over HTTP from the host: serve one first with
`mkdir -p /tmp/httproot && cd /tmp/httproot && head -c 200000 /dev/urandom
> blob.bin && sha256sum blob.bin > blob.sha && python3 -m http.server
8088`.  `--bt` needs [Bumble](https://github.com/google/bumble)
(`python3 -m venv .venv-bumble && .venv-bumble/bin/pip install bumble`):
`test/bt-peer.py` serves a virtual controller and a second device with a
serial port service to talk to.

`boot-test.py` boots the image, logs in, runs commands (pass your own as
arguments), then halts.  Options add checks:

| option | checks |
|---|---|
| `--fptest` | runs the FPU test |
| `--net` | DHCP and ping over the emulated Ethernet |
| `--usb` | a USB stick, a USB Ethernet adapter and an FTDI serial adapter (QEMU only) |
| `--usj` | logs in over the USB-Serial/JTAG port (QEMU only) |
| `--gpio` | toggles a GPIO pin |
| `--sd` | formats, writes and reads back a blank SD card (QEMU only) |
| `--wifi` | scans, joins the emulator's access point (WPA2/WPA3), DHCP, ping, HTTP |
| `--bt` | Bluetooth up, inquiry, SDP and RFCOMM against a Bumble peer (QEMU only) |
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
in both, the gateway forwards to the host's loopback.  For Wi-Fi in QEMU
add `-nic user,model=espwifi` (the access point is `qemu-ap`, passphrase
`qemu-password`; `-global esp32s31-coproc.ssid=...` and `.password=...`
change them), and for Bluetooth `-chardev socket,id=bt,host=localhost,port=9544
-global esp32s31-coproc.chardev=bt` with `test/bt-peer.py` running.
esp-emulator's access point is `myssid`/`mypassword`, and its Ethernet
and Wi-Fi share one network and one DHCP address: take `awge0` down
(`dhcpcd -x awge0; ifconfig awge0 down`) before using Wi-Fi there.

The QEMU machine (`qemu/patches`) models the parts NetBSD and the boot
firmware use:

* CLIC-mode harts: stvec alignment, SPIL in scause, no sie/sip
* the CLIC, interrupt matrix, SYSTIMER, UART0, TRNG and reset register
* the flash XIP window
* the Ethernet MAC: QEMU's NPCM GMAC model, which is the same DesignWare
  core, taught the S31's enhanced descriptors
* the USB OTG controller: QEMU's dwc2 host model, as on the Raspberry Pi
* the USB-Serial/JTAG port and the GPIO registers
* the SD/MMC host: a DesignWare MSHC model
* hart 1's radio firmware: a stand-in that speaks its side of the
  message rings, with an access point on a QEMU network backend and HCI
  to a chardev

QEMU emulates the SPL step itself: it parses `u-boot.itb` and starts
OpenSBI the way the SPL would.  esp-emulator runs the real mask ROM and
SPL instead, and models Espressif's own peripherals.  That makes it the
more independent check of the boot path, the flash image format and the
Ethernet MAC.  It runs the real Wi-Fi firmware on hart 1, against its
emulated Wi-Fi MAC and access point.  It has no USB OTG or SD model:
there `dwctwo0` fails to reset the core and `dwcmmc0` finds no
controller, and the rest of the system carries on.  Its Bluetooth
emulation stands in for the controller by intercepting the firmware's
calls to it, but only on hart 0, so `btconfig espbt0 up` there wedges the
radio firmware.

## How it fits together

* **Kernel:** `sys/arch/riscv/espressif` in the NetBSD branch:
  * the CLIC (`espclic`, `espintmtx`) and SYSTIMER (`espsystimer`)
  * the UART (`espuart`) and USB-Serial/JTAG port (`espusj`)
  * the TRNG (`esptrng`), clocks and resets (`espclkrst`), and GPIO and
    pin muxing (`espgpio`)
  * the Ethernet glue for `awge` (`esp_gmac.c`)
  * the USB PHY (`espusbphy`); the controller is the stock `dwctwo`
  * the SD host glue for the stock `dwcmmc`
  * the radio co-processor (`espcp`) and its Wi-Fi (`espwifi`) and
    Bluetooth (`espbt`) drivers, and `espwifictl`
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
* **Boot firmware:** Espressif's OpenSBI and U-Boot, built with the
  NetBSD toolchain (`tools/build-firmware.sh`).  `opensbi/patches` adds
  two calls to OpenSBI's vendor extension, to start hart 1 on a firmware
  NetBSD loaded and to stop it again; `uboot/patches` only fixes the
  host-side build (SWIG 4.3+, macOS, binman without `pkg_resources`).
* **Radio firmware** (`espcp/`): an ESP-IDF application with Espressif's
  Wi-Fi and Bluetooth libraries, built single-core and started on hart 1.
  `espcp_sys` replaces IDF's start-up with the per-hart part only (the
  clocks, flash, PSRAM and caches are NetBSD's by then) and routes
  interrupts to hart 1; `idf-patches` make IDF leave the host's hardware
  alone (the systimer, the flash, chip resets).  It runs in place from its
  flash partition, with its data above OpenSBI in the SRAM and a heap in
  PSRAM that NetBSD lends it.  NetBSD and the firmware exchange Ethernet
  frames, HCI packets and control messages through two rings in that
  PSRAM (`esp_cpproto.h`), ringing each other with FROM_CPU interrupts.
  The firmware's log goes to NetBSD's.

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
* **Radio firmware.**  Hart 1 starts in M-mode with its PMP and PMA
  registers as reset left them, and the firmware skips the chip set-up
  ESP-IDF normally does at boot: clocks (it runs at whatever U-Boot set),
  the RTC slow clock calibration, and RF calibration data in flash (it
  calibrates at every start).  The systimer is shared: NetBSD uses alarm
  0, the firmware alarms 1 and 2.  If the firmware stops, NetBSD prints
  why and carries on without the radio.

If it doesn't come up, the first line to look for after U-Boot's
`Starting kernel ...` is `esp32s31-loader: copying NetBSD kernel to PSRAM`.

## License

The scripts and tests here are under the BSD 2-clause license (`LICENSE`).
The QEMU patches are GPL-2.0-or-later, like QEMU.  The NetBSD code is
under NetBSD's usual BSD license.
