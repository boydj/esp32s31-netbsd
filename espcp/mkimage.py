#!/usr/bin/env python3
"""
mkimage.py: pack the co-processor firmware ELF into the image NetBSD's
espcp(4) loads (struct espcp_fw_header in esp_cpproto.h).

The parts of the ELF that live in the XIP window go in the image at their
offset from the window's base, so the image runs in place once it is
flashed at the matching flash offset.  The RAM segments (HP SRAM, LP RAM)
follow; espcp(4) copies them and clears their bss before it starts hart 1.

usage: mkimage.py [--xip-base ADDR] [--xip-size SIZE] [--extmem SIZE]
                  [--shm SIZE] [--ident TEXT] firmware.elf out.bin
"""

import argparse
import struct
import sys
import zlib

from elftools.elf.elffile import ELFFile
from elftools.elf.constants import SH_FLAGS

FW_MAGIC = 0x43313353
FW_VERSION = 1
MAXSEGS = 12
HDR_FMT = "<IHHIIIIIIII8s64s"           # up to eh_ident
SEG_FMT = "<IIII"
HDR_SIZE = struct.calcsize(HDR_FMT) + MAXSEGS * struct.calcsize(SEG_FMT)

RAM_RANGES = [
    (0x2F000000, 0x2F080000),           # HP SRAM
    (0x2E000000, 0x2E008000),           # LP RAM
]


def in_ram(addr, size):
    return any(lo <= addr and addr + size <= hi for lo, hi in RAM_RANGES)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xip-base", type=lambda s: int(s, 0), default=0x40C00000)
    ap.add_argument("--xip-size", type=lambda s: int(s, 0), default=0x300000)
    ap.add_argument("--extmem", type=lambda s: int(s, 0), default=0x100000)
    ap.add_argument("--shm", type=lambda s: int(s, 0), default=0x40000)
    ap.add_argument("--ident", default="espcp")
    ap.add_argument("elf")
    ap.add_argument("out")
    a = ap.parse_args()

    with open(a.elf, "rb") as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name(".symtab")

        def sym(name):
            s = symtab.get_symbol_by_name(name)
            if not s:
                sys.exit(f"mkimage.py: no symbol {name}")
            return s[0]["st_value"]

        entry = sym("espcp_entry")
        params = sym("espcp_bootparams")

        image = bytearray(HDR_SIZE)
        xip_end = a.xip_base + a.xip_size

        # Flash-resident sections, in place.
        for sec in elf.iter_sections():
            if not sec["sh_flags"] & SH_FLAGS.SHF_ALLOC:
                continue
            addr, size = sec["sh_addr"], sec["sh_size"]
            if not (a.xip_base <= addr < xip_end) or size == 0:
                continue
            if addr + size > xip_end:
                sys.exit(f"mkimage.py: {sec.name} past the end of the partition")
            if sec["sh_type"] == "SHT_NOBITS":
                continue
            off = addr - a.xip_base
            if off < HDR_SIZE:
                sys.exit(f"mkimage.py: {sec.name} overlaps the header")
            if len(image) < off + size:
                image.extend(bytes(off + size - len(image)))
            image[off:off + size] = sec.data()

        # RAM segments, after them.
        segs = []
        crc = 0
        for seg in elf.iter_segments():
            if seg["p_type"] != "PT_LOAD":
                continue
            addr, filesz, memsz = seg["p_paddr"], seg["p_filesz"], seg["p_memsz"]
            if a.xip_base <= addr < xip_end:
                continue
            if not in_ram(addr, memsz):
                sys.exit(f"mkimage.py: segment at {addr:#x} not in RAM")
            data = seg.data()[:filesz]
            while len(image) % 16:
                image.append(0)
            segs.append((addr, len(image), filesz, memsz))
            image.extend(data)
            crc = zlib.crc32(data, crc)
        if len(segs) > MAXSEGS:
            sys.exit("mkimage.py: too many RAM segments")
        if not any(s[0] <= entry < s[0] + s[2] for s in segs):
            sys.exit("mkimage.py: entry point not in a RAM segment")
        if not any(s[0] <= params and params + 32 <= s[0] + s[2] for s in segs):
            sys.exit("mkimage.py: boot parameters not in a loaded RAM segment")
        if len(image) > a.xip_size:
            sys.exit(f"mkimage.py: image too big ({len(image)} > {a.xip_size})")

    hdr = struct.pack(HDR_FMT, FW_MAGIC, FW_VERSION, HDR_SIZE, len(image),
                      a.xip_base, entry, params, a.extmem, a.shm, len(segs),
                      crc & 0xFFFFFFFF, bytes(8), a.ident.encode()[:63])
    for s in segs + [(0, 0, 0, 0)] * (MAXSEGS - len(segs)):
        hdr += struct.pack(SEG_FMT, *s)
    assert len(hdr) == HDR_SIZE
    image[:HDR_SIZE] = hdr
    with open(a.out, "wb") as f:
        f.write(image)
    print(f"{a.out}: {len(image)} bytes, entry {entry:#x}, "
          f"{len(segs)} RAM segments")


if __name__ == "__main__":
    main()
