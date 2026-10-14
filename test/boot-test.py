#!/usr/bin/env python3
"""
Boot the ESP32-S31 flash image in QEMU's esp32s31 machine, log in on the
serial console and run a few commands.

usage: boot-test.py [--qemu PATH | --esp-emu PATH] [--flash FILE]
                    [--reboots N] [--vi] [--net] [--fptest] [--usb] [--gpio]
                    [--usj] [--sd]
                    [cmd ...]
"""
import argparse
import os
import socket
import sys
import tempfile
import time

import pexpect

WORK = os.environ.get("W", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work"))

ap = argparse.ArgumentParser()
ap.add_argument("--qemu", default=os.path.join(WORK, "qemu/build/qemu-system-riscv32"))
ap.add_argument("--flash", default=os.path.join(WORK, "out/s31_netbsd_flash.bin"))
ap.add_argument("--timeout", type=int, default=300)
ap.add_argument("--log", default="boot-test.log")
ap.add_argument("--machopts", default="")
ap.add_argument("--esp-emu", metavar="PATH",
                help="run in Espressif's esp-emulator (real mask ROM) "
                     "instead of QEMU")
ap.add_argument("--vi", action="store_true",
                help="edit and save a file with vi over the console")
ap.add_argument("--reboots", type=int, default=0,
                help="warm-reboot this many times (stale RAM) before halting")
ap.add_argument("--net", action="store_true",
                help="attach user-mode networking and check DHCP and ping "
                     "(QEMU needs libslirp)")
ap.add_argument("--fptest", action="store_true",
                help="run the FPU context-switch test")
ap.add_argument("--usb", action="store_true",
                help="QEMU only: plug in a USB stick, a USB Ethernet adapter "
                     "and an FTDI serial adapter, and check all three")
ap.add_argument("--usj", action="store_true",
                help="QEMU only: log in over the USB-Serial/JTAG port")
ap.add_argument("--sd", action="store_true",
                help="QEMU only: format, write and read back a blank SD card")
ap.add_argument("--gpio", action="store_true",
                help="drive a GPIO pin with gpioctl and read it back")
ap.add_argument("cmds", nargs="*")
a = ap.parse_args()

cmds = a.cmds or [
    "uname -a",
    "sysctl hw.model hw.machine_arch hw.physmem kern.boottime",
    "dmesg | tail -20",
    "ps axl",
    "vmstat",
    "df -h",
    "echo 'scale test' | awk '{ print toupper($0), 6*7, sqrt(2) }'",
    "ls -l /bin | head",
    "sleep 2; uptime",
]

log = open(a.log, "wb")
if a.esp_emu:
    argv = [a.esp_emu, "--chip", "esp32s31", "--firmware", a.flash,
            "--net", "user"]
else:
    argv = [a.qemu, "-M", "esp32s31,flash=" + a.flash + a.machopts,
            "-nographic", "-serial", "mon:stdio", "-monitor", "none"]
    if a.net:
        argv += ["-nic", "user"]
    if a.usj:
        # The USB-Serial/JTAG port is QEMU's second serial port.
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            usj_port = probe.getsockname()[1]
        argv += ["-serial",
                 f"tcp:127.0.0.1:{usj_port},server=on,wait=off"]
    if a.sd:
        # A blank 32 MiB card (QEMU wants a power-of-two size).
        sdimg = tempfile.NamedTemporaryFile(prefix="s31sd.", suffix=".img",
                                            delete=False)
        sdimg.truncate(32 * 1024 * 1024)
        sdimg.close()
        argv += ["-drive", f"if=sd,format=raw,file={sdimg.name}"]
    if a.usb:
        # A FAT "stick" made by QEMU from a directory, an Ethernet adapter
        # on its own user-mode network, and an FTDI serial adapter.
        usbdir = tempfile.mkdtemp(prefix="s31usb.")
        stick = os.path.join(usbdir, "stick")
        os.mkdir(stick)
        with open(os.path.join(stick, "hello.txt"), "w") as f:
            f.write("hello from a USB stick\n")
        serout = os.path.join(usbdir, "ftdi.out")
        argv += ["-drive", f"if=none,id=stick,format=raw,file=fat:rw:{stick}",
                 "-device", "usb-storage,drive=stick",
                 "-netdev", "user,id=usbnet,net=10.0.3.0/24",
                 "-device", "usb-net,netdev=usbnet",
                 "-chardev", f"file,id=ftdi,path={serout}",
                 "-device", "usb-serial,chardev=ftdi"]
child = pexpect.spawn(argv[0], argv[1:], timeout=a.timeout)
child.logfile_read = log

t0 = time.time()
i = child.expect(["login:", "esp32s31# "])
print(f"{'login prompt' if i == 0 else 'shell prompt'} after {time.time() - t0:.1f}s", flush=True)
if i == 0:
    child.sendline("root")
    child.expect("esp32s31# ")
for c in cmds:
    child.sendline(c)
    child.expect("esp32s31# ")
    out = child.before.decode(errors="replace")
    print(f"$ {c}\n{out.split(chr(10), 1)[1] if chr(10) in out else out}")
def check(name, cmd, want, timeout=120):
    child.sendline(cmd)
    child.expect("esp32s31# ", timeout=timeout)
    out = child.before.decode(errors="replace").split("\n", 1)[-1]
    ok = want in out
    print(f"{name}: {'ok' if ok else 'FAILED'}", flush=True)
    if not ok:
        print(out)
if a.fptest:
    check("fptest", "fptest", "fptest: ok")
if a.net:
    # The guest's dhcpcd runs in the background; give it time to finish.
    check("dhcp", "i=0; while ! netstat -rn -f inet | grep -q ^default &&"
          " [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done;"
          " netstat -rn -f inet | grep -q ^default && echo dhcp-$((6*7))",
          "dhcp-42", 180)
    # The gateway is 10.0.2.2 in QEMU and 192.168.4.1 in esp-emulator.
    check("ping", "ping -c 3 -w 10 $(netstat -rn -f inet |"
          " awk '$1 == \"default\" { print $2; exit }')",
          " 0.0% packet loss")
def usj_session():
    """Log in on the USB-Serial/JTAG port and run a command there."""
    sock = socket.create_connection(("127.0.0.1", usj_port), timeout=5)
    buf = b""

    def until(pat, secs=60):
        nonlocal buf
        end = time.time() + secs
        while pat not in buf and time.time() < end:
            try:
                data = sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                break
            buf += data
        found = pat in buf
        buf = b""
        return found

    sock.sendall(b"\r")
    ok = until(b"login: ")
    sock.sendall(b"root\r")
    ok = ok and until(b"# ")
    sock.sendall(b"tty </dev/tty 2>/dev/null; sysctl -n hw.machine;"
                 b" jot 1000 | wc -l\r")
    ok = ok and until(b"1000")
    sock.sendall(b"exit\r")
    sock.close()
    return ok
if a.usj:
    print(f"usb-serial/jtag login: {'ok' if usj_session() else 'FAILED'}",
          flush=True)
if a.sd:
    check("sd card", "newfs_msdos -F 16 /dev/rld0c >/dev/null &&"
          " mount -t msdos /dev/ld0c /mnt &&"
          " dd if=/dev/urandom of=/mnt/r bs=64k count=32 2>/dev/null &&"
          " sha256 -q /mnt/r > /tmp/sd.sum && umount /mnt &&"
          " mount -t msdos /dev/ld0c /mnt && sha256 -q /mnt/r |"
          " cmp -s - /tmp/sd.sum && umount /mnt && echo sd-$((6*7))",
          "sd-42", 300)
if a.gpio:
    # GPIO4 is free on the dev board.
    check("gpio", "gpioctl -q gpio0 4 set out && gpioctl -q gpio0 4 1 &&"
          " a=$(gpioctl gpio0 4) && gpioctl -q gpio0 4 0 &&"
          " b=$(gpioctl gpio0 4) && echo \"[$a] [$b]\"",
          "[pin 4: state 1] [pin 4: state 0]")
if a.usb:
    check("usb stick", "mount -t msdos /dev/sd0e /mnt && cat /mnt/hello.txt;"
          " umount /mnt", "hello from a USB stick")
    check("usb ethernet", "i=0; while ! netstat -rn -f inet |"
          " grep -q '^default.*cdce0' &&"
          " [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done;"
          " ping -c 3 -w 10 10.0.3.2", " 0.0% packet loss", 180)
    child.sendline("(stty clocal; echo hello over ftdi; sleep 1)"
                   " < /dev/dtyU0 > /dev/dtyU0")
    child.expect("esp32s31# ")
    with open(serout) as f:
        ok = "hello over ftdi" in f.read()
    print(f"usb serial: {'ok' if ok else 'FAILED'}", flush=True)
if a.vi:
    # full-screen editor over the serial console
    child.sendline("TERM=vt100 vi /tmp/vi-test")
    child.expect(["/tmp/vi-test", "new file", "~"])
    time.sleep(1)
    child.send("ihello from vi on the ESP32-S31\x1b")
    time.sleep(0.5)
    child.send(":wq\r")
    child.expect("esp32s31# ")
    child.sendline("cat /tmp/vi-test")
    child.expect("esp32s31# ")
    out = child.before.decode(errors="replace")
    ok = "hello from vi on the ESP32-S31" in out.split("\n", 1)[-1]
    print(f"vi test: {'ok' if ok else 'FAILED'}", flush=True)
for n in range(a.reboots):
    child.sendline("reboot")
    t1 = time.time()
    child.expect("login:")
    child.sendline("root")
    child.expect("esp32s31# ")
    child.sendline("uptime; dmesg | grep -c panic")
    child.expect("esp32s31# ")
    out = child.before.decode(errors="replace")
    print(f"reboot {n + 1}: login after {time.time() - t1:.1f}s;"
          f" {out.split(chr(10), 1)[1].strip() if chr(10) in out else out}",
          flush=True)
child.sendline("halt -p")
try:
    child.expect(pexpect.EOF, timeout=60)
except pexpect.TIMEOUT:
    child.terminate(force=True)
print(f"done after {time.time() - t0:.1f}s")
