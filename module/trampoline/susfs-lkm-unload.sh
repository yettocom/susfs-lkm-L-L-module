#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Installed by customize.sh into /data/adb/post-fs-data.d/ - the ONE file this module puts outside
# its own directory, and the reason it has to:
#
#   KernelSU does not run any script of a disabled module, and it runs uninstall.sh only for a
#   removed one.  A loader's whole effect is a kernel module that stays loaded, so with no entry
#   point outside the module directory, disabling it would unload nothing - "disable" would mean
#   nothing at all.  The unload itself lives in lib.sh and is called from here.
#
# Ordering: on a soft reboot soft_reboot.rs runs run_stage("emulated-soft-reboot") (the loader)
# first and on_post_fs_data() (which runs post-fs-data.d) afterwards, so an ENABLED module has
# already been loaded when this runs - which is why the enabled case must return immediately.
# The CVE flow's `ksud --late-load` does not run post-fs-data.d at all, so there this hook is
# simply not reached; the flag still keeps the next load from happening.
#
# Removal: uninstall.sh deletes this file before the module directory goes away, and the fallback
# below deletes it when it finds the directory already gone - a leftover here would be harmless
# (it would try to rmmod something that is not loaded) but there is no reason to leave one.
MODDIR=/data/adb/modules/susfs_lkm_loader_lateload

# Enabled and present: the module's own scripts run, and they are what load the LKM.
if [ -d "$MODDIR" ] && [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ]; then
  exit 0
fi

# Disabled (or marked for removal): KernelSU runs nothing from the module directory in this state,
# so the unload happens here.  Five lines, and they live with the entry point on purpose - the
# alternative was a separate disable script inside the module plus this trampoline, and one file
# fewer is worth more than that separation.
if [ -d "$MODDIR" ]; then
  echo "SUSFS loader disabled; unloading susfs_samsung_lkm"
  . "$MODDIR/lib.sh" 2>/dev/null
  unload 2>/dev/null || rmmod susfs_samsung_lkm 2>/dev/null
  # Report it in module.prop too: no script of this module runs again while it is disabled.  The
  # writer lives in lib.sh (it carries the module's introduction over), so it is sourced here
  # rather than copied - the module directory still exists in this branch.
  set_status 'Worldline Shifting...' 2>/dev/null
  exit 0
fi

# The module directory is gone (uninstalled): finish the job here and stop being a leftover.
echo "SUSFS loader is gone; unloading susfs_samsung_lkm and removing this hook"
rmmod susfs_samsung_lkm 2>/dev/null ||
  { [ -x /data/adb/ksu/bin/busybox ] && /data/adb/ksu/bin/busybox rmmod susfs_samsung_lkm 2>/dev/null; }
rm -f "$0"
