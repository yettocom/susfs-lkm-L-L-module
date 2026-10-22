/*
 * Samsung RKP-aware function replacement.
 *
 * See susfs_rkp.h for why this exists.  The short version: on a target whose
 * text and rodata are taken away at EL2, ksu_patch_text()'s fixmap write is refused, so a
 * caller that wants a table entry to point somewhere else cannot write the entry.  This file
 * writes the *function* instead - one B instruction at its entry - which the kernel's own
 * text-patching path (aarch64_insn_patch_text(), the one kprobes use) does allow.
 *
 * A stub next to it keeps the original callable.  The stub has to be reached with a plain
 * call: it lives in a page no module owns, so a CFI-checked indirect call to it would fail
 * the check, and callers that reach it are marked __nocfi for that reason.
 *
 * Ported from KernelSU's Samsung patch (kernel/compat/samsung_rkp.c, same tree) and kept as a
 * copy rather than a shared symbol on purpose: sharing would mean resolving the other module's
 * function by name and inheriting its lifetime, and its "replace the target for this slot" API
 * has no chain mode - a second request for the same function would silently replace the first.
 * Two independent implementations instead refuse the second request cleanly: a branch already at
 * fn+0 is PC-relative, and susfs_rkp_is_pc_relative() turns that into -EINVAL instead of a
 * half-armed hook.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/moduleloader.h>
#include <linux/set_memory.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "symbol_resolver.h"
#include "susfs_rkp.h"
#include "susfs_log.h"

#ifdef __aarch64__

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#include <linux/execmem.h>
#define susfs_rkp_alloc(sz) execmem_alloc(EXECMEM_MODULE_TEXT, (sz))
#define susfs_rkp_free(p) execmem_free(p)
#else
#define susfs_rkp_alloc(sz) module_alloc(sz)
#define susfs_rkp_free(p) vfree(p)
#endif

#define SUSFS_RKP_MAX_SITES 16

/*
 * One stub is 16 bytes, entry at +4:
 *   [0] the replaced function's kCFI type hash (it sits at fn-4).  Every caller here is
 *       __nocfi so it is never consulted on this kernel; it is what would make the stub a
 *       valid indirect target on a kernel that does check
 *   [4] a landing pad: the PAC instruction that was at fn+0 when it was replaced, or BTI C
 *   [8] the instruction the branch displaced, re-executed before returning to the function
 *  [12] B back to the first instruction after the displaced one
 */
#define SUSFS_RKP_STUB_SIZE 16
#define SUSFS_RKP_STUBS_PER_PAGE (PAGE_SIZE / SUSFS_RKP_STUB_SIZE)

#define SUSFS_RKP_INSN_BTI_C 0xD503245Fu
#define SUSFS_RKP_INSN_PACIASP 0xD503233Fu
#define SUSFS_RKP_INSN_PACIBSP 0xD503237Fu
#define SUSFS_RKP_INSN_B_OPC 0x14000000u

typedef int (*susfs_rkp_insn_patch_t)(void *addrs[], u32 insns[], int cnt);

struct rkp_site {
    void *fn;
    u32 insn_at_patch; /* what the branch displaced, put back on restore */
    u32 insn_at_fn0;   /* the PAC instruction at fn+0 that was replaced, or 0 */
    unsigned int stub_idx;
    unsigned int preamble_skip;
};

static susfs_rkp_insn_patch_t rkp_insn_patch;
static struct rkp_site rkp_sites[SUSFS_RKP_MAX_SITES];
static unsigned int rkp_site_count;
static DEFINE_MUTEX(rkp_site_lock);
static void *rkp_stub_page;
static unsigned int rkp_stub_next;
static bool rkp_armed;

bool susfs_rkp_active(void)
{
    return READ_ONCE(rkp_armed);
}

/* A B instruction, or 0 when @to is out of the +/-128 MB a B can reach. */
static u32 susfs_rkp_encode_b(void *from, void *to)
{
    s64 delta = (s64)(unsigned long)to - (s64)(unsigned long)from;
    s64 imm26 = delta >> 2;

    if (delta & 0x3)
        return 0;
    if (imm26 < -(1LL << 25) || imm26 > (1LL << 25) - 1)
        return 0;

    return SUSFS_RKP_INSN_B_OPC | ((u32)imm26 & 0x3FFFFFFu);
}

static bool susfs_rkp_is_pac_signing(u32 insn)
{
    return insn == SUSFS_RKP_INSN_PACIASP || insn == SUSFS_RKP_INSN_PACIBSP;
}

