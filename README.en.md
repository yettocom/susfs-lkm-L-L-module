[中文](README.md)

# SUSFS LKM Loader L-L

A loadable kernel module (LKM) port of SUSFS: run susfs without recompiling the kernel.

This build targets Samsung. Those kernels protect `sys_call_table` at EL2 and refuse the write, so
nothing here touches text or rodata: the route is LSM kprobes plus O_PATH kretprobes, the DEFEX and
KDP credentials are handled in pairs, and what was installed is put back on unload. The module
**only installs the `.ko`** and brings up the `/proc/susfs_*` control nodes; every rule is applied by
the manager. Where all of this comes from is under **Basis** below.

`android13-5.15` / `android14-6.1` / `android15-6.6`; arm64 only, KernelSU-family managers only
(Magisk is not supported).

## Basis

| Layer | Comes from |
|---|---|
| kernel sources and hook points (the LKM baseline) | [inforcqb/susfs4ksu-lkm](https://github.com/inforcqb/susfs4ksu-lkm) `v2.3.0-r18.3` (`899976e`) |
| the flashable module scripts (`module/`) | [matsuzaka-yuki/susfs-lkm-module](https://github.com/matsuzaka-yuki/susfs-lkm-module) `v2.3.0-r18.1-fix-6442ebe` |
| SUSFS itself (behaviour and hook points) | [simonpunk/susfs4ksu](https://gitlab.com/simonpunk/susfs4ksu) |
| the `patch_memory` / `lsm_hook` / `symbol_resolver` approach | [SukiSU-Ultra](https://github.com/SukiSU-Ultra/SukiSU-Ultra) |

On top of that, what this repository adds is the Samsung adaptation: no text or rodata writes (those
kernels protect `sys_call_table` at EL2), so the route is LSM kprobes plus O_PATH kretprobes; DEFEX
and KDP credentials handled in pairs and restored on unload; the load moved from `post-fs-data` to
`late-load`; the module's state written into `module.prop`; and one more gate when picking the `.ko`
(the firmware release in `lib/releases`).

## Loading

Pick the `.ko` that matches the device kernel, e.g. a 5.15 device uses
`susfs_samsung_lkm-android13-5.15.ko`.

**KernelSU devices**:

```sh
ksud insmod /data/adb/loader/susfs_samsung_lkm.ko
```

**Non-KernelSU devices**: use the `susfs_insmod` loader shipped with the release:

```sh
susfs_insmod /data/adb/loader/susfs_samsung_lkm.ko expose_proc=1
```

Uninstall:

```sh
rmmod susfs_samsung_lkm
```

## Flashing as a KernelSU module

To skip picking the `.ko` by hand, each release ships a packaged module
`susfs-lkm-module-<tag>-arm64-multi-kmi.zip`: flash it in a KernelSU manager, then **soft reboot**
(which is what the manager offers first in late-load mode), or run `ksud late-load` once more. The
module picks the `.ko` for the device kernel (`/proc/sys/kernel/osrelease`), loads it with
`expose_proc=1`, applies no susfs rules, and skips if susfs is already present.

**Why the load happens in the `late-load` stage and not in `post-fs-data`**: the KernelSU this pairs
with is itself late-loaded, so at `post-fs-data` there is no KernelSU in the kernel yet and susfs
cannot ask `ksu_uid_should_umount` - the hide gates would fall back to `uid >= 10000`. `ksud
late-load` installs its daemon, loads kernelsu.ko, and only then runs this stage, which is where the
module's `late-load.sh` sits; `emulated-soft-reboot.sh` makes a soft reboot run the same script, so a
soft reboot loads too.

- **A hard reboot does not activate it.** KernelSU is late-loaded, so after a cold boot the late-load
  flow has to run again before both kernelsu.ko and this susfs ko are back in the kernel.
- **Disable / uninstall**: disable or delete the module in the manager and **soft reboot**. A
  disabled module has none of its scripts run by the manager, so installation puts one trampoline at
  `/data/adb/post-fs-data.d/susfs-lkm-unload.sh` to `rmmod` it - the only file outside the module
  directory, and `uninstall.sh` removes it again. `rmmod susfs_samsung_lkm` unloads it on the spot.
- **Status**: the `description=[…]` the manager shows is the module's own report, decided in
  `module/lib.sh`: `Worldline Shifting...` = installed but never activated; `< 0%` = disabled or
  marked for removal, or a load was tried and did not take, or no layer came up; `1.0485%` = only
  some layers up; `1.048596%` = everything armed, on the branch route; `1.130205%` = everything
  armed, on the table route.
- **How the `.ko` is chosen**: `select_kmi` accepts only the canonical
  `<kver>.<numeric patch>[-<android tag>…]` shape and refuses rather than guesses; the release
  recorded for that KMI in `lib/releases` must equal the running one, or the load is refused too
  (`ALLOW_RELEASE_MISMATCH=1` overrides it). Before loading, `bin/susfs_probe` has to agree that
  susfs is absent (already in the kernel means nothing is done; cannot tell means refuse), and the
  load uses the bundled `bin/susfs_insmod` rather than `ksud insmod` - `/data/adb/ksud` is a shared,
  mutable file that late-load renames its own daemon over.

## Interface docs

All `/proc` control nodes: see **[PROC_INTERFACES.en.md](PROC_INTERFACES.en.md)**.

## Build

```sh
./build.sh                                      # kernel modules: docker + the DDK images, output in out/
python3 tools/pack-samsung-module.py --tag <tag> # pack the flashable zip, each ko hash-checked first
```

## License

GPL-2.0, see [LICENSE](LICENSE).

## Disclaimer

For technical research and study only. Use at your own risk.
