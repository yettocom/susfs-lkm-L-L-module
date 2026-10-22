#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pack the flashable Samsung module ZIP from this tree, entirely from local inputs.

This is the Samsung branch's packer.  Upstream's tools/pack_module.py is left untouched and packs
the upstream-style module; this one packs the same module/ scripts plus what the Samsung flow adds:

  * lib/susfs_samsung_lkm-<kmi>.ko  the Samsung (RKP-safe) builds from out/, hash-verified against
                                  samsung.lock.json before packaging
  * bin/susfs_insmod              built by tools/build-loader.sh from tools/susfs_insmod.c
  * bin/susfs_probe               compiled here from tools/susfs_probe.c (NDK clang, bionic)
  * lib/releases                  generated from the lock: <kmi> <release> per line, which is what
                                  late-load.sh checks the running kernel against
  * module.prop                   taken from module/ as-is (the module owns its id and version)

Nothing is downloaded: without the NDK the build fails instead of shipping something unverified.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "samsung.lock.json").read_text())

# Windows checkouts get CRLF, and a shell script or module.prop with CRLF is broken on the device
# (toybox sh keeps the CR, and the manager parses "id=susfs_samsung_lkm\r"), so every text entry is
# normalised on the way into the ZIP.
TEXT_SUFFIXES = {".sh", ".prop", ".md", ".json", ".txt", ".c"}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def add_text(archive, path, name):
    name = Path(name).as_posix()
    data = path.read_bytes()
    if path.suffix in TEXT_SUFFIXES and b"\r\n" in data:
        data = data.replace(b"\r\n", b"\n")
        print(f"lf   {name} (CRLF normalised)")
    archive.writestr(name, data)


def verify(path, digest, what):
    if not path.exists():
        sys.exit(f"missing {what}: {path}")
    got = sha256(path)
    if got != digest:
        sys.exit(f"SHA-256 mismatch for {what}:\n  want {digest}\n  got  {got}")
    print(f"ok   {what}")


def find_clang():
    """The NDK's clang driver, Windows or Linux host.

    Deliberately the plain `clang`/`clang.exe`, driven with --target=aarch64-linux-android28: the
    NDK's target-prefixed .cmd wrapper is a batch file that failed here with a bare "Access is
    denied." and no compiler diagnostics, while the driver itself compiles the same probe.  The
    probe links against bionic, so it has to be the NDK's clang rather than a system one.
    """
    candidates = []
    if os.environ.get("ANDROID_NDK_HOME"):
        candidates.append(Path(os.environ["ANDROID_NDK_HOME"]))
    for base in filter(None, (os.environ.get("ANDROID_HOME"),
                              os.environ.get("ANDROID_SDK_ROOT"),
                              "D:/root/toolchains/android-sdk")):
        ndk_dir = Path(base) / "ndk"
        if ndk_dir.is_dir():
            candidates.extend(sorted(ndk_dir.iterdir(), reverse=True))
    for ndk in candidates:
        for host in ("windows-x86_64", "linux-x86_64"):
            for name in ("clang.exe", "clang"):
                clang = ndk / "toolchains/llvm/prebuilt" / host / "bin" / name
                if clang.exists():
                    return clang
    sys.exit("no NDK clang found (set ANDROID_NDK_HOME)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ko-dir", default=str(ROOT / "out"), help="directory holding <kmi>_susfs_samsung_lkm.ko")
    ap.add_argument("--insmod", default=str(ROOT / "bin/susfs_insmod"), help="the built loader")
    ap.add_argument("--tag", required=True, help="module version string")
    ap.add_argument("--out", default=None, help="output ZIP path")
    args = ap.parse_args()

    ko_dir = Path(args.ko_dir)
    insmod = Path(args.insmod)
    if not insmod.is_file():
        sys.exit(f"loader not found: {insmod} (run tools/build-loader.sh first)")

    kmis = LOCK["supported_kmis"]
    kos = {}
    for kmi in kmis:
        built = ko_dir / f"{kmi}_susfs_samsung_lkm.ko"
        verify(built, LOCK["assets"][built.name], f"lib/susfs_samsung_lkm-{kmi}.ko")
        kos[kmi] = built

    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    probe = build / "susfs_probe"
    clang = find_clang()
    print(f"cc   {clang}")
    subprocess.run([str(clang), "--target=aarch64-linux-android28",
                    "-O2", "-Wall", "-Wextra", "-Werror", "-fPIE", "-pie",
                    str(ROOT / "tools/susfs_probe.c"), "-o", str(probe)], check=True)
    print(f"ok   bin/susfs_probe ({probe.stat().st_size} bytes)")

    releases = "".join(f"{kmi} {release}\n" for kmi, release in LOCK["expected_releases"].items())
    provenance = {
        "repository": "https://github.com/inforcqb/susfs4ksu-lkm",
        "baseline_commit": LOCK["upstream_lkm"]["commit"],
        "samsung_build": LOCK["samsung_build"],
        "finasu_ksud_compat": LOCK["finasu_ksud_compat"],
        "architecture": "arm64",
        "wrapper_version": args.tag,
        "load_stage": "late-load",
        "supported_kmis": kmis,
        "expected_releases": LOCK["expected_releases"],
        "assets_sha256": {f"susfs_samsung_lkm-{kmi}.ko": sha256(p) for kmi, p in kos.items()}
        | {"susfs_insmod": sha256(insmod), "susfs_probe": sha256(probe)},
    }

    out = Path(args.out) if args.out else (
        ROOT / "dist" / f"susfs-lkm-module-{args.tag}-arm64-multi-kmi.zip")
    out.parent.mkdir(parents=True, exist_ok=True)
    # Built next to the destination and renamed only when complete: an interrupted build must not
    # leave a truncated file that still looks like an installable module.
    staging = out.with_suffix(".zip.part")
    with zipfile.ZipFile(staging, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted((ROOT / "module").rglob("*")):
            if path.is_file():
                add_text(archive, path, path.relative_to(ROOT / "module"))
        archive.writestr("kmis", "".join(k + "\n" for k in kmis))
        archive.writestr("lib/releases", releases)
        archive.write(probe, "bin/susfs_probe")
        archive.write(insmod, "bin/susfs_insmod")
        for kmi, path in kos.items():
            archive.write(path, f"lib/susfs_samsung_lkm-{kmi}.ko")
        add_text(archive, ROOT / "tools/susfs_probe.c", "source/susfs_probe.c")
        add_text(archive, ROOT / "LICENSE", "LICENSE")
        archive.writestr("provenance.json", json.dumps(provenance, indent=2) + "\n")
    os.replace(staging, out)
    print(out)
    print("sha256:", sha256(out))


if __name__ == "__main__":
    main()
