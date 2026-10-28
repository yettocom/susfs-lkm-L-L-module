[中文](README.md)

# susfs-lkm-loader L-L

A loadable kernel module (LKM) port of SUSFS for Samsung RKP/KDP/DEFEX devices: susfs hiding
without recompiling the kernel, alongside a late-loaded KernelSU.

Based on [inforcqb/susfs4ksu-lkm](https://github.com/inforcqb/susfs4ksu-lkm) ;
feature source [susfs4ksu](https://gitlab.com/simonpunk/susfs4ksu).

## Usage

Each release provides:

- `susfs_samsung_lkm-<kmi>.ko` -- one per kernel KMI (android13-5.15 / android14-6.1 / android15-6.6)
- `susfs-lkm-module-<tag>-arm64.zip` -- KernelSU module: flash it in a manager, then **soft reboot**;
  `late-load.sh` picks the ko for `/proc/sys/kernel/osrelease` and loads it with `expose_proc=1`
  (loading only, no susfs rules; skipped if susfs is already present). A hard reboot removes both
  modules - rerun the late-load flow.

Manual load / unload:

```sh
insmod susfs_samsung_lkm.ko   # non-KernelSU devices: use the shipped bin/susfs_insmod
rmmod susfs_samsung_lkm
```

The `/proc/susfs_*` control nodes: see [PROC_INTERFACES.en.md](PROC_INTERFACES.en.md).

## Module status

The description shown in the manager's module list is the live status (decided by
`status_refresh()` in `module/lib.sh`):

| Status | Meaning |
|---|---|
| `Worldline Shifting...` | disabled / pending removal; or installed but never activated |
| `< 0%` | a load was attempted and did not take; or the ko is in the kernel with zero layers armed |
| `1.0485%` | partially armed; or status unreadable |
| `1.048596%` | fully armed on the branch route |
| `1.130205%` | fully armed on the table route |

## License

GPL-3.0. For technical research and study only, use at your own risk.
