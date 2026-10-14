#!/usr/bin/env python3
"""
Boot the ESP32-S31 flash image in QEMU's esp32s31 machine, log in on the
serial console and run a few commands.

usage: boot-test.py [--qemu PATH | --esp-emu PATH] [--flash FILE]
                    [--reboots N] [--vi] [cmd ...]
"""
import argparse
import os
import sys
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
