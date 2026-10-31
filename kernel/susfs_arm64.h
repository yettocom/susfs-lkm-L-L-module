/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SUSFS_ARM64_H
#define __SUSFS_ARM64_H

/*
 * The AArch64 instructions this module has to recognise, in one place.
 *
 * Three files ask the same question - "is the word at a function's entry a landing pad an
 * indirect call may branch to?" - for three different reasons: susfs_rkp.c decides where the
 * stub's copy of the displaced instruction starts, susfs_fp_hook.c refuses to install a wrapper
 * that has none (indirect calls into this module would raise a branch-type exception), and
 * susfs_opath.c reports it on the device for a wrapper that is not installed yet.  A second copy
 * of the masks is a second answer to keep in step, so the answer lives here and nobody else
 * spells the constants out.
 *
 * BTI C / BTI JC are the declared landing pads; a PAC instruction is a valid target too - it is
 * what a pac-ret function starts with, and it is the instruction the branch route has to
 * re-execute for a signed function to return.  On a core without BTI 'bti c' is an unallocated
 * HINT, so testing for it is correct either way.
 */

#include <linux/compiler.h>
#include <linux/types.h>

#define SUSFS_ARM64_BTI_C	0xd503245fu
#define SUSFS_ARM64_BTI_JC	0xd50324dfu
#define SUSFS_ARM64_PACIASP	0xd503233fu
#define SUSFS_ARM64_PACIBSP	0xd503237fu

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

/* The same test against the word at a function's entry, without dereferencing NULL. */
static inline bool susfs_arm64_fn_has_landing_pad(const void *fn)
{
	return fn && susfs_arm64_insn_is_landing_pad(READ_ONCE(*(const u32 *)fn));
}

#endif /* __SUSFS_ARM64_H */
