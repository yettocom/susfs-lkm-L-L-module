[English](README.en.md)

# SUSFS LKM Loader L-L

SUSFS 的**可加载内核模块（LKM）移植版**：不重编译内核就能用 susfs。

这一版面向三星：这类内核在 EL2 保护着 `sys_call_table`，写不进去，所以整条路线不碰 text/rodata —— 走
LSM kprobe + O_PATH kretprobe，DEFEX 与 KDP 的凭据成对处理，卸载时把装上去的东西还回去。模块**只负责把
`.ko` 装进内核**并建起 `/proc/susfs_*` 控制节点，规则全部由管理器下发。来源与我们的改动见下面的
**基于**。

支持 `android13-5.15` / `android14-6.1` / `android15-6.6`；仅 arm64，仅 KernelSU 系管理器（不支持 Magisk）。

## 基于

| 这一层 | 来自 |
|---|---|
| 内核侧源码与 hook 点（LKM 基线） | [inforcqb/susfs4ksu-lkm](https://github.com/inforcqb/susfs4ksu-lkm) `v2.3.0-r18.3`（`899976e`） |
| 可刷入模块脚本（`module/`） | [matsuzaka-yuki/susfs-lkm-module](https://github.com/matsuzaka-yuki/susfs-lkm-module) `v2.3.0-r18.1-fix-6442ebe` |
| SUSFS 本身（功能逻辑与 hook 点） | [simonpunk/susfs4ksu](https://gitlab.com/simonpunk/susfs4ksu) |
| `patch_memory` / `lsm_hook` / `symbol_resolver` 思路 | [SukiSU-Ultra](https://github.com/SukiSU-Ultra/SukiSU-Ultra) |

在这个基础上，本仓库做的是三星适配：这类内核在 EL2 保护 `sys_call_table`，所以不写 text/rodata，改走 LSM
kprobe + O_PATH kretprobe；DEFEX 与 KDP 的凭据成对处理、卸载时还原；加载时机从 `post-fs-data` 挪到
`late-load`；模块状态写进 `module.prop`；选 `.ko` 时多一道固件版本闸（`lib/releases`）。

## 加载

`.ko` 要选与设备内核对应的那一份，例如 5.15 设备用 `susfs_samsung_lkm-android13-5.15.ko`。

**KernelSU 设备**：

```sh
ksud insmod /data/adb/loader/susfs_samsung_lkm.ko
```

**非 KernelSU 设备**：用 release 附带的加载器 `susfs_insmod`：

```sh
susfs_insmod /data/adb/loader/susfs_samsung_lkm.ko expose_proc=1
```

卸载：

```sh
rmmod susfs_samsung_lkm
```

## 作为 KernelSU 模块刷入

想免去手动选 `.ko`，每个 release 附带打包好的模块 `susfs-lkm-module-<tag>-arm64-multi-kmi.zip`：在
KernelSU 管理器里刷入，然后**软重启**（late-load 模式下管理器优先给的就是软重启），或再跑一次
`ksud late-load`。模块按设备内核（`/proc/sys/kernel/osrelease`）自动选择并加载对应的 `.ko`（带
`expose_proc=1`），只负责加载、不设任何 susfs 规则；系统里已有 susfs 则跳过。

**为什么加载在 `late-load` 阶段、而不是 `post-fs-data`**：配套的 KernelSU 本身就是 late-load 起来的，
`post-fs-data` 时内核里还没有 KernelSU，susfs 也就问不到 `ksu_uid_should_umount`，hide 门只能先退回
`uid >= 10000`。`ksud late-load` 的顺序是"先装 daemon → 加载 kernelsu.ko → 再跑这个 stage"，模块的
`late-load.sh` 就跑在这里；`emulated-soft-reboot.sh` 让软重启复用同一个脚本，所以软重启也会重新加载。

- **硬重启不会激活它**：KernelSU 是 late-load 的，硬重启后要重新走一遍 late-load 流程，才会把
  kernelsu.ko 和这份 susfs ko 一起带起来。
- **停用 / 卸载**：在管理器里停用或删除模块，**软重启后生效**。被停用的模块管理器不会跑它的任何脚本，
  所以安装时会在 `/data/adb/post-fs-data.d/susfs-lkm-unload.sh` 放一个跳板去 `rmmod`（这是模块目录之外
  唯一一个文件，`uninstall.sh` 会把它删掉）；也可以 `rmmod susfs_samsung_lkm` 当场卸载。
- **状态**：管理器里看到的 `description=[…]` 就是模块自报的状态，判定在 `module/lib.sh`：
  `Worldline Shifting...` = 装了还没激活过；`< 0%` = 停用/删除，或加载试过没成，或一层都没起来；
  `1.0485%` = 只起来一部分；`1.048596%` = 全部生效（B 分支路线）；`1.130205%` = 全部生效但走表项路线。
- **`.ko` 怎么选**：`select_kmi` 只认 `<kver>.<数字 patch>[-<android 标签>…]` 这种规范形状，选不出来就拒绝
  而不是猜；`lib/releases` 里该 KMI 记录的固件版本必须与当前 release 完全相等，否则也拒绝
  （`ALLOW_RELEASE_MISMATCH=1` 可强行跳过）。加载前先 `bin/susfs_probe` 确认（已在核里就什么都不做，
  说不清就拒绝），加载用模块自带的 `bin/susfs_insmod`，不用 `ksud insmod`——`/data/adb/ksud` 是共享可变
  文件，late-load 会把自己的 daemon 覆盖上去。

## 接口文档

全部 `/proc` 控制节点，见 **[PROC_INTERFACES.md](PROC_INTERFACES.md)**。

## 构建

```sh
./build.sh                                      # 内核模块，需要 docker + DDK 镜像，产物在 out/
python3 tools/pack-samsung-module.py --tag <tag> # 打成可刷入的 zip，每份 ko 先按 samsung.lock.json 校验
```

## License

GPL-2.0，见 [LICENSE](LICENSE)。

## 免责声明

仅用于技术研究与学习。在自担风险的前提下使用。
