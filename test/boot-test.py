#!/usr/bin/env python3
"""
Boot the ESP32-S31 flash image in QEMU's esp32s31 machine, log in on the
serial console and run a few commands.

usage: boot-test.py [--qemu PATH | --esp-emu PATH] [--flash FILE]
                    [--reboots N] [--vi] [--net] [--fptest] [--usb] [--gpio]
                    [--usj] [--sd] [--wifi] [--bt]
                    [cmd ...]
"""
import argparse
import atexit
import os
import socket
import threading
import subprocess
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
ap.add_argument("--uart", action="store_true",
                help="UART1 through a TCP echo peer (QEMU only)")
ap.add_argument("--hw", action="store_true",
                help="sensors, RTC time across a reboot, watchdog reset")
ap.add_argument("--persist", action="store_true",
                help="write to /persist and /etc (flash), check after a reboot")
ap.add_argument("--crypto", action="store_true",
                help="check espcrypto(4): the AES implementation, and"
                     " test/cryptotest (built by its build.sh, served from"
                     " /tmp/httproot) through /dev/crypto; needs --wifi"
                     " or --net, and esp-emulator (QEMU has no engines)")
ap.add_argument("--wifi", action="store_true",
                help="join the access point (esp-emulator's, or QEMU's "
                     "stand-in) with espwifictl, then DHCP, ping and an HTTP "
                     "download (serve /tmp/httproot on the host's port 8088)")
ap.add_argument("--bt", action="store_true",
                help="Bluetooth inquiry, SDP and RFCOMM against a Bumble "
                     "peer (test/bt-peer.py)")
ap.add_argument("--espcp-elf",
                default=os.path.join(WORK, "build-espcp/espcp.elf"),
                help="the co-processor firmware's ELF, for esp-emulator's "
                     "Bluetooth interception")
ap.add_argument("--bumble-python", default=sys.executable,
                help="a Python with Bumble installed (pip install bumble)")
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
if a.bt:
    peer = subprocess.Popen(
        [a.bumble_python,
         os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "bt-peer.py")],
        stdout=open(a.log + ".bt-peer", "w"), stderr=subprocess.STDOUT)
    atexit.register(peer.terminate)
    time.sleep(3)
# Wi-Fi's gateway: esp-emulator's NAT, or the QEMU stand-in's own
# user-mode network (apart from the Ethernet's 10.0.2.0/24).
gateway = "192.168.4.1" if a.esp_emu else "10.0.4.2"
if a.esp_emu:
    argv = [a.esp_emu, "--chip", "esp32s31", "--firmware", a.flash,
            "--net", "user"]
    if a.bt:
        argv += ["--elf", a.espcp_elf, "--ble-hci", "tcp:localhost:9544"]
else:
    argv = [a.qemu, "-M", "esp32s31,flash=" + a.flash + a.machopts,
            "-nographic", "-serial", "mon:stdio", "-monitor", "none"]
    if a.net:
        argv += ["-nic", "user"]
    if a.wifi:
        # The co-processor stand-in's access point.
        argv += ["-nic", "user,model=espwifi,net=10.0.4.0/24",
                 "-global", "esp32s31-coproc.ssid=myssid",
                 "-global", "esp32s31-coproc.password=mypassword"]
    if a.bt:
        argv += ["-chardev", "socket,id=bt,host=localhost,port=9544",
                 "-global", "esp32s31-coproc.chardev=bt"]
    if a.usj:
        # The USB-Serial/JTAG port is QEMU's second serial port.
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            usj_port = probe.getsockname()[1]
        argv += ["-serial",
                 f"tcp:127.0.0.1:{usj_port},server=on,wait=off"]
    if a.uart:
        # UART1 is QEMU's third serial port; the harness echoes it back in
        # upper case.
        if not a.usj:
            argv += ["-serial", "null"]
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            uart_port = probe.getsockname()[1]
        argv += ["-serial",
                 f"tcp:127.0.0.1:{uart_port},server=on,wait=off"]
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
    try:
        child.expect("esp32s31# ", timeout=timeout)
    except pexpect.TIMEOUT:
        if a.esp_emu:
            # ^C would stop esp-emu itself; wait some more.
            child.expect("esp32s31# ", timeout=timeout)
        else:
            child.sendcontrol("c")
            child.expect("esp32s31# ", timeout=30)
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
if a.wifi:
    # esp-emulator's Ethernet and Wi-Fi share one network and one DHCP
    # address: keep the Ethernet out of the way.
    child.sendline("dhcpcd -x awge0; ifconfig awge0 down")
    child.expect("esp32s31# ")
    check("wifi scan", "espwifictl scan", "myssid", 60)
    check("wifi join", "espwifictl join myssid mypassword &&"
          " espwifictl wait 60", "associated with \"myssid\"", 120)
    check("wifi dhcp", "i=0; while ! netstat -rn -f inet |"
          " grep -q '^default.*espwifi0' && [ $i -lt 60 ]; do sleep 1;"
          " i=$((i+1)); done; netstat -rn -f inet | grep ^default",
          "espwifi0", 180)
    check("wifi ping", f"ping -c 3 -w 10 {gateway}", " 0.0% packet loss")
    check("wifi http", f"ftp -o /tmp/blob http://{gateway}:8088/blob.bin"
          " >/dev/null 2>&1; sha256 -q /tmp/blob",
          open("/tmp/httproot/blob.sha").read().split()[0]
          if os.path.exists("/tmp/httproot/blob.sha") else "no-blob", 300)
