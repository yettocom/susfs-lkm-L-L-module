[中文](README.md)

# susfs4ksu-lkm

A loadable kernel module (LKM) port of SUSFS: run susfs without recompiling the kernel.

## Loading

Pick the `.ko` that matches the device kernel, e.g. a 5.15 device uses `susfs_samsung_lkm-android13-5.15.ko`.

**KernelSU devices**:

```sh
ksud insmod /data/adb/loader/susfs_samsung_lkm.ko
```

**Non-KernelSU devices**: use the `susfs_insmod` loader shipped with the release:

```sh
susfs_insmod /data/adb/loader/susfs_samsung_lkm.ko
```

Uninstall:

```sh
rmmod susfs_samsung_lkm
```

## Flashing as a KernelSU module

Each release ships a packaged module `susfs-lkm-module-<tag>-arm64-multi-kmi.zip`: flash it in a
KernelSU manager, then **soft reboot** (what the manager prefers in late-load mode anyway) or run
`ksud late-load` once more. The module picks the `.ko` for the device kernel
(`/proc/sys/kernel/osrelease`), loads it with `expose_proc=1`, applies no susfs rules, and skips if
susfs is already present. arm64 and KernelSU-compatible managers only.

**It loads in the late-load stage, not in `post-fs-data`.** KernelSU in this fork is late-loaded
itself, so at post-fs-data there is no KernelSU in the kernel and SUSFS could not ask it for
`ksu_uid_should_umount` (the hide gates would fall back to the old `uid >= 10000` rule).
`ksud late-load` stages its daemon, loads kernelsu.ko and only then runs this stage, which is where
the module's `late-load.sh` runs; `emulated-soft-reboot.sh` reuses the same script on a soft reboot.
The loader is the module's own `bin/susfs_insmod` rather than `ksud insmod`: `/data/adb/ksud` is a
shared, mutable file that late-load renames its own daemon over, so only the packaged one is
provably the loader that was tested.

- **A hard reboot does not activate it**: KernelSU is late-loaded, so after a hard reboot the CVE
  flow / `ksud late-load` has to run again to bring both modules up.
- To remove it: disable or remove the module in the manager, which takes effect **after a soft
  reboot** - the manager runs no script of a disabled module, so a small hook is installed at
  `/data/adb/post-fs-data.d/susfs-lkm-unload.sh` to `rmmod`; or `rmmod susfs_samsung_lkm` to unload
  it live.
- `expose_proc=1` creates the `/proc/susfs_*` control nodes; sus rules are applied through them.
  `/proc/susfs_status` reports what the module says about itself (which route it took, which layers
  are up).
- The "worldline divergence" in the module's `module.prop` description is its current state; the
  ladder is `status_refresh()` in `module/lib.sh`.

## Interface docs

All `/proc` control nodes: see **[PROC_INTERFACES.en.md](PROC_INTERFACES.en.md)**.

## License

GPL-2.0, see [LICENSE](LICENSE). Ported from: [susfs4ksu](https://gitlab.com/simonpunk/susfs4ksu) (feature logic and hook points), [SukiSU-Ultra](https://github.com/SukiSU-Ultra/SukiSU-Ultra) (`patch_memory` / `lsm_hook` / `symbol_resolver`).

## Disclaimer

For technical research and study only. Use at your own risk.
