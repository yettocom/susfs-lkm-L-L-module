/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Arming a probe a second time needs the struct handed back the way the kernel left it the
 * first time.
 *
 * register_kprobe() resolves .symbol_name only while .addr is NULL: kprobe_addr() ->
 * _kprobe_addr() (kernel/kprobes.c) answers ERR_PTR(-EINVAL) as soon as both are set -
 *
 *     if ((symbol_name && addr) || (!symbol_name && !addr))
 *             goto invalid;
 *     ...
 * invalid:
 *     return ERR_PTR(-EINVAL);
 *
 * - and register_kprobe() stores the address it resolved BEFORE the checks that can still
 * fail it (`p->addr = addr;` sits above warn_kprobe_rereg()/check_kprobe_address_safe()), so
 * the field is written even when the call returns an error.  unregister_kprobes() clears it
 * only for a probe that was never on a list:
 *
 *     for (i = 0; i < num; i++)
 *             if (__unregister_kprobe_top(kps[i]) < 0)
 *                     kps[i]->addr = NULL;    (only on failure)
 *
 * which means a probe that DID arm keeps its address after it is removed.
 *
 * Put together: a struct kprobe that has armed once - or whose registration failed after the
 * symbol was resolved - can never be registered again.  Every later register_kprobe() on that
 * struct returns -EINVAL without touching the address, so the feature it carries stays off for
 * the rest of the module's life, and a failed retry looks exactly like a symbol that is not
 * probeable.
 *
 * Both sequences are ordinary here: every enable/disable control surface disarms and arms
 * again (the avc node, the sus_mount supercall), and every rule-management path retries a
 * registration that failed earlier (kstat's maps hook, open_redirect's forward and reverse
 * hooks, uname's set, sus_map's probes).  So both sides clear the field: the arm side before
 * it returns an error, the disarm side after unregistering.  sus_mount_fdinfo_arm()/disarm()
 * already did that by hand for its dynamically resolved addresses; these two helpers are the
 * same statement, so the next probe does not have to rediscover why it is needed.
 *
 * The struct also keeps KPROBE_FLAG_DISABLED after an unregister (__disable_kprobe() sets it on
 * the way out) and register_kprobe() preserves that one bit from the caller, so the same reuse
 * that needs .addr cleared needs the flag cleared too, or the probe is registered and never armed.
 *
 * A kretprobe embeds its kprobe, hence the second helper.
 */
#ifndef _SUSFS_KPROBE_H
#define _SUSFS_KPROBE_H

#include <linux/kprobes.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/uaccess.h>	/* copy_from_kernel_nofault() */
#include <linux/kallsyms.h>	/* kernel_text_address(): is a stripped x30 real code */

#include "symbol_resolver.h"
#include "susfs_arm64.h"

/* Call AFTER unregister_kprobe()/unregister_kretprobe() returns, never before: unregister_kprobes()
 * runs each probe's bottom half only `if (kps[i]->addr)`, so clearing the field first would skip
 * __unregister_kprobe_bottom() and leave the probe's instruction slot behind.  Call it on every
 * registration failure too, for the reason in the block above. */
static inline void susfs_kp_forget_addr(struct kprobe *kp)
{
	kp->addr = NULL;
	/* An unregistered probe keeps KPROBE_FLAG_DISABLED - __disable_kprobe() sets it, and
	 * register_kprobe() preserves exactly that one bit (`p->flags &= KPROBE_FLAG_DISABLED`)
	 * before it decides whether to arm.  Left set, the next registration succeeds, is listed,
	 * and never fires: the layer reports itself armed while its handler is never called. */
	kp->flags &= ~KPROBE_FLAG_DISABLED;
}

static inline void susfs_krp_forget_addr(struct kretprobe *krp)
{
	krp->kp.addr = NULL;
	krp->kp.flags &= ~KPROBE_FLAG_DISABLED;
}

