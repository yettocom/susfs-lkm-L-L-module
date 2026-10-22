[English](README.en.md)

# susfs4ksu-lkm

SUSFS 的**可加载内核模块（LKM）移植版**：让 susfs 可以在不用重编译内核的情况下运行 。

## 加载

`.ko` 要选与设备内核对应的那一份，例如 5.15 设备用 `susfs_samsung_lkm-android13-5.15.ko`。

**KernelSU 设备**：

```sh
ksud insmod /data/adb/loader/susfs_samsung_lkm.ko
```

**非 KernelSU 设备**：用 release 附带的加载器 `susfs_insmod`：

```sh
susfs_insmod /data/adb/loader/susfs_samsung_lkm.ko
```

卸载：

```sh
rmmod susfs_samsung_lkm
```

## 作为 KernelSU 模块刷入

每个 release 附带打包好的模块 `susfs-lkm-module-<tag>-arm64-multi-kmi.zip`：在 KernelSU 管理器里刷入，
然后**软重启**（late-load 模式下管理器优先给的就是这个）或再跑一次 `ksud late-load`。模块按设备内核
（`/proc/sys/kernel/osrelease`）自动选择并加载对应的 `.ko`（带 `expose_proc=1`），只负责加载、不设任何
susfs 规则；系统里已有 susfs 则跳过。仅 arm64、仅 KernelSU 系管理器。

**加载时机是 late-load 阶段，不是 `post-fs-data`。** 这份 fork 的 KernelSU 自己就是 late-load 加载的，
post-fs-data 时内核里根本没有 KernelSU，susfs 也就问不到 `ksu_uid_should_umount`（hide 门会退回老的
`uid >= 10000` 规则）。`ksud late-load` 的顺序是"先装 daemon → 加载 kernelsu.ko → 再跑这个 stage"，
模块的 `late-load.sh` 就跑在这里；`emulated-soft-reboot.sh` 让软重启复用同一个脚本。加载器用模块自带的
`bin/susfs_insmod` 而不是 `ksud insmod`：`/data/adb/ksud` 是共享可变文件，late-load 会把自己的 daemon
覆盖上去，只有自带那份能保证是我们验过的加载器。

- **硬重启不会激活它**：KernelSU 是 late-load 的，硬重启后要再跑一次 CVE 流程 / `ksud late-load` 才会把
  两个模块都带起来。
- 要移除：在管理器里停用或删除本模块，**软重启后生效**——停用的模块管理器不跑它的脚本，所以装了一个
  `/data/adb/post-fs-data.d/susfs-lkm-unload.sh` 小钩子去 `rmmod`；也可以 `rmmod susfs_samsung_lkm`
  当场卸载。
- `expose_proc=1` 建立 `/proc/susfs_*` 控制节点，sus 规则通过这些节点下发；`/proc/susfs_status` 报告模块
  自报状态（走的是哪条路线、哪些层起来了）。
- 模块 `module.prop` 描述里那串"世界线变动率"就是当前状态，判定逻辑在 `module/lib.sh` 的
  `status_refresh()`。

## 接口文档

全部 `/proc` 控制节点，见 **[PROC_INTERFACES.md](PROC_INTERFACES.md)**。

## License

GPL-2.0，见 [LICENSE](LICENSE)。移植来源：[susfs4ksu](https://gitlab.com/simonpunk/susfs4ksu)（功能逻辑与 hook 点）、
[SukiSU-Ultra](https://github.com/SukiSU-Ultra/SukiSU-Ultra)（`patch_memory` / `lsm_hook` / `symbol_resolver`）。

## 免责声明

仅用于技术研究与学习。在自担风险的前提下使用。
