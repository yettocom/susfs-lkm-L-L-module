#!/usr/bin/env bash
# Samsung LSM carrier audit.
#
# The 244 lines the Samsung build wraps around lsm_hook.{c,h} and sus_path.c exist for ONE
# reason: our hooks are insert-shaped (they go at the HEAD of security_hook_heads.<member>),
# and RKP refuses that store.  The obvious way to delete them - move the hooks to upstream's
# replace shape, which writes an 8-byte function-pointer slot and therefore now travels the
# branch patcher - does not work on any of our three targets, and this script is the check
# that keeps that from being re-litigated (or half-applied) later.
#
# Stops with a non-zero exit if a condition the deletion depends on no longer holds.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

fail=0
say() { printf '%-58s %s\n' "$1" "$2"; }
bad() { say "$1" "FAIL: $2"; fail=1; }

# 1. Our hooks must stay insert-shaped: replace needs one symbol per hook and, below 6.12, a way
#    to run the displaced function, which the upstream code does not provide (see 2.).
n=$(grep -h 'KSU_LSM_HOOK_INIT' kernel/*.c 2>/dev/null | grep -cv '^#define')
[ "$n" -eq 0 ] || bad "replace shape unused" "$n use site(s) in kernel/*.c"

# 2. Below 6.12 SUS_LSM_PASS_ORIG is empty, so a replaced hook that returns 0 swallows
#    SELinux's decision instead of forwarding it - fail-open for every hook it took over.
#    Both branches must exist and the empty one must be the <6.12 side; count, don't guess.
empty=$(grep -c 'define SUS_LSM_PASS_ORIG(hook, member, ...) do { } while (0)' kernel/sus_path.c)
real=$(grep -c 'define SUS_LSM_PASS_ORIG(hook, member, ...)' kernel/sus_path.c)
say "PASS_ORIG definitions (live / empty)" "$real / $empty"
if [ "$empty" -eq 1 ] && [ "$real" -eq 2 ]; then
    say "PASS_ORIG empty below 6.12" "yes - replace cannot forward (5.15/6.1/6.6 all < 6.12)"
elif [ "$empty" -eq 0 ]; then
    bad "PASS_ORIG below 6.12" "no empty branch left - replace may have become safe, re-check"
else
    bad "PASS_ORIG layout" "expected 1 live + 1 empty definition, got $real / $empty"
fi

# 3. Every hook needs both carriers, else one build silently applies fewer hooks than the other.
#    Count call sites only: the macro definitions start at column 0 with #define.
k=$(grep -c '^SUS_PATH_KPROBE(' kernel/sus_path.c)
i=$(grep -c '^[[:space:]]\+SUS_PATH_LSM_INSERT(' kernel/sus_path.c)
say "hook carriers (kprobe / insert)" "$k / $i"
[ "$k" -eq "$i" ] || bad "carrier parity" "$k kprobe vs $i insert"

# 4. Insert writes a 4/8-byte data slot (hlist head), which the branch patcher must never be
#    handed: it would treat the node pointer as a function and patch its first word.
if grep -q 'susfs_patch_text_slot' kernel/lsm_hook.c; then
    bad "slot router in lsm_hook.c" "the hlist head is data, not a function slot"
else
    say "slot router not used for LSM insert" "correct (head store is data)"
fi

# 5. Upstream itself ships replace as API only - no use site.  If that ever changes, taking
#    their shape becomes a real option and this audit should be revisited.
u=_pristine-r18.6/kernel
if [ -d "$u" ]; then
    nu=$(grep -h 'KSU_LSM_HOOK_INIT' "$u"/*.c 2>/dev/null | grep -cv '^#define')
    say "upstream replace use sites (r18.6)" "$nu"
    [ "$nu" -eq 0 ] || say "  note" "upstream now uses replace - revisit this decision"
fi

# 6. The branch route reads the slot's *content* as a function address, so it has to know the
#    target is text before it reads that address's first word.  kernel_text_address() is
#    resolved optionally at run time (a kernel without it only loses the check, never the
#    route), but the check itself must stay in the code.
if grep -q 'kernel_text_address' kernel/susfs_rkp.c; then
    say "slot target is text-checked" "yes (kernel_text_address, resolved optionally)"
else
    bad "slot target is text-checked" "no kernel_text_address guard in kernel/susfs_rkp.c"
fi
if grep -q 'a caller that passed the unlocked check above has to look again' kernel/susfs_rkp.c; then
    say "stub page survives the unlocked check" "yes (re-checked under rkp_site_lock)"
else
    bad "stub page survives the unlocked check" "exit can free the page under a live caller"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "OK: the Samsung LSM carrier stays kprobe-shaped; replace is not safe on 5.15/6.1/6.6."
else
    echo "FAILED: do not fold the LSM carrier into upstream's shape on these findings."
fi
exit "$fail"