/*
 * Where a probe may sit on a function that something reaches through a pointer.
 *
 * A probe writes its breakpoint ON the instruction it is given.  When the caller is an indirect
 * branch - sys_call_table[], seq_ops->show, i_op->get_link - the CPU compares the target's first
 * instruction against PSTATE.BTYPE before executing it, so a breakpoint at offset 0 removes the
 * landing pad and the next call raises a branch-type exception instead of running the function.
 * Measured on a vendor 5.15 kernel with CONFIG_ARM64_BTI_KERNEL=y and panic_on_oops=1: `Oops -
 * BTI` at invoke_syscall+0x58, BTYPE=-c, one panic and one reboot, 175 ms after the probe armed.
 *
 * A landing pad is exactly one 4-byte instruction - the four 'bti' forms and the two PAC words -
 * so on such a target the second word is the function's first real instruction, and that is where
 * the probe goes.  The word at +0 does not have to be recognised: whatever it is, this device has
 * been branching onto it for its whole uptime, which is the hardware accepting it as a pad.  An
 * unrecognised pad is logged rather than silently assumed, so a future encoding shows up in the
 * log instead of becoming a guess nobody can see.
 *
 * What a handler must not do is return through x30 directly.  Behind a paciasp/pacibsp that
 * register already holds a signed pointer, so a probe sitting at +4 - the only place an
 * indirect call target may carry one - fires with the signature already in place.  Handlers
 * therefore leave through susfs_kp_skip_call()/susfs_kp_skip_syscall() below.
 *
 * Neither helper arms anything: they fill kp->addr and leave registration to the caller, so every
 * site keeps the register-then-forget-on-failure shape it already had.
 *
 * One ceiling, named: either way a handler leaves, the word at +0 has already run - a probe at +4
 * fires behind it.  On a BTI kernel that word is a pad and running it changed nothing; on a kernel
 * built without BTI it can be the prologue, and leaving from behind it keeps the frame that prologue
 * pushed.  A module cannot tell the two apart at load (see the CONFIG note above
 * hide_syms_resolve_show()), every target this fork ships to is a Samsung 5.15/6.1/6.6 kernel with
 * CONFIG_ARM64_BTI_KERNEL=y, and the alternative - arming at offset 0 - panics those devices
 * outright, so the rule stays as it is.
 */
static inline int susfs_kp_set_indirect_addr_at(struct kprobe *kp, unsigned long fn,
						const char *name)
{
	unsigned long base = fn;
	u32 insn;

	if (!fn)
		return -ENOENT;
	if (copy_from_kernel_nofault(&insn, (void *)fn, sizeof(insn))) {
		pr_err("susfs_kp: %s: the entry is not readable, not arming a probe there\n",
		       name);
		return -EFAULT;
	}
	if (!susfs_arm64_insn_is_pad(insn))
		pr_info("susfs_kp: %s opens with 0x%08x, taking it as a landing pad\n",
			name, insn);
	fn += sizeof(insn);
	if (copy_from_kernel_nofault(&insn, (void *)fn, sizeof(insn))) {
		pr_err("susfs_kp: %s: the word behind the entry is not readable, "
		       "not arming a probe there\n", name);
		return -EFAULT;
	}
	if (susfs_arm64_is_pac(insn))
		pr_info("susfs_kp: %s signs its link register at +4; the probe replaces that "
			"instruction, so the signature never runs\n", name);
	kp->addr = (kprobe_opcode_t *)fn;
	pr_info("susfs_kp: %s+0x%lx\n", name, fn - base);
	return 0;
}

static inline int susfs_kp_set_indirect_addr(struct kprobe *kp, const char *name)
{
	return susfs_kp_set_indirect_addr_at(kp, find_kernel_symbol_exact(name), name);
}

/*
 * How a pre_handler leaves without running the call.
 *
 * On a kernel with pointer authentication the word at a function's entry signs x30, and a probe
 * that must sit behind that word (offset 4, where an indirect-call target has to be probed)
 * fires after the signature is in place.  Jumping to x30 from there jumps to a signed value:
 * the address bits are the caller's return address and the field above them is a MAC.  Measured
 * on a vendor 5.15 kernel with CONFIG_ARM64_PTR_AUTH_KERNEL=y and panic_on_oops=1: pc became
 * 0xeac70440080aff08 - the real 0xffffffc0080aff08 with the MAC across the high bits - and the
 * instruction abort took the device down 19 ms after the module loaded.
 *
 * Two ways out, tried in that order:
 *  - susfs_skip_resume_pc: the instruction after the indirect call in invoke_syscall(), found
 *    once at load.  A constant, so no register can be signed.
 *  - susfs_kp_skip_call(): strip the signature and keep the result only if it is kernel text.
 *    For callers that are not invoke_syscall() - seq_read_iter() reaches seq_operations.show
 *    the same way but has no single resume point to point at.
 */
extern unsigned long susfs_skip_resume_pc;

static inline void susfs_kp_skip_call(struct pt_regs *regs)
{
	unsigned long lr = regs->regs[30];
	unsigned long stripped = susfs_arm64_pac_strip_key_a(lr, regs->sp);

	/* AUTIA over a value that was never signed yields a mixture, not that value, so a result
	 * is only trusted when the kernel would execute from it.  Both keys are tried: pac-ret
	 * signs with the A key, and ruling out the B key costs one instruction. */
	if (!kernel_text_address(stripped)) {
		unsigned long other = susfs_arm64_pac_strip_key_b(lr, regs->sp);

		stripped = kernel_text_address(other) ? other : lr;
	}
	regs->pc = stripped;
}

static inline void susfs_kp_skip_syscall(struct pt_regs *regs)
{
	if (susfs_skip_resume_pc)
		regs->pc = susfs_skip_resume_pc;
	else
		susfs_kp_skip_call(regs);
}

#endif /* _SUSFS_KPROBE_H */
