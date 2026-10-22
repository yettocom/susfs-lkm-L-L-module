#!/bin/bash
# Build susfs_samsung_lkm.ko against a DDK kernel tree, with the Samsung (RKP) path on.
set -euo pipefail

SRC=/mnt/d/root/susfs4ksu-lkm-work
OUT=/mnt/d/root/susfs4ksu-lkm-work/out

build() {
    local kmi="$1" rel="$2" out="$3" samsung="${4:-1}"
    echo "=== $kmi  rel=$rel  samsung=$samsung"
    docker run --rm -v "$SRC":/src -e REL="$rel" -e KMI="$kmi" -e SAMSUNG="$samsung" \
        "ghcr.io/ylarod/ddk-min:$kmi" bash -lc '
        set -e
        KDIR=/opt/ddk/kdir/$KMI
        echo "$REL" > "$KDIR/include/config/kernel.release"
        sed -i "s/^#define UTS_RELEASE .*/#define UTS_RELEASE \"$REL\"/" "$KDIR/include/generated/utsrelease.h"
        make -C "$KDIR" M=/src/kernel clean >/dev/null 2>&1 || true
        make -C "$KDIR" M=/src/kernel ARCH=arm64 LLVM=1 LLVM_IAS=1 \
            SUSFS_SAMSUNG_NO_PATCH_TEXT=$SAMSUNG \
            KBUILD_MODPOST_WARN=1 modules -j16 2>&1 |
            grep -E "error|warning|LD \[M\]|CC \[M\]" | tail -40
        strings /src/kernel/susfs_samsung_lkm.ko | grep -m1 vermagic=
    '
    cp "$SRC/kernel/susfs_samsung_lkm.ko" "$OUT/$out"
    echo "saved $out: $(stat -c%s "$OUT/$out") bytes"
}

mkdir -p "$OUT"
build android13-5.15 "5.15.189-android13-8-3251900-abS9180ZCS8FZI1" android13-5.15_susfs_samsung_lkm.ko 1
build android14-6.1  "6.1.145-android14-11-3254743-abS9280ZCS6DZI1"  android14-6.1_susfs_samsung_lkm.ko  1
build android15-6.6  "6.6.98-android15-8-p5a696e2-abogkiS9380ZCSDCZH1-4k" android15-6.6_susfs_samsung_lkm.ko 1

ls -l "$OUT"