if a.bt:
    check("bt up", "btconfig espbt0 up && btconfig espbt0", "UP", 60)
    check("bt inquiry", "btconfig espbt0 inquiry", "aa:bb:cc:dd:ee:01", 120)
    check("bt sdp", "sdpquery -a aa:bb:cc:dd:ee:01 search SP",
          "Bumble echo", 120)
    check("bt rfcomm", "(echo hello-rfcomm; sleep 4) |"
          " rfcomm_sppd -a aa:bb:cc:dd:ee:01 -s SP & sleep 10; kill $! 2>/dev/null",
          "HELLO-RFCOMM", 120)
if a.crypto:
    check("crypto aes", "sysctl -n kern.crypto.aes.selected", "ESP32-S31 AES")
    check("crypto dev", f"ftp -o /tmp/cryptotest http://{gateway}:8088/"
          "cryptotest >/dev/null 2>&1; chmod +x /tmp/cryptotest;"
          " /tmp/cryptotest", "cryptotest: ok", 300)
    check("crypto hw", "vmstat -e | grep -E 'espcrypto0 (SHA|modexp)' |"
          " awk '$4 > 0 { n++ } END { print \"hw-used-\" n }'", "hw-used-2")
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
if a.uart:
    def uart_echo():
        sock = socket.create_connection(("127.0.0.1", uart_port))
        while True:
            d = sock.recv(256)
            if not d:
                break
            sock.sendall(d.upper())
    threading.Thread(target=uart_echo, daemon=True).start()
    check("uart1", "exec 3<>/dev/ttyEU1; stty raw -echo 9600 <&3 &&"
          " stty -a <&3 | head -1; (sleep 1; echo hello-uart1 >&3) &"
          " dd bs=1 count=11 <&3 2>/dev/null; echo; exec 3<&-",
          "HELLO-UART1")
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
def relogin():
    child.sendline("reboot")
    t1 = time.time()
    child.expect("login:")
    child.sendline("root")
    child.expect("esp32s31# ")
    return time.time() - t1
if a.persist:
    # /persist is the flash's writable partition, made on first boot;
    # /etc is union-mounted on /persist/etc.
    check("persist mounted", "mount | grep -cE ' on /persist type ffs|"
          " on /etc type union'", "2")
    check("persist write", "echo persist-$((6*7)) > /etc/ptest &&"
          " dd if=/dev/urandom of=/persist/blob bs=4k count=96 2>/dev/null &&"
          " sha256 -q /persist/blob > /persist/blob.sha &&"
          " echo written-$((6*7))", "written-42", 600)
    print(f"persist: login after {relogin():.1f}s", flush=True)
    check("persist after reboot", "[ \"$(cat /etc/ptest)\" = persist-42 ] &&"
          " sha256 -q /persist/blob | cmp -s - /persist/blob.sha &&"
          " echo persisted-$((6*7))", "persisted-42", 300)
    check("persist rewrite", "dd if=/dev/urandom of=/persist/blob bs=4k"
          " count=96 conv=notrunc 2>/dev/null && sha256 -q /persist/blob >"
          " /persist/blob.sha && rm /etc/ptest && sync &&"
          " fsck_ffs -n /dev/respnor0a 2>&1 | tail -1", "files,", 600)
if a.hw:
    # Temperature from the co-processor (the stand-in says 25 C); in
    # QEMU, two ADC channels as well.
    check("sensors", "sysctl -w hw.espsensor0.adc_channels=0x101 >/dev/null;"
          " envstat -s espsensor0:temperature",
          "degC", 60)
    if not a.esp_emu:
        check("adc", "envstat | grep 'ADC2_0'", "1.800", 60)
        # QEMU's comparator pads: GPIO37 1.0 V, GPIO38 2.0 V, GPIO40 1.2 V.
        check("comparator", "sysctl -w hw.espcmpr0.ch1=37 hw.espcmpr0.ch2=38"
              " hw.espcmpr0.reference=2 >/dev/null;"
              " a=$(envstat -d espcmpr0 | grep -c TRUE);"
              " sysctl -w hw.espcmpr0.reference=40 >/dev/null;"
              " b=$(envstat -d espcmpr0 | grep -c TRUE);"
              " sysctl -n hw.espgpio0.pin37; echo cmpr-$a-$b", "cmpr-2-1")
        # The RTC timer keeps the time of day across a reboot.
        check("rtc set", "date 203001020304.05 >/dev/null; date +%Y", "2030")
        print(f"rtc: login after {relogin():.1f}s", flush=True)
        check("rtc after reboot", "date +%Y%m%d", "20300102")
    # Armed and never tickled, the watchdog resets the system.
    child.sendline("wdogctl -e -p 3 espwdt0 && wdogctl && sleep 30")
    t1 = time.time()
    try:
        child.expect("login:", timeout=120)
        print(f"watchdog: reset after {time.time() - t1:.1f}s", flush=True)
        child.sendline("root")
        child.expect("esp32s31# ")
    except pexpect.TIMEOUT:
        print("watchdog: FAILED (no reset)", flush=True)
        child.sendcontrol("c")
        child.expect("esp32s31# ")
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

