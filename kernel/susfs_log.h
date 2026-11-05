/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SUSFS_LOG_H
#define __SUSFS_LOG_H

#include <linux/types.h>
#include <linux/printk.h>

#ifdef pr_fmt
#undef pr_fmt
#define pr_fmt(fmt) "susfs_samsung_lkm: " fmt
#endif

/* ENABLE_LOG the way upstream has it: SUSFS_LOGI() is the informational channel (rule add/remove,
 * hook arming, hit paths) behind the switch CMD_SUSFS_ENABLE_LOG / /proc/susfs_enable_log toggles -
 * default ON, matching upstream's DEFINE_STATIC_KEY_TRUE(susfs_is_log_enabled).  pr_warn/pr_err stay
 * unconditional: a failure must be visible whether or not logging is on (upstream's SUSFS_LOGE is
 * unconditional too).  Measured before the fix: the flag had no readers, so `enable_log 0` plus a
 * rule add still printed "susfs_samsung_lkm: sus_path: ..." in dmesg - a root-side fingerprint. */
bool susfs_log_enabled(void);

/* A log of our own, kept in memory.  The kernel ring buffer wraps long before a phone gets handed
 * back to a desk, and a panic erases it outright - which is exactly the case this module has to
 * explain.  So the lines that describe a load are also stored here, and read back through
 * /proc/susfs_log (256 lines, the timestamp reads like dmesg, write anything to clear). */
void susfs_log_store(const char *fmt, ...);

#define SUSFS_LOGI(fmt, ...)						\
	do {								\
		if (susfs_log_enabled()) {				\
			susfs_log_store(fmt, ##__VA_ARGS__);		\
			pr_info(fmt, ##__VA_ARGS__);			\
		}								\
	} while (0)

/* Warnings and errors reach the ring whether or not logging is on, matching pr_warn/pr_err being
 * unconditional: a load that failed has to stay explainable after dmesg has moved on. */
#define SUSFS_LOGW(fmt, ...)						\
	do {								\
		susfs_log_store(fmt, ##__VA_ARGS__);			\
		pr_warn(fmt, ##__VA_ARGS__);				\
	} while (0)

#define SUSFS_LOGE(fmt, ...)						\
	do {								\
		susfs_log_store(fmt, ##__VA_ARGS__);			\
		pr_err(fmt, ##__VA_ARGS__);				\
	} while (0)

#endif
