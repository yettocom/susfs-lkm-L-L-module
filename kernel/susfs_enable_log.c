// SPDX-License-Identifier: GPL-2.0
/*
 * susfs_enable_log.c - toggle SUSFS debug logging (ENABLE_LOG feature).  Upstream gates SUSFS_LOGI() behind the
 * static branch susfs_is_log_enabled; an LKM has no static branch, so the same semantics live in a global flag
 * with a /proc node (write "1"/"0", read to query), ON at load like upstream's DEFINE_STATIC_KEY_TRUE.  Measured
 * before the fix: the flag had no readers and "susfs_samsung_lkm: ..." kept appearing after enable_log 0.
 */
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/cred.h>	/* current_uid(), control-node gate */
#include <linux/spinlock.h>
#include <linux/string.h>	/* memset(), clearing the ring */
#include <linux/stdarg.h>	/* va_list for susfs_log_store() */
#include <linux/timekeeping.h>	/* ktime_get_ns(), the stamp readers match against dmesg */
#include "susfs_abi.h"
#include "susfs_log.h"
#include "susfs.h"	/* susfs_expose_proc */
#include "ksu_allow_gate.h"	/* the control-node gate is su's own */

static bool log_enabled = true;

/* Load-time switch, read-only in sysfs; the runtime switch is the /proc node and the supercall, the interfaces
 * upstream's userspace drives (ksud insmod ... enable_log=0).  A load that starts silent is the only state in
 * which the unconditional "loaded." line of susfs_main.c matters: without it, silent-but-loaded looks unloaded. */
module_param_named(enable_log, log_enabled, bool, 0444);

bool susfs_log_enabled(void)
{
	return READ_ONCE(log_enabled);
}

/*
 * The ring behind /proc/susfs_log.
 *
 * 256 lines is sized for the load-time burst (site table, LSM hooks, the refusals) several times
 * over; 256 bytes per line holds the longest message this module writes, and a longer one is
 * truncated by vsnprintf rather than lost.  Only this module's own lines are here - filtering
 * dmesg by prefix shows the same strings, but only until the kernel ring wraps, and a panic takes
 * that buffer with it while this one is gone too, which is why the node is read on a live device.
 */
#define SUSFS_LOG_RING_SLOTS	256
#define SUSFS_LOG_RING_LINE	256

struct susfs_log_line {
	u64 ns;
	char text[SUSFS_LOG_RING_LINE];
};

static struct susfs_log_line susfs_log_ring[SUSFS_LOG_RING_SLOTS];
static unsigned int susfs_log_head;
static unsigned int susfs_log_count;
static DEFINE_SPINLOCK(susfs_log_lock);

void susfs_log_store(const char *fmt, ...)
{
	struct susfs_log_line *line;
	unsigned long flags;
	va_list args;

	if (!fmt)
		return;

	/* Formatting happens under the lock: a slot that wrapped must not be half written while the
	 * reader copies it, and 256 bytes into a static buffer is not worth a second lock. */
	spin_lock_irqsave(&susfs_log_lock, flags);
	line = &susfs_log_ring[susfs_log_head];
	susfs_log_head = (susfs_log_head + 1) % SUSFS_LOG_RING_SLOTS;
	if (susfs_log_count < SUSFS_LOG_RING_SLOTS)
		susfs_log_count++;
	line->ns = ktime_get_ns();
	va_start(args, fmt);
	vsnprintf(line->text, sizeof(line->text), fmt, args);
	va_end(args);
	spin_unlock_irqrestore(&susfs_log_lock, flags);
}

static int susfs_log_proc_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	unsigned int count, i, first;
	struct susfs_log_line one;

	spin_lock_irqsave(&susfs_log_lock, flags);
	count = susfs_log_count;
	first = (susfs_log_head + SUSFS_LOG_RING_SLOTS - count) % SUSFS_LOG_RING_SLOTS;
	spin_unlock_irqrestore(&susfs_log_lock, flags);

	seq_printf(m, "susfs ring: %u of %u lines held for this boot\n", count,
		   (unsigned int)SUSFS_LOG_RING_SLOTS);
	seq_printf(m, "timestamps read like dmesg; write anything here to clear the ring\n\n");
	for (i = 0; i < count; i++) {
		spin_lock_irqsave(&susfs_log_lock, flags);
		one = susfs_log_ring[(first + i) % SUSFS_LOG_RING_SLOTS];
		spin_unlock_irqrestore(&susfs_log_lock, flags);
		seq_printf(m, "[%5u.%06u] %s", (unsigned int)(one.ns / NSEC_PER_SEC),
			   (unsigned int)((one.ns % NSEC_PER_SEC) / 1000), one.text);
	}
	return 0;
}

static int susfs_log_proc_open(struct inode *inode, struct file *file)
{
	/* Same contract as the enable node above: 0777 so DAC passes and sus_path's hidden set
	 * answers ENOENT for non-root callers, plus this check for when that hook is not the one
	 * answering. */
	if (!susfs_control_visible())
		return -ENOENT;
	return single_open(file, susfs_log_proc_show, NULL);
}

