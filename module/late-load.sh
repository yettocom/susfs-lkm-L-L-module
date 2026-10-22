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

# lib/releases records the release each .ko was built against.  A different release of the same KMI
# means the image's vermagic does not match this kernel: `ksud insmod` refuses it, and the standalone
# loader would rewrite the vermagic to force it in - ABI-lying, not proof of compatibility.  Fail
# closed, with an override for an operator who knows the two firmwares share the ABI.
if [ -r "$MODDIR/lib/releases" ]; then
  # `while read` from a redirection, not a pipe: a pipe would run the loop in a subshell and EXPECTED
  # would be lost.  Plain shell, not awk: a missing applet must not make this gate disappear.
  EXPECTED=""
  while read -r rel_kmi rel_release _rest; do
    [ "$rel_kmi" = "$KMI" ] || continue
    EXPECTED="$rel_release"
    break
  done < "$MODDIR/lib/releases"
  # The packager writes one entry per packaged .ko, so a KMI we ship without one means the two halves
  # of the package disagree - worth stopping on, not guessing about.
  if [ -z "$EXPECTED" ]; then
    echo "Refusing to load: $KMI has no usable entry in lib/releases (the package is inconsistent)."
    exit 1
  fi
  if [ "$EXPECTED" != "$REAL_KERNEL" ]; then
    if [ "$ALLOW_RELEASE_MISMATCH" = "1" ]; then
      echo "WARNING: running release $REAL_KERNEL != $EXPECTED for $KMI; continuing (ALLOW_RELEASE_MISMATCH=1)"
    else
      echo "Running release: $REAL_KERNEL"
      echo "Built for:       $EXPECTED ($KMI)"
      echo "Refusing to load: the packaged LKM is built for another release of this KMI."
      echo "If the two firmwares really share the kernel ABI, set ALLOW_RELEASE_MISMATCH=1 and re-run."
      exit 1
    fi
  fi
else
  echo "WARNING: lib/releases is missing - the built-for-release check is skipped"
fi

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
# binary comes from the package and can also rewrite a vermagic the kernel refused (the
# ALLOW_RELEASE_MISMATCH case above), which ksud's insmod cannot.
"$MODDIR/bin/susfs_insmod" "$LKM" expose_proc=1
RESULT=$?
[ "$RESULT" = "0" ] || { echo "susfs_insmod failed: $RESULT"; exit "$RESULT"; }
echo "Loaded via susfs_insmod"

"$MODDIR/bin/susfs_probe" || { echo "Load returned success but protocol probe failed."; exit 1; }
echo "SUSFS detected after loading. No rules were applied by this module."
