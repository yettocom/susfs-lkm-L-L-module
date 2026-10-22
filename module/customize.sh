#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
[ "$KSU" = true ] || abort "KernelSU-compatible managers only; Magisk is not supported."
[ "$ARCH" = arm64 ] || abort "Only arm64 is packaged."
REAL_KERNEL=$(cat /proc/sys/kernel/osrelease) || abort "Cannot read real kernel release."
. "$MODPATH/lib.sh" || abort "Missing script library."
KMI=$(select_kmi "$REAL_KERNEL" "$MODPATH") \
  || abort "Unrecognized kernel release: $REAL_KERNEL. No packaged KMI matches it; refusing to guess."
[ -s "$MODPATH/lib/susfs_samsung_lkm-$KMI.ko" ] || abort "Missing packaged LKM for $KMI."
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/late-load.sh" 0 0 0755
set_perm "$MODPATH/emulated-soft-reboot.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
set_perm "$MODPATH/bin/susfs_probe" 0 0 0755
set_perm "$MODPATH/bin/susfs_insmod" 0 0 0755
set_perm "$MODPATH/lib.sh" 0 0 0755
set_perm "$MODPATH/post-mount.sh" 0 0 0755
set_perm "$MODPATH/trampoline/susfs-lkm-unload.sh" 0 0 0755
touch "$MODPATH/skip_mount"

# The unload an UNINSTALL needs is uninstall.sh's job - the soft reboot that applies one runs
# on_post_fs_data() and prune_modules() lives there - so nothing has to be installed outside the
# module directory for that.  A DISABLE is different: KernelSU runs no script of a disabled module
# ("<dir> is disabled, skip", userspace/ksud/src/module.rs), so the only way "disable" can unload
# the kernel module is an entry point outside the module directory.  That is the single file
# installed below; all of its logic lives in this module (trampoline/susfs-lkm-unload.sh), and
# uninstall.sh removes it again.  See README.md, "Flashing as a KernelSU module".
POSTFS_DIR=/data/adb/post-fs-data.d
mkdir -p "$POSTFS_DIR"
cp -f "$MODPATH/trampoline/susfs-lkm-unload.sh" "$POSTFS_DIR/susfs-lkm-unload.sh"
set_perm "$POSTFS_DIR/susfs-lkm-unload.sh" 0 0 0755

# The "installed but not activated" status is NOT written here: ksud copies the ZIP's module.prop
# over the installed one after customize.sh has run (module.rs: move the staged dir into place), so
# anything written here is discarded.  It is therefore shipped as this module's description - see
# module.prop, which starts life as 0.571024% - and the first post-mount.sh run replaces it with
# the live status.  See lib.sh's status_refresh for the ladder.

ui_print "- Loader only: nothing is loaded during installation."
ui_print "- Selected kernel KMI: $KMI (arm64). Vendor ABI compatibility is not guaranteed."
ui_print "- Samsung (RKP-safe) LKM builds: no text/rodata patching."
ui_print "- Activates on a KernelSU soft reboot (the manager prefers one in late-load mode),"
ui_print "  or on the next 'ksud late-load' of the CVE flow - either way right after kernelsu.ko."
ui_print "- A hard reboot does NOT activate it: KernelSU itself is late-loaded on this setup."
ui_print "- Uninstall unloads SUSFS on the soft reboot that applies it, and so does disable:"
ui_print "  KernelSU runs no script of a disabled module, so one small hook is installed in"
ui_print "  /data/adb/post-fs-data.d/ to reach this module's unload hook"
ui_print "  (trampoline/susfs-lkm-unload.sh). Nothing else is put outside the module directory."
ui_print "- Do not stack this with a built-in SUSFS or another SUSFS LKM."
