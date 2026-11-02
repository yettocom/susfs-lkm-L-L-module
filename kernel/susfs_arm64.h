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

#endif /* __SUSFS_ARM64_H */
