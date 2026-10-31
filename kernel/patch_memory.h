/* SPDX-License-Identifier: GPL-2.0-only */
/* patch_memory.h - arbitrary kernel text patching (ported from KernelSU/SukiSU).
 * Copyright (C) 2023 bmax121. All Rights Reserved. */

#ifndef __SUSFS_PATCH_MEMORY_H
#define __SUSFS_PATCH_MEMORY_H

#include <linux/types.h>
#include <linux/version.h>

#ifdef __aarch64__
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#include "asm/text-patching.h"
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
#include "asm/patching.h"
#else
#include "asm/insn.h"
#endif
#elif __x86_64__
#include <asm/ptrace.h>
#else
#error "Unsupported arch"
#endif

#define KSU_PATCH_TEXT_FLUSH_DCACHE 1
#define KSU_PATCH_TEXT_FLUSH_ICACHE 2

unsigned long phys_from_virt(unsigned long addr, int *err);
int ksu_patch_memory_init(void);
int ksu_patch_text(void *dst, void *src, size_t len, int flags);

/*
 * The 8-byte "make the slot at @dst point at @src" write, on whichever route the target allows:
 * the plain fixmap write, or - on a Samsung (RKP) target, where that write is taken away at EL2 -
 * a branch at the entry of the function @dst still points at.  On the branch route *@callthru
 * receives the callable entry for the displaced original before the branch becomes reachable, so a
 * wrapper that saved the original keeps working; on the plain route *@callthru is left alone,
 * because there the slot itself is written and the saved pointer stays valid.
 *
 * Restoring is the same call with @src holding what was saved: the slot still names the original,
 * so the branch site is found from it and removed.
 */
int susfs_patch_text_slot(void *dst, void *src, void **callthru);
void *scan_call_to(void *start, size_t size, void *target);

#endif
