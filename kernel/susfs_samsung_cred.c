// SPDX-License-Identifier: GPL-2.0-only
/*
 * susfs_samsung_cred.c - credential references through Samsung's KDP helpers.
 *
 * A Samsung kernel keeps a credential's usecount and its non_rcu flag in memory that is protected
 * at EL2, so the plain inline get_cred()/put_cred() this module compiles out of <linux/cred.h>
 * write to a protected page.  That is not a failed write: it is the fault RMG recorded as the
 * original S25U panic (a generic inline put_cred() in ksu_mark_running_process_locked), and it is
 * why KernelSU's Samsung patch routes every credential release through
 * ksu_samsung_kdp_put_cred() - the same shape this file repeats for susfs.
 *
 * Samsung's own <linux/cred.h> answers those two fields with kdp_usecount_inc() /
 * kdp_usecount_dec_and_test() / kdp_set_cred_non_rcu(), and those symbols are in the kernel's
 * kallsyms - measured as `T` in the S24U ZI1 symbol table (24u/ko-build/24u_symtab.txt:47166-47169).
 * Resolving them by name is therefore enough; nothing has to be exported for us.
 *
 * sus_path is the only place in this module that takes or drops a credential reference of its own.
 * override_creds() and revert_creds() are the kernel's own functions, so they already use Samsung's
 * implementation and are not touched here; the two read-only uses of current_cred() (sus_mount.c,
 * susfs_open_redirect.c) never touch a refcount.  Everywhere else, the standard inline pair is
 * exactly right, and on a kernel without the helpers - a non-Samsung one, or a Samsung one built
 * with KDP off - that is what this file falls back to, which is upstream's behaviour verbatim.
 *
 * All-or-nothing: one missing helper puts the whole pair back on the standard path, because a
 * get_cred() that incremented through KDP and a put_cred() that decremented inline would leave the
 * counter somewhere neither implementation agrees on.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cred.h>
#include <linux/version.h>

#include "symbol_resolver.h"
#include "susfs_log.h"
#include "susfs_samsung_cred.h"

#if SUSFS_SAMSUNG_NO_PATCH_TEXT

typedef void (*kdp_usecount_inc_t)(struct cred *cred);
typedef void (*kdp_set_cred_non_rcu_t)(struct cred *cred, int val);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
typedef unsigned int (*kdp_usecount_sub_and_test_t)(int nr, struct cred *cred);
#else
typedef unsigned int (*kdp_usecount_dec_and_test_t)(struct cred *cred);
#endif

static kdp_usecount_inc_t kdp_usecount_inc_fn;
static kdp_set_cred_non_rcu_t kdp_set_cred_non_rcu_fn;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static kdp_usecount_sub_and_test_t kdp_usecount_dec_fn;
#else
static kdp_usecount_dec_and_test_t kdp_usecount_dec_fn;
#endif

static bool samsung_cred_ready;

void susfs_samsung_cred_init(void)
{
	if (READ_ONCE(samsung_cred_ready))
		return;

	kdp_usecount_inc_fn = (kdp_usecount_inc_t)find_kernel_symbol_exact("kdp_usecount_inc");
	kdp_set_cred_non_rcu_fn =
		(kdp_set_cred_non_rcu_t)find_kernel_symbol_exact("kdp_set_cred_non_rcu");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	kdp_usecount_dec_fn = (kdp_usecount_sub_and_test_t)find_kernel_symbol_exact(
		"kdp_usecount_sub_and_test");
#else
	kdp_usecount_dec_fn = (kdp_usecount_dec_and_test_t)find_kernel_symbol_exact(
		"kdp_usecount_dec_and_test");
#endif

	if (!kdp_usecount_inc_fn || !kdp_set_cred_non_rcu_fn || !kdp_usecount_dec_fn) {
		/* Not a degradation worth warning about on every non-Samsung kernel: the standard
		 * path is what upstream uses and what this module did before this file existed. */
		SUSFS_LOGI("susfs: Samsung KDP credential helpers absent (%px/%px/%px) - "
			   "using the standard credential path\n",
			   (void *)kdp_usecount_inc_fn, (void *)kdp_set_cred_non_rcu_fn,
			   (void *)kdp_usecount_dec_fn);
		return;
	}

	/* WRITE_ONCE, matching the READ_ONCE in the pair below: this flag is what publishes the three
	 * addresses resolved above, and the module marks both sides of such a flag even where the
	 * ordering is already given by init running before any hook that uses the pair is armed. */
	WRITE_ONCE(samsung_cred_ready, true);
	SUSFS_LOGI("susfs: Samsung KDP credential helpers resolved (inc=%px non_rcu=%px dec=%px)\n",
		   (void *)kdp_usecount_inc_fn, (void *)kdp_set_cred_non_rcu_fn,
		   (void *)kdp_usecount_dec_fn);
}

/*
 * Every call through a resolved kernel address carries __nocfi, and is noinline so LTO cannot
 * fold the wrapper back into an instrumented caller - the same shape ksu_umount_gate.c uses for
 * susfs_ksu_umount_ask().  kCFI checks the type id at an indirect call site, and a helper that
 * was compiled without one (or whose signature differs by a word) turns the call into a kernel
 * panic instead of a failed call.  Measured on this kernel: CONFIG_CFI_CLANG=y.
 */
static __nocfi noinline void susfs_kdp_usecount_inc(struct cred *cred)
{
	kdp_usecount_inc_fn(cred);
}

static __nocfi noinline void susfs_kdp_set_cred_non_rcu(struct cred *cred, int val)
{
	kdp_set_cred_non_rcu_fn(cred, val);
}

static __nocfi noinline bool susfs_kdp_usecount_dec_and_test(struct cred *cred)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	return kdp_usecount_dec_fn(1, cred) != 0;
#else
	return kdp_usecount_dec_fn(cred) != 0;
#endif
}

const struct cred *susfs_samsung_get_cred(const struct cred *cred)
{
	struct cred *c = (struct cred *)cred;

	if (!c || !READ_ONCE(samsung_cred_ready))
		return get_cred(cred);

	/* get_cred() is get_new_cred() plus `non_rcu = 0`, and on this kernel both of those
	 * fields are the protected ones - so both go through KDP. */
	susfs_kdp_set_cred_non_rcu(c, 0);
	susfs_kdp_usecount_inc(c);
	return c;
}

void susfs_samsung_put_cred(const struct cred *cred)
{
	struct cred *c = (struct cred *)cred;

	if (!c)
		return;

	if (!READ_ONCE(samsung_cred_ready)) {
		put_cred(cred);
		return;
	}

	/* __put_cred() only when the count reached zero, exactly as put_cred() does - the kernel
	 * frees the credential and its ucounts there, which is the half KDP does not replace. */
	if (susfs_kdp_usecount_dec_and_test(c))
		__put_cred(c);
}

#else /* !SUSFS_SAMSUNG_NO_PATCH_TEXT */

/* Not the Samsung target: keep upstream's inline pair, and keep the header's contract. */
void susfs_samsung_cred_init(void)
{
}

const struct cred *susfs_samsung_get_cred(const struct cred *cred)
{
	return get_cred(cred);
}

void susfs_samsung_put_cred(const struct cred *cred)
{
	put_cred(cred);
}

#endif /* SUSFS_SAMSUNG_NO_PATCH_TEXT */
