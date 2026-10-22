#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Runs AFTER the load stage on both activation paths (soft_reboot.rs -> on_post_fs_data() ->
# run_stage("post-mount"), and ksud --late-load -> run_stage("post-mount")), which makes it the
# one place that has to write the status the loader produced: late-load.sh has eight early exits
# and none of them should have to remember to report.
MODDIR=${0%/*}
. "$MODDIR/lib.sh" || exit 0
status_refresh
