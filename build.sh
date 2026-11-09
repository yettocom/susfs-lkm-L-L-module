#!/bin/bash
# Build susfs_samsung_lkm.ko against the DDK kernel trees, with the Samsung (RKP) path on.
#
# Usage:  ./build.sh [kmi ...]        (default: all three)
#
# The DDK image supplies the kernel release and its own patch level: nothing here writes
# UTS_RELEASE, so each ko carries the image's default vermagic -- one string per KMI, not
# per firmware.  The loader (bin/susfs_insmod) rewrites it to the running kernel's release
# on demand, so a single ko per KMI serves every firmware of that generation.
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
OUT="$SRC/out"
ALL=(android13-5.15 android14-6.1 android15-6.6)
if [ $# -eq 0 ]; then KMIS=("${ALL[@]}"); else KMIS=("$@"); fi

build() {
    local kmi="$1" samsung="${2:-1}"
    local name="${kmi}_susfs_samsung_lkm.ko"

    # Remove the previous artifact first: a failed build must not leave a stale ko behind
    # for the packer to ship.
    rm -f "$OUT/$name"

    echo "=== $kmi  samsung=$samsung"
    docker run --rm -v "$SRC":/src -e KMI="$kmi" -e SAMSUNG="$samsung" \
        "ghcr.io/ylarod/ddk-min:$kmi" bash -lc '
        set -e
        KDIR=/opt/ddk/kdir/$KMI
        echo "ddk release: $(cat "$KDIR/include/config/kernel.release" 2>/dev/null || echo MISSING)"
        make -C "$KDIR" M=/src/kernel clean >/dev/null 2>&1 || true
        make -C "$KDIR" M=/src/kernel ARCH=arm64 LLVM=1 LLVM_IAS=1 \
            SUSFS_SAMSUNG_NO_PATCH_TEXT=$SAMSUNG \
            KBUILD_MODPOST_WARN=1 modules -j16 2>&1 |
            grep -E "error|warning|LD \[M\]" | tail -40 || true
        strings /src/kernel/susfs_samsung_lkm.ko | grep -m1 vermagic= || true
    '

    if [ ! -f "$SRC/kernel/susfs_samsung_lkm.ko" ]; then
        echo "NO KO PRODUCED for $kmi" >&2
        return 1
    fi
    mkdir -p "$OUT"
    cp "$SRC/kernel/susfs_samsung_lkm.ko" "$OUT/$name"
    echo "saved $name: $(stat -c%s "$OUT/$name") bytes"
}

for kmi in "${KMIS[@]}"; do
    build "$kmi" 1
done

echo "=== artifacts ==="
ls -l "$OUT"/*_susfs_samsung_lkm.ko
