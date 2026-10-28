// SPDX-License-Identifier: GPL-2.0

/* issue #34: the hide gates ask KernelSU whether it would umount modules for a uid, instead of the
 * old `uid >= 10000` proxy that hid from the manager and from su-granted apps too.
 *
 * The address is resolved ONCE, at load, by name through kallsyms (kernel/symbol_resolver.c) and kept
 * in a function pointer - it is not imported from kernelsu.ko, for two reasons:
 *
 *   - an import would have to be strong.  A __weak one is emitted as a GOT entry
 *     (R_AARCH64_ADR_GOT_PAGE + R_AARCH64_LD64_GOT_LO12_NC), and arm64's module relocator implements
 *     neither, so the module would refuse to load at all - measured on the artifacts, including with
 *     visibility("hidden") and with -fno-pic, neither of which removes that entry (it comes from the
 *     weak reference itself, not from PIC);
 *   - resolving by name answers the environment question directly: the symbol is there or it is not.
 *     Nothing here depends on which loader ran the module or on the SELinux domain it ran in, so no
 *     loader-side detection is needed either.
 *
 * The answer is decided once, here, and the gates only read the flag afterwards.  Two consequences
 * worth knowing.  A KernelSU loaded AFTER this module is not noticed until this module is reloaded
 * (the warning below says so).  A KernelSU that is UNLOADED would leave the saved address dangling -
 * a call would jump into freed module text - so this file keeps a module notifier to clear the
 * pointer when its provider goes away and runs the call inside an RCU read-side section; see
 * susfs_ksu_umount_ask() and susfs_ksu_umount_gate_notify() below. */

#include <linux/kernel.h>
#include <linux/kallsyms.h>	/* KSYM_SYMBOL_LEN, for ksu_symbol_name_of()'s buffer */
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/rcupdate.h>	/* rcu_read_lock(): the call below is guarded by the provider's own synchronize_rcu() */
#include <linux/types.h>

#include "ksu_umount_gate.h"
#include "symbol_resolver.h"
#include "susfs_log.h"

#define KSU_UMOUNT_GATE_SYMBOL	"ksu_uid_should_umount"

/* Fail-safe default: the pre-#34 answer, until the init below proves that KernelSU's symbol exists.
 * A path that skips that init entirely - the fail_layer=2 diagnostic in susfs_main.c is one, and this
 * layer is deliberately not fatal - therefore leaves the gates on the safe branch instead of calling
 * through a NULL pointer.  Only a successful resolve turns it off. */
bool susfs_ksu_umount_gate_off = true;

/* Written once in init, read by every gate through susfs_ksu_umount_ask().  Non-NULL exactly while
 * the gate is off == false, which is the invariant the helper relies on instead of testing the
 * pointer on every call; the default above is what makes that invariant hold even when init is
 * skipped. */
static bool (*ksu_uid_should_umount_fn)(uid_t uid);

/* Name of the module that actually provided the symbol, recorded at resolve time
 * (ksu_symbol_name_of() hands the owner back).  The notifier below uses it to tell "the provider
 * is going away" from "something unrelated was unloaded" - the second must not silently turn
 * issue #34's fix off. */
static char susfs_ksu_umount_gate_owner[MODULE_NAME_LEN];
static struct notifier_block susfs_ksu_umount_gate_nb;
static bool susfs_ksu_umount_gate_nb_registered;

/* noinline __nocfi: an indirect call through a resolved address.  CFI checks indirect call sites, and
 * LTO would otherwise inline a small static helper back into a caller that is not marked.
 *
 * The NULL check is what makes the notifier's window safe, and it is not redundant with the flag:
 * susfs_is_current_proc_umounted_app() reads the flag and then calls this, so a task that loaded
 * the flag before the notifier ran still arrives here - and finds the pointer already cleared
 * rather than calling freed text.  Answering true is the pre-#34 rule, i.e. exactly what the flag
 * being set means.
 *
 * The read and the call are one RCU read-side section, and that is what makes the NULL check
 * sufficient rather than merely likely.  A task that read the pointer BEFORE the notifier cleared it
 * is not covered by the NULL check - it is holding a live address - so the window would otherwise be
 * as long as one preemption: long enough for the provider to finish exit(), be notified and be
 * freed.  With the section held, free_module()'s own synchronize_rcu() (kernel/module.c, right
 * before module_memfree() of the core layout) cannot complete while that task is still inside the
 * call, and the provider's text is only released after it.  Nothing in the section sleeps:
 * susfs_ksu_umount_ask() is reached from kprobe handlers as well as from process context, and
 * KernelSU's predicate is rcu_read_lock() + kref_get_unless_zero() + kfree_rcu() - no mutex (the
 * allowlist mutex is write-side only). */
bool __nocfi noinline susfs_ksu_umount_ask(uid_t uid)
{
	bool (*fn)(uid_t);
	bool answer;

	rcu_read_lock();
	fn = READ_ONCE(ksu_uid_should_umount_fn);
	answer = fn ? fn(uid) : true;
	rcu_read_unlock();
	return answer;
}

/* A kallsyms lookup holds no reference on the module that provides the symbol, so rmmod of that
 * module succeeds and leaves the saved address dangling - a call would jump into freed text.
 * (A strong import would have blocked the rmmod through ref_module()/source_list, which is why
 * this is a real exposure and not an inherited one; see the module's own issue #50.)
 *
 * The notifier runs on MODULE_STATE_GOING, which delete_module() delivers AFTER the provider's
 * exit() and before free_module() (kernel/module.c) - so by the time this clears the pointer the
 * provider's code is still mapped, and it is released only later, after free_module()'s own
 * synchronize_rcu().  Clearing here is therefore enough on its own for every reader that has not
 * yet loaded the pointer; the ones that have are covered by the RCU section in
 * susfs_ksu_umount_ask(). */
