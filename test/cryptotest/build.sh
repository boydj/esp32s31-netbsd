#!/bin/sh
# Build cryptotest for NetBSD/riscv32 with the NetBSD tools (W=work dir).
set -e
W=${W:-$(cd "$(dirname "$0")/../.." && pwd)/work}
D=$(cd "$(dirname "$0")" && pwd)
"$W/tools-rv32/bin/riscv32--netbsd-gcc" --sysroot="$W/dest-rv32" -static -Os \
    -I"$D" -o "${1:-$D/cryptotest}" "$D/cryptotest.c"