/* BTI variants are bits[31:8] = 0xD50324 with an even op2 and bits[4:0] = 0x1F; the PAC
 * instructions are landing pads too, which is why they are accepted here. */
static bool susfs_rkp_is_landing_pad(u32 insn)
{
    return (insn & 0xFFFFFF3Fu) == 0xD503241Fu || susfs_rkp_is_pac_signing(insn);
}

/* An instruction whose meaning depends on the address it sits at cannot be moved into a
 * stub: it would compute against the stub instead of the function. */
static bool susfs_rkp_is_pc_relative(u32 insn)
{
    return ((insn & 0x1F000000u) == 0x10000000u) || /* ADR / ADRP        */
           ((insn & 0x3B000000u) == 0x18000000u) || /* LDR literal       */
           ((insn & 0x7C000000u) == 0x14000000u) || /* B / BL            */
           ((insn & 0xFE000000u) == 0x54000000u) || /* B.cond / BC.cond  */
           ((insn & 0x7E000000u) == 0x34000000u) || /* CBZ / CBNZ        */
           ((insn & 0x7E000000u) == 0x36000000u);   /* TBZ / TBNZ        */
}

static struct rkp_site *susfs_rkp_find_site(void *fn)
{
    unsigned int i;

    for (i = 0; i < rkp_site_count; i++) {
        if (rkp_sites[i].fn == fn)
            return &rkp_sites[i];
    }

    return NULL;
}

/*
 * Put a site back exactly as it was found.  When fn+0 was replaced, both words go back in one
 * patch - the same way they went in.  Restoring them separately would leave a window in which
 * fn+0 is the original again while the branch is still there, and a caller that arrives then
 * runs the function without its PAC sign and dies at the epilogue.
 */
static int susfs_rkp_restore_site(struct rkp_site *site)
{
    void *patch_addr = (u8 *)site->fn + site->preamble_skip * 4;
    int ret;

    if (site->insn_at_fn0) {
        void *addrs[2] = { patch_addr, site->fn };
        u32 insns[2] = { site->insn_at_patch, site->insn_at_fn0 };

        ret = rkp_insn_patch(addrs, insns, 2);
    } else {
        u32 insn = site->insn_at_patch;

        ret = rkp_insn_patch(&patch_addr, &insn, 1);
    }

    if (ret)
        pr_err("rkp: restore at %pS failed: %d\n", site->fn, ret);

    return ret;
}

/*
 * Fill a stub.  The page is read-only and executable, so the words go in through the same
 * path kprobes use for their instruction slots - which also does the cache maintenance a
 * bare store into a fresh executable page would need.  All four words go in as one patch,
 * so a stub is never reachable half-written.
 */
static int susfs_rkp_fill_stub(void *stub, void *fn, void *resume, u32 displaced, u32 landing)
{
    void *addrs[4];
    u32 insns[4];
    u32 b_resume = susfs_rkp_encode_b((u8 *)stub + 12, resume);
    unsigned int i;

    if (!b_resume) {
        pr_err("rkp: stub for %pS cannot reach %pS\n", fn, resume);
        return -ERANGE;
    }

    memcpy(&insns[0], (u8 *)fn - sizeof(u32), sizeof(insns[0]));
    insns[1] = landing;
    insns[2] = displaced;
    insns[3] = b_resume;

    for (i = 0; i < ARRAY_SIZE(insns); i++)
        addrs[i] = (u8 *)stub + i * sizeof(u32);

    return rkp_insn_patch(addrs, insns, ARRAY_SIZE(insns));
}

