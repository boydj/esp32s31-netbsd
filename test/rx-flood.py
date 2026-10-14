#!/usr/bin/env python3
"""
Flood the console with input to exercise the UART RX interrupt path.

usage: rx-flood.py [flash.bin]     (QEMU from $W/qemu/build, see boot-test.py)
"""
import os
import sys
import time

import pexpect

WORK = os.environ.get("W", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work"))
q = os.environ.get("QEMU", os.path.join(WORK, "qemu/build/qemu-system-riscv32"))
f = sys.argv[1] if len(sys.argv) > 1 else os.path.join(WORK, "out/s31_netbsd_flash.bin")

c = pexpect.spawn(q, ["-M", "esp32s31,flash=" + f, "-nographic", "-serial",
                      "mon:stdio", "-monitor", "none"], timeout=120)
c.logfile_read = open("rx-flood.log", "wb")
c.expect("login:"); c.sendline("root"); c.expect("esp32s31# ")
c.sendline("stty -echo; cat > /tmp/x"); time.sleep(0.5)
line = ("0123456789abcdefghijklmnopqrstuvwxyz" * 3)[:100] + "\n"
total = 0
for i in range(200):          # 20200 bytes, sent as fast as the pty takes it
    c.send(line); total += len(line)
c.sendcontrol("d")
c.expect("esp32s31# ")
c.sendline("stty echo; wc -c /tmp/x; sort -u /tmp/x | wc -l")
c.expect("esp32s31# ")
out = [l.strip() for l in c.before.decode(errors="replace").splitlines() if l.strip()]
print(f"sent {total} bytes; received: {out[-2]}, distinct lines: {out[-1]}")
c.terminate(force=True)
