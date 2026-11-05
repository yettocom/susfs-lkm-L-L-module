/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SUSFS_ARM64_H
#define __SUSFS_ARM64_H

/*
 * The AArch64 instructions this module has to recognise, in one place.
 *
 * Two questions get asked about the word at a function's entry, and they have different
 * answers, so they live side by side here instead of being spelled out per file.
 *
 * "May an indirect call branch here?" - susfs_fp_hook.c refuses to install a wrapper that has no
 * landing pad (indirect calls into this module would raise a branch-type exception) and
 * susfs_opath.c reports the same test on the device for a wrapper that is not installed yet.
 * Only 'bti c', 'bti jc' and the PAC words answer yes.
 *
 * "Where may a branch be written without taking that pad away?" - susfs_rkp.c steps over the
 * preamble before writing, and there a plain 'bti' counts too: it restricts nothing, so an
 * indirect call onto it is legal, and it was overwriting one of those that raised the BTI
 * violation this module shipped once.  'bti j' is a jump target, so it belongs in this set as
 * well - the branch goes after it, never on it.
 *
 * BTI C / BTI JC are the declared landing pads; a PAC instruction is a valid target too - it is
 * what a pac-ret function starts with, and it is the instruction the branch route has to
 * re-execute for a signed function to return.  On a core without BTI 'bti c' is an unallocated
 * HINT, so testing for it is correct either way.
 */

#include <linux/compiler.h>
#include <linux/types.h>

#define SUSFS_ARM64_BTI		0xd503241fu
#define SUSFS_ARM64_BTI_C	0xd503245fu
#define SUSFS_ARM64_BTI_J	0xd503249fu
#define SUSFS_ARM64_BTI_JC	0xd50324dfu
#define SUSFS_ARM64_PACIASP	0xd503233fu
#define SUSFS_ARM64_PACIBSP	0xd503237fu

/*
 * Two more words, neither of them a landing pad.  The first ends a function body, and the scan
 * in susfs_main.c stops on it so that a missing indirect call cannot walk into the function that
 * follows and pick up an address belonging to it.  The second is the indirect call itself:
 * invoke_syscall() reaches every __arm64_sys_* wrapper through one `blr x20`.
 */
#define SUSFS_ARM64_RET		0xd65f03c0u
#define SUSFS_ARM64_BLR_X20	0xd63f0280u

/* Pointer authentication, i.e. an instruction that signs the return address. */
static inline bool susfs_arm64_is_pac(u32 insn)
{
	return insn == SUSFS_ARM64_PACIASP || insn == SUSFS_ARM64_PACIBSP;
}

/* Exactly the four words an indirect CALL may branch to, which is upstream's own test
 * (fp_has_landing_pad() in susfs_fp_hook.c).  A mask would also accept plain `bti` and
 * `bti j`: those are jump targets, not call targets, and a BLR onto one raises a
 * branch-type exception on a BTI kernel. */
static inline bool susfs_arm64_insn_is_landing_pad(u32 insn)
{
	switch (insn) {
	case SUSFS_ARM64_BTI_C:
	case SUSFS_ARM64_BTI_JC:
		return true;
	default:
		return susfs_arm64_is_pac(insn);
	}
}

/* The words a branch has to step over, i.e. the set above plus the two BTI variants that are
 * not call targets: a plain 'bti' restricts nothing, so an indirect call onto it is legal, and
 * overwriting one with a branch is what raised the BTI violation in the field.  'bti j' takes
 * jumps only, which still makes it a word no branch may be written on.  Used by the preamble
 * walk in susfs_rkp.c; the narrow test above stays what the wrapper checks use. */
static inline bool susfs_arm64_insn_is_pad(u32 insn)
{
	return insn == SUSFS_ARM64_BTI || insn == SUSFS_ARM64_BTI_J ||
	       susfs_arm64_insn_is_landing_pad(insn);
}

/* The same test against the word at a function's entry, without dereferencing NULL. */
static inline bool susfs_arm64_fn_has_landing_pad(const void *fn)
{
	return fn && susfs_arm64_insn_is_landing_pad(READ_ONCE(*(const u32 *)fn));
}

/*
 * Take a pointer-authentication signature off @addr, using @sp the way the signing instruction
 * used it - the stack pointer the callee had when it ran paciasp/pacibsp.
 *
 * AUTIA recomputes the signature from the modifier and the address and writes the result back,
 * so a value that was never signed does not come back unchanged: it comes back mixed.  Callers
 * have to decide what to do with that - susfs_kp_skip_call() keeps the result only when it is a
 * kernel text address.
 *
 * The modifier is passed in a register (autia xN, xM) instead of moving sp, so an interrupt
 * taken inside this sequence still sees the stack it interrupted, and nothing here writes
 * memory or touches the link register.
 *
 * The instructions go in as .inst words rather than mnemonics.  This module is built with
 * ThinLTO and the LTO backend assembles inline asm without the -mbranch-protection the kernel
 * compiles its C with, so "autia" is refused there with "instruction requires: pauth" even on an
 * image that has CONFIG_ARM64_PTR_AUTH_KERNEL=y.  The architecture fixes the encoding and .inst
 * reaches the object as written: autia x9, x10 = 0xdac10149, autib x9, x10 = 0xdac10549.
 */
static __always_inline unsigned long susfs_arm64_pac_strip_key_a(unsigned long addr,
							 unsigned long sp)
{
	/* The modifier goes into x10 first and the pointer into x9 after it, so neither copy can
	 * destroy the other one whatever registers the compiler picked; both operands are
	 * read-write, which keeps them out of the two registers named below. */
	asm volatile("mov\tx10, %1\n"
		     "mov\tx9, %0\n"
		     ".inst\t0xdac10149\n"	/* autia x9, x10 */
		     "mov\t%0, x9"
		     : "+r" (addr), "+r" (sp)
		     :
		     : "x9", "x10", "memory");
	return addr;
}

static __always_inline unsigned long susfs_arm64_pac_strip_key_b(unsigned long addr,
							 unsigned long sp)
{
	asm volatile("mov\tx10, %1\n"
		     "mov\tx9, %0\n"
		     ".inst\t0xdac10549\n"	/* autib x9, x10 */
		     "mov\t%0, x9"
		     : "+r" (addr), "+r" (sp)
		     :
		     : "x9", "x10", "memory");
	return addr;
}

#endif /* __SUSFS_ARM64_H */
