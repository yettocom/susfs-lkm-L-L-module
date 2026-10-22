#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# prune_modules() runs this before it deletes the module directory, on every path that can prune: the
# next cold boot's `ksud late-load`, a full KernelSU uninstall, and - the one that matters for
# "uninstall, then soft reboot" - the soft reboot itself, because soft_reboot.rs calls
# on_post_fs_data() and prune_modules() lives there.  That is why the unload needs no hook outside
# the module.
#
# A cold boot starts with a fresh kernel, so the rmmod below is a no-op there and this is purely the
# directory/state cleanup.  On the two live-kernel paths it is what takes SUSFS out, so the manager's
# SUSFS page stops answering "compatible" for a module that is gone.  See README.md, "Flashing as a KernelSU module".
echo "SUSFS uninstall: unloading susfs_samsung_lkm"
MODDIR=${0%/*}
. "$MODDIR/lib.sh" 2>/dev/null
# Remove the post-fs-data.d trampoline first: it is the one file outside the module directory, and
# this is the reliable place to take it out (the trampoline also deletes itself when it finds this
# directory gone - see trampoline/susfs-lkm-unload.sh).
rm -f /data/adb/post-fs-data.d/susfs-lkm-unload.sh
if unload 2>/dev/null || rmmod susfs_samsung_lkm 2>/dev/null; then
  echo "SUSFS uninstall: susfs_samsung_lkm unloaded"
else
  echo "SUSFS uninstall: could not unload susfs_samsung_lkm (it goes away on the next reboot)"
fi