static int susfs_ksu_umount_gate_notify(struct notifier_block *nb,
					unsigned long action, void *data)
{
	struct module *mod = data;

	/* nb is the API's own handle for this block, not something to interpret (MISRA C++ 0-1-11:
	 * the parameter is imposed by the notifier interface, and the module never looks at it). */
	(void)nb;

	if (action != MODULE_STATE_GOING || !READ_ONCE(ksu_uid_should_umount_fn))
		return NOTIFY_DONE;
	if (!susfs_ksu_umount_gate_owner[0] ||
	    strcmp(mod->name, susfs_ksu_umount_gate_owner) != 0)
		return NOTIFY_DONE;

	/* Pointer first, flag second: a reader that already passed the flag check has to find the
	 * pointer either still valid or already NULL, never a freed address. */
	WRITE_ONCE(ksu_uid_should_umount_fn, NULL);
	WRITE_ONCE(susfs_ksu_umount_gate_off, true);
	pr_warn("susfs_samsung_lkm: %s went away - the hide gates fall back to the uid >= 10000 rule\n",
		mod->name);
	return NOTIFY_DONE;
}

int susfs_ksu_umount_gate_init(void)
{
	unsigned long addr = ksu_kallsyms_lookup_name(KSU_UMOUNT_GATE_SYMBOL);
	char name[KSYM_SYMBOL_LEN];
	char *owner = NULL;

	if (!addr) {
		susfs_ksu_umount_gate_off = true;
		/* pr_warn, not SUSFS_LOGI: "issue #34's fix is not in effect on this device" is something an
		 * operator has to be able to see, and the load is otherwise silent about it. */
		pr_warn("susfs_samsung_lkm: %s is not in kallsyms - the hide gates keep the uid >= 10000 rule, so issue #34's fix (hiding follows KernelSU's exclusion list) is NOT in effect here.  A KernelSU loaded after this module needs this module reloaded to be picked up.\n",
			KSU_UMOUNT_GATE_SYMBOL);
		return 0;
	}

	/* Which module provides it, so the notifier only reacts to that one - and BEFORE the pointer is
	 * published below.  A vmlinux symbol has no owner and needs no notifier: it cannot be unloaded.
	 * A name that does not fit this buffer is an owner the notifier could never match, i.e. a
	 * provider this layer cannot protect, and keeping an address that can outlive its module is the
	 * one outcome this file exists to prevent - so that case refuses the gate instead, which is the
	 * same answer as "no symbol" (the pre-#34 rule).  MODULE_NAME_LEN bounds both sides, so it
	 * cannot happen today; reading strscpy()'s return is what CERT ERR33-C asks for around a
	 * bounded copy whose result is then used as a key. */
	if (ksu_symbol_name_of(addr, name, &owner) >= 0 && owner) {
		/* The copy is a statement of its own, not the right-hand side of the test above: it
		 * has a persistent side effect, which MISRA C 2012 Rule 13.5 keeps out of a
		 * short-circuited operand. */
		if (strscpy(susfs_ksu_umount_gate_owner, owner,
			    sizeof(susfs_ksu_umount_gate_owner)) < 0) {
			pr_warn("susfs_samsung_lkm: %s comes from %s, whose name does not fit %zu bytes - "
				"the hide gates keep the uid >= 10000 rule\n",
				KSU_UMOUNT_GATE_SYMBOL, owner,
				sizeof(susfs_ksu_umount_gate_owner));
			return 0;
		}
	}

	ksu_uid_should_umount_fn = (bool (*)(uid_t))addr;
	susfs_ksu_umount_gate_off = false;	/* only now is there something to ask */

	susfs_ksu_umount_gate_nb.notifier_call = susfs_ksu_umount_gate_notify;
	if (register_module_notifier(&susfs_ksu_umount_gate_nb) == 0) {
		susfs_ksu_umount_gate_nb_registered = true;
	} else {
		/* Without it the dangling call is back, so say so rather than let the load look clean. */
		pr_warn("susfs_samsung_lkm: register_module_notifier failed - if %s is unloaded while this "
			"module is up, the next hide-gate call jumps into freed text\n",
			KSU_UMOUNT_GATE_SYMBOL);
	}

	SUSFS_LOGI("ksu_umount_gate: resolved %s at %px - the hide gates now answer KernelSU (issue #34)\n",
		   KSU_UMOUNT_GATE_SYMBOL, (void *)addr);
	return 0;
}

/* Runs on unload and on the rollback of a failed load.  Idempotent: the flag is what makes it so,
 * and this layer's init may never have got as far as registering. */
void susfs_ksu_umount_gate_exit(void)
{
	if (!susfs_ksu_umount_gate_nb_registered)
		return;
	susfs_ksu_umount_gate_nb_registered = false;
	/* Checked rather than ignored (CERT ERR33-C): a block the chain did not have would stay
	 * registered, and the kernel would then call it after this module's text is freed.  It cannot
	 * fail - the flag above is only set when registration succeeded - but an operator has to be
	 * able to see it if it ever does. */
	if (unregister_module_notifier(&susfs_ksu_umount_gate_nb) != 0)
		pr_warn("susfs_samsung_lkm: unregister_module_notifier failed - this module's notifier may outlive it\n");
	/* The gates are about to lose the answer they were given, and the module is going away
	 * anyway - but leave the state consistent for a reload in the same boot. */
	WRITE_ONCE(ksu_uid_should_umount_fn, NULL);
	WRITE_ONCE(susfs_ksu_umount_gate_off, true);
}
