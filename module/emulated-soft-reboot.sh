#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Load on a KernelSU soft reboot.  soft_reboot.rs runs this stage after resetting sys.boot_completed,
# and the kernel - with KernelSU, root and the su grants still in it - is what the loader needs, so
# "install, then soft reboot" activates SUSFS here.  Same loader as the late-load stage, one
# implementation.  See README.md, "Flashing as a KernelSU module".
MODDIR=${0%/*}
exec sh "$MODDIR/late-load.sh"