static ssize_t susfs_log_proc_write(struct file *file, const char __user *buf,
				    size_t len, loff_t *off)
{
	unsigned long flags;

	if (!susfs_control_visible())
		return -ENOENT;
	if (!len)
		return 0;

	spin_lock_irqsave(&susfs_log_lock, flags);
	susfs_log_head = 0;
	susfs_log_count = 0;
	memset(susfs_log_ring, 0, sizeof(susfs_log_ring));
	spin_unlock_irqrestore(&susfs_log_lock, flags);
	return len;
}

static const struct proc_ops susfs_log_proc_ops = {
	.proc_open = susfs_log_proc_open,
	.proc_read = seq_read,
	.proc_write = susfs_log_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_dir_entry *susfs_log_entry;
/* Deliberately NOT EXPORT_SYMBOL'd: the only user is this module (susfs_log.h wraps it), and an exported name
 * shows up as a [susfs_samsung_lkm]-owned symbol in /proc/kallsyms - outward surface upstream SUSFS lacks. */

static int log_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", log_enabled ? 1 : 0);
	return 0;
}

static int log_proc_open(struct inode *inode, struct file *file)
{
	/* 0777 on purpose: the ENOENT contract for non-root callers comes from sus_path's hidden set, and a
	 * restrictive mode would answer EACCES - which leaks that the node exists.  That makes sus_path's hook the
	 * only thing between an app and this interface, so the handler refuses non-root callers itself as well. */
	if (!susfs_control_visible())
		return -ENOENT;
	return single_open(file, log_proc_show, NULL);
}

static ssize_t log_proc_write(struct file *file, const char __user *buf,
			      size_t len, loff_t *off)
{
	char c;

	/* Same reason as the open check: an fd opened before the process dropped privileges must not become a
	 * way in. */
	if (!susfs_control_visible())
		return -ENOENT;

	if (copy_from_user(&c, buf, 1))
		return -EFAULT;

	/* Only '0' and '1' are the protocol.  The old code accepted every other byte, still reported len, and
	 * toggled nothing - a typo was indistinguishable from success. */
	if (c != '0' && c != '1')
		return -EINVAL;

	if (c == '1') {
		WRITE_ONCE(log_enabled, true);
		SUSFS_LOGI("susfs: enable logging to kernel\n");
	} else {
		WRITE_ONCE(log_enabled, false);
		/* Unconditional on purpose (upstream uses its unconditional SUSFS_LOGE here): the confirmation
		 * that silence is now in effect must not itself be silenced. */
		pr_info("susfs: disable logging to kernel\n");
	}
	return len;
}

static const struct proc_ops log_proc_ops = {
	.proc_open = log_proc_open,
	.proc_read = seq_read,
	.proc_write = log_proc_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_dir_entry *log_proc_entry;

int susfs_enable_log_init(void)
{
	/* Nothing to register here (logging is toggled over the supercall or by this node), so the node is the
	 * only thing to gate.  See susfs_control_node_allowed(): 0777 so DAC passes, LSM answers ENOENT. */
	if (susfs_control_node_allowed()) {
		log_proc_entry = proc_create("susfs_enable_log", 0777, NULL,
					     &log_proc_ops);
		if (!log_proc_entry)
			pr_warn("proc_create(susfs_enable_log) failed\n");
		else
			SUSFS_LOGI("susfs_enable_log: armed (proc: /proc/susfs_enable_log)\n");
		susfs_log_entry = proc_create("susfs_log", 0777, NULL, &susfs_log_proc_ops);
		if (!susfs_log_entry)
			pr_warn("proc_create(susfs_log) failed - the ring is kept but cannot be read\n");
		else
			SUSFS_LOGI("susfs_log: armed (proc: /proc/susfs_log, %u lines)\n",
				   (unsigned int)SUSFS_LOG_RING_SLOTS);
	} else {
		SUSFS_LOGI("susfs_enable_log: /proc node not created (expose_proc=%d lsm=%d)\n",
			(int)susfs_expose_proc, (int)sus_path_lsm_active());
	}
	return 0;
}

void susfs_enable_log_exit(void)
{
	if (susfs_log_entry) {
		proc_remove(susfs_log_entry);
		susfs_log_entry = NULL;
	}
	if (log_proc_entry) {
		proc_remove(log_proc_entry);
		log_proc_entry = NULL;
	}
	log_enabled = true;	/* back to the load-time default */
}

/* supercall: CMD_SUSFS_ENABLE_LOG */
void susfs_enable_log_supercall(void __user **arg)
{
    struct st_susfs_log info = {0};

    if (copy_from_user(&info, (void __user *)*arg, sizeof(info))) {
        info.err = -EFAULT;
        goto out;
    }
    if (info.enabled) {
        WRITE_ONCE(log_enabled, true);
        SUSFS_LOGI("susfs: enable logging to kernel (supercall)\n");
    } else {
        WRITE_ONCE(log_enabled, false);
        pr_info("susfs: disable logging to kernel (supercall)\n");
    }
    info.err = 0;
out:
    /* upstream writes back only ->err for input-type commands */
    if (copy_to_user(&((struct st_susfs_log __user *)*arg)->err,
                     &info.err, sizeof(info.err)))
        pr_warn("enable_log supercall copy_to_user failed\n");
}
