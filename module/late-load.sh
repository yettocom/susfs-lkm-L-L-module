#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# The loader.  Run by the `late-load` stage - not post-fs-data.
#
# ksud maps a stage name straight onto "<stage>.sh", and `ksud late-load` stages its daemon, loads
# kernelsu.ko and only then runs this stage.  That is what makes the whole thing work here: at
# post-fs-data there is no KernelSU in the kernel at all (ours is late-loaded after boot), while by
# this stage kernelsu.ko is up - so SUSFS resolves KernelSU's ksu_uid_should_umount at load time and
# the hide gates follow KernelSU's exclusion list from the start instead of after a reload.
#
# emulated-soft-reboot.sh runs this same script, so a soft reboot loads too; SUSFS_STAGE only changes
# the name in the log.  The script is idempotent: a second run sees the existing SUSFS interface in
# the probe and leaves it alone.  See README.md, "Flashing as a KernelSU module".
MODDIR=${0%/*}
umask 077
exec > "$MODDIR/load.log" 2>&1
echo "SUSFS load: stage=${SUSFS_STAGE:-late-load} $(date)"

# A safety net, not the mechanism: ksud already skips a disabled or removed module.  It stays because
# loading SUSFS back into a module the user just removed is the one outcome worth ruling out twice.
if [ -f "$MODDIR/disable" ] || [ -f "$MODDIR/remove" ]; then
  echo "Module is disabled or marked for removal; not loading."
  exit 0
fi

# Read the real release: uname may already be spoofed by another SUSFS implementation.
REAL_KERNEL=$(cat /proc/sys/kernel/osrelease) || exit 1
. "$MODDIR/lib.sh" || { echo "Missing script library."; exit 1; }
KMI=$(select_kmi "$REAL_KERNEL" "$MODDIR") \
  || { echo "Refusing unrecognized kernel release (no packaged KMI matches): $REAL_KERNEL"; exit 1; }
LKM="$MODDIR/lib/susfs_samsung_lkm-$KMI.ko"
[ -s "$LKM" ] || { echo "Missing packaged LKM for $KMI."; exit 1; }
echo "Selected kernel KMI: $KMI"

# The .ko carries the DDK image's own release string (one per KMI, not per firmware), which will
# not match this device's.  bin/susfs_insmod reads the release the kernel asks for out of kmsg and
# rewrites the module's vermagic before retrying - that is upstream's mechanism, not an override,
# so nothing here compares releases.

# 0 = SUSFS already there, 10 = provably absent, anything else = we cannot tell, so refuse.
"$MODDIR/bin/susfs_probe"
RESULT=$?
case "$RESULT" in
  0) echo "SUSFS already detected; leaving it untouched."; exit 0 ;;
  10) ;;
  *) echo "Cannot safely establish SUSFS absence (probe=$RESULT); refusing load."; exit 1 ;;
esac

# The standalone loader that ships in this package, and only that one.  `ksud insmod` is deliberately
# not used: /data/adb/ksud is a shared, mutable file - late-load renames its own staged daemon over it
# (late_load.rs:44, before this stage runs), so it is usually the matching one, but nothing here can
# prove it, and a foreign or stale ksud would load the module with a loader we did not build.  This
# binary comes from the package and is the only loader here that can rewrite a vermagic the kernel
# refused, which ksud's insmod cannot.
# owner_nonce= names the instance that owns this ko, by module id (`${MODDIR##*/}`), and
# /proc/susfs_status echoes it back.  The id and not the path: `$0`-derived paths differ between
# a direct call, a sourced file and the post-fs-data.d trampoline, and lib.sh's unload compares
# this value before it rmmod's anything - a path mismatch would make "disable" unload nothing.
"$MODDIR/bin/susfs_insmod" "$LKM" expose_proc=1 owner_nonce="${MODDIR##*/}"
RESULT=$?
[ "$RESULT" = "0" ] || { echo "susfs_insmod failed: $RESULT"; exit "$RESULT"; }
echo "Loaded via susfs_insmod"

"$MODDIR/bin/susfs_probe" || { echo "Load returned success but protocol probe failed."; exit 1; }
echo "SUSFS detected after loading. No rules were applied by this module."