static int susfs_rkp_ensure_stub_page(void)
{
    int ret;

    if (rkp_stub_page)
        return 0;

    rkp_stub_page = susfs_rkp_alloc(PAGE_SIZE);
    if (!rkp_stub_page)
        return -ENOMEM;

    /* A page that is still writable must not be handed out as code. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    ret = set_memory_rox((unsigned long)rkp_stub_page, 1);
#else
    ret = set_memory_ro((unsigned long)rkp_stub_page, 1);
    if (!ret)
        ret = set_memory_x((unsigned long)rkp_stub_page, 1);
#endif
    if (ret) {
        susfs_rkp_free(rkp_stub_page);
        rkp_stub_page = NULL;
    }

    return ret;
}

/*
 * Replace the function @dst points at with the function @src points at.
 *
 * Both are pointers to function pointers, which is ksu_patch_text()'s shape; reading through
 * them here is what lets patch_memory.c forward an 8-byte slot write without knowing anything
 * else about the caller.
 */
int susfs_rkp_branch_patch_callthru(void *dst, void *src, void **callthru)
{
    void *fn;
    void *new_fn;
    struct rkp_site *site;
    void *patch_addr;
    void *resume;
    void *stub;
    u32 fn0_insn;
    u32 displaced;
    u32 landing;
    u32 branch;
    unsigned int preamble_skip;
    int ret;

    if (!rkp_armed || !dst || !src)
        return -EOPNOTSUPP;

    fn = *(void **)dst;
    new_fn = *(void **)src;
    if (!fn || !new_fn)
        return -EINVAL;

    mutex_lock(&rkp_site_lock);

    site = susfs_rkp_find_site(fn);
    if (site) {
        ret = susfs_rkp_restore_site(site);
        if (ret) {
            mutex_unlock(&rkp_site_lock);
            return ret;
        }

        *site = rkp_sites[--rkp_site_count];
        SUSFS_LOGI("rkp: branch site at %pS removed\n", fn);
        if (new_fn == fn) {
            mutex_unlock(&rkp_site_lock);
            return 0;
        }
    }

    if (fn == new_fn) {
        mutex_unlock(&rkp_site_lock);
        return 0;
    }

    if (rkp_site_count >= SUSFS_RKP_MAX_SITES) {
        mutex_unlock(&rkp_site_lock);
        pr_err("rkp: branch site table is full\n");
        return -ENOMEM;
    }

    ret = susfs_rkp_ensure_stub_page();
    if (ret) {
        mutex_unlock(&rkp_site_lock);
        pr_err("rkp: no executable page for stubs: %d\n", ret);
        return ret;
    }

    if (rkp_stub_next >= SUSFS_RKP_STUBS_PER_PAGE) {
        mutex_unlock(&rkp_site_lock);
        pr_err("rkp: stub page is full\n");
        return -ENOMEM;
    }

    /*
     * shortcut: stub slots are a bump allocator and are never handed back, so a site that is
     * installed and removed repeatedly (one selinux_hide toggle is four sites) runs the page
     * out after 64 toggles and the install then fails with -ENOMEM; reclaiming a slot needs a
     * grace period for callers still inside the old stub.
     */
    fn0_insn = *(u32 *)fn;
    displaced = fn0_insn;

    /* Step over the BTI/PAC preamble (at most two instructions) so the branch does not
     * clobber the landing pad: an indirect call arrives with BLR, and a function whose
     * fn+0 no longer takes that call raises a BTI violation instead of running. */
    preamble_skip = 0;
    while (preamble_skip < 2 && susfs_rkp_is_landing_pad(displaced)) {
        preamble_skip++;
        displaced = *(u32 *)((u8 *)fn + preamble_skip * 4);
    }

    patch_addr = (u8 *)fn + preamble_skip * 4;
    resume = (u8 *)fn + (preamble_skip + 1) * 4;

    if (susfs_rkp_is_pc_relative(displaced)) {
        mutex_unlock(&rkp_site_lock);
        pr_err("rkp: %pS: instruction at +%u is PC-relative, not installing\n", fn,
               preamble_skip * 4);
        return -EINVAL;
    }

    /* PACIASP/PACIBSP at fn+0 signs LR in place.  Left there, it would sign before the
     * branch, and a replacement that signs again in its own entry makes AUTXSP fail at its
     * epilogue - a fatal FPAC exception.  BTI C keeps the landing pad without signing. */
    landing = SUSFS_RKP_INSN_BTI_C;
    if (preamble_skip > 0 && susfs_rkp_is_pac_signing(fn0_insn))
        landing = fn0_insn;

    branch = susfs_rkp_encode_b(patch_addr, new_fn);
    if (!branch) {
        mutex_unlock(&rkp_site_lock);
        pr_err("rkp: %pS -> %pS is out of branch range\n", fn, new_fn);
        return -ERANGE;
    }

    stub = (u8 *)rkp_stub_page + rkp_stub_next * SUSFS_RKP_STUB_SIZE;
    ret = susfs_rkp_fill_stub(stub, fn, resume, displaced, landing);
    if (ret) {
        mutex_unlock(&rkp_site_lock);
        return ret;
    }

    /*
     * Publish the callable entry for the displaced original BEFORE the branch goes live.  A
     * caller that arrives in between must reach the stub - which runs the instruction the branch
     * displaced and resumes past it, i.e. the original function - and not the function whose
     * entry is about to become the branch.  The other order is not a window but a permanent
     * infinite recursion on whatever path the caller saves its original from, and it is the one
     * failure this parameter exists to prevent.
     *
     * The stub is a valid entry in both states, so a caller that keeps it never has to be told
     * that the branch write below failed - it just runs the original either way.
     */
    if (callthru)
        WRITE_ONCE(*callthru, (void *)((u8 *)stub + 4));

    /* fn+0 and the branch go in together: a CPU that saw one without the other would either
     * lose the landing pad or branch from a signed frame. */
    if (landing == fn0_insn && fn0_insn) {
        void *addrs[2] = { fn, patch_addr };
        u32 insns[2] = { SUSFS_RKP_INSN_BTI_C, branch };

        ret = rkp_insn_patch(addrs, insns, 2);
    } else {
        ret = rkp_insn_patch(&patch_addr, &branch, 1);
    }

    if (ret) {
        mutex_unlock(&rkp_site_lock);
        pr_err("rkp: patching %pS failed: %d\n", fn, ret);
        return ret;
    }

    rkp_sites[rkp_site_count].fn = fn;
    rkp_sites[rkp_site_count].insn_at_patch = displaced;
    rkp_sites[rkp_site_count].insn_at_fn0 = (landing == fn0_insn && fn0_insn) ? fn0_insn : 0;
    rkp_sites[rkp_site_count].stub_idx = rkp_stub_next;
    rkp_sites[rkp_site_count].preamble_skip = preamble_skip;
    rkp_site_count++;
    rkp_stub_next++;

    SUSFS_LOGI("rkp: %pS -> %pS (stub %u)\n", fn, new_fn,
            rkp_sites[rkp_site_count - 1].stub_idx);

    mutex_unlock(&rkp_site_lock);
    return 0;
}

int susfs_rkp_branch_patch(void *dst, void *src)
{
    return susfs_rkp_branch_patch_callthru(dst, src, NULL);
}

void *susfs_rkp_get_stub(void *fn)
{
    struct rkp_site *site;
    void *stub = NULL;

    if (!rkp_armed || !fn)
        return NULL;

    mutex_lock(&rkp_site_lock);
    site = susfs_rkp_find_site(fn);
    if (site && rkp_stub_page)
        stub = (u8 *)rkp_stub_page + site->stub_idx * SUSFS_RKP_STUB_SIZE + 4;
    mutex_unlock(&rkp_site_lock);

    return stub;
}

int susfs_rkp_init(void)
{
    rkp_insn_patch = (susfs_rkp_insn_patch_t)find_kernel_symbol_exact("aarch64_insn_patch_text");

    if (!rkp_insn_patch) {
        SUSFS_LOGI("rkp: instruction patching is not reachable, staying off\n");
        return 0;
    }

    /* RKP is the reason the direct route is unavailable; without it the plain route is both
     * cheaper and better tested, so the branch route stays off. */
    if (!find_kernel_symbol_exact("rkp_started")) {
        SUSFS_LOGI("rkp: no RKP on this kernel, the direct route is used\n");
        return 0;
    }

    rkp_armed = true;
    SUSFS_LOGI("rkp: RKP detected, function slots are served by branch patching\n");
    return 0;
}

void susfs_rkp_exit(void)
{
    struct rkp_site *site;
    bool restored = true;

    mutex_lock(&rkp_site_lock);

    while (rkp_site_count > 0) {
        site = &rkp_sites[rkp_site_count - 1];
        if (susfs_rkp_restore_site(site)) {
            pr_warn("rkp: %pS keeps its branch\n", site->fn);
            restored = false;
        }
        rkp_site_count--;
    }

    /* A branch that is still installed reaches into the stub page, so the page has to
     * outlive the module in that case; the alternative is a jump into freed memory. */
    if (rkp_stub_page && restored) {
        set_memory_nx((unsigned long)rkp_stub_page, 1);
        susfs_rkp_free(rkp_stub_page);
        rkp_stub_page = NULL;
        rkp_stub_next = 0;
    } else if (rkp_stub_page) {
        pr_err("rkp: a branch is still installed, keeping the stub page\n");
    }

    rkp_armed = false;
    mutex_unlock(&rkp_site_lock);
}

#else /* !__aarch64__ */

bool susfs_rkp_active(void)
{
    return false;
}

int susfs_rkp_branch_patch(void *dst, void *src)
{
    return -EOPNOTSUPP;
}

int susfs_rkp_branch_patch_callthru(void *dst, void *src, void **callthru)
{
    return -EOPNOTSUPP;
}

void *susfs_rkp_get_stub(void *fn)
{
    return NULL;
}

int susfs_rkp_init(void)
{
    return 0;
}

void susfs_rkp_exit(void)
{
}

#endif /* __aarch64__ */
