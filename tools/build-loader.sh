#!/bin/bash
# Build the standalone loader (bin/susfs_insmod) from tools/susfs_insmod.c.
#
# The same command upstream's CI uses.  It is a freestanding static-pie aarch64 binary: no libc,
# its own _start, raw syscalls - which is what lets it run before /data is mounted and on a device
# with no usable dynamic linker.  -fPIC is required by -static-pie with a current clang; the CI's
# older clang accepted the same source without it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/bin/susfs_insmod}"

mkdir -p "$(dirname "$OUT")"
clang --target=aarch64-linux-gnu -O2 -fPIC -nostdlib -static-pie \
      -fno-stack-protector -fno-builtin -fuse-ld=lld -Wl,-e,_start \
      -o "$OUT" "$ROOT/tools/susfs_insmod.c"

file "$OUT"
printf 'sha256: %s\n' "$(sha256sum "$OUT" | cut -d' ' -f1)"
