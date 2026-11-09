[English](README.en.md)

# susfs-lkm-loader L-L

SUSFS 的可加载内核模块（LKM）移植，面向三星 RKP/KDP/DEFEX 设备：不重编译内核即可获得 susfs 隐藏，
配合 late-load 的 KernelSU 使用。

基于 [inforcqb/susfs4ksu-lkm](https://github.com/inforcqb/susfs4ksu-lkm) ，
功能源头 [susfs4ksu](https://gitlab.com/simonpunk/susfs4ksu)。

## 使用

Release 提供：

- `susfs_samsung_lkm-<kmi>.ko` —— 按内核 KMI 三选一（android13-5.15 / android14-6.1 / android15-6.6）
- `susfs-lkm-module-<tag>-arm64.zip` —— KernelSU 模块包：管理器刷入后**软重启**，`late-load.sh`
  按 `/proc/sys/kernel/osrelease` 自动选 ko 加载（`expose_proc=1`，只加载、不设 susfs 规则；
  系统已有 susfs 则跳过）。硬重启后两个模块都不在，需重跑 late-load 流程。

手动加载 / 卸载：

```sh
insmod susfs_samsung_lkm.ko   # 非 KernelSU 设备用 release 自带的 bin/susfs_insmod
rmmod susfs_samsung_lkm
```

`/proc/susfs_*` 控制节点见 [PROC_INTERFACES.md](PROC_INTERFACES.md)。

## 仓库结构

| 目录 | 内容 |
|---|---|
| `kernel/` | 模块源码。`susfs_*.c` 是各功能层；`susfs_rkp.c`、`susfs_arm64.h` 是分支补丁与 AArch64 指令判定；`kallrecon/lib/` 是符号解析（vendored，只编 `lib/`） |
| `tools/` | 构建期工具。`build-loader.sh` 编 `bin/susfs_insmod`；`pack_module.py` 打模块 zip；`susfs_insmod.c`、`susfs_probe.c` 是 loader 与状态探针的源码 |
| `module/` | KernelSU 模块骨架。`module.prop`、`customize.sh`、`post-mount.sh`、`late-load.sh`、`lib.sh`、`uninstall.sh`、`emulated-soft-reboot.sh`、`trampoline/` |
| `bin/` | 预编译的 aarch64 静态程序。`susfs_insmod` 改重定位后加载 ko；`susfs_probe` 读 `/proc/susfs_status` |

编译：`./build.sh`，在 DDK 容器里按三个 KMI 各编一次。

## 模块状态

管理器模块列表里 `module.prop` 的描述显示当前状态（判定逻辑在 `module/lib.sh` 的 `status_refresh()`）：

| 状态 | 含义 |
|---|---|
| `Worldline Shifting...` | 已停用/待删除；或已安装但从未激活过 |
| `< 0%` | 试过加载但没成功；或 ko 在内核里但一层都没 armed |
| `1.0485%` | 部分生效；或状态读不出来 |
| `1.048596%` | 全部生效，走 B 分支路线 |
| `1.130205%` | 全部生效，走表项改写路线 |

## License

GPL-3.0。仅用于技术研究与学习，风险自担。
