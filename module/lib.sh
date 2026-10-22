#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# What the entry-point scripts share: the KMI selector and the module.prop status writer.
#
# Every file ksud can run has to exist under its own name (module.rs maps a stage straight onto
# "<stage>.sh"), so the file count is fixed by ksud; what can be reduced is the duplicated logic
# inside them.  This one file is what is left of two helpers that used to be sourced separately.
#
# Status = a Steins;Gate worldline divergence: the module's state IS "which worldline are we on",
# and 1.048596% is the one where everything is in place.

# Match the real kernel release, not the Android OS version or spoofable uname.
# The packaged KMIs are listed in "$2/kmis" (one <android-os>-<kver> per line), so a new upstream
# KMI needs no change here.  Only the canonical GKI shape
#   <kver>.<numeric patch>[-<kmi generation>...][-<build id>...]
# is accepted: the patch level is parsed out first (a glob "*" would swallow a "-vendor" token), so
# a reordered release, a malformed one, or one with no Android tag is refused instead of guessed -
# the loader may patch vermagic, so a wrong pick would look like a successful load.
select_kmi() {
  rel="$1"
  while IFS= read -r kmi; do
    [ -n "$kmi" ] || continue
    os="${kmi%-*}"
    kver="${kmi#*-}"
    case "$rel" in
      "$kver".*) ;;
      *) continue ;;
    esac
    # $rel is guaranteed to start with "<kver>." now, so these expansions strip it.
    patch="${rel#"$kver".}"
    tag="${patch#*-}"
    patch="${patch%%-*}"
    case "$patch" in
      ''|*[!0-9]*) continue ;;
    esac
    case "$tag" in
      "$os" | "$os"-*) echo "$kmi"; return 0 ;;
    esac
  done < "$2/kmis"
  # Fallback for a custom kernel that keeps the GKI version but carries no Android tag in
  # osrelease.  Only a release with NO Android tag may take it, and only when that version maps to
  # a single packaged KMI (a KMI is ABI-stable across its patch levels, so an unambiguous version
  # cannot be a wrong pick; a version shared by two KMIs is refused rather than guessed).  The
  # Android-tag check is case-insensitive on purpose: the strict match above is case-sensitive, so
  # a tag that merely differs in case must be refused here as well.
  case "$rel" in
    *[Aa][Nn][Dd][Rr][Oo][Ii][Dd][0-9]*) return 1 ;;
  esac
  match=""
  n=0
  while IFS= read -r kmi; do
    [ -n "$kmi" ] || continue
    kver="${kmi#*-}"
    case "$rel" in
      "$kver".*) ;;
      *) continue ;;
    esac
    patch="${rel#"$kver".}"
    patch="${patch%%-*}"
    case "$patch" in
      ''|*[!0-9]*) continue ;;
    esac
    match="$kmi"
    n=$((n + 1))
  done < "$2/kmis"
  [ "$n" = 1 ] && { echo "$match"; return 0; }
  return 1
}

# Write the status into module.prop as "description=[<state>] <introduction>".  Only description is
# touched, and it goes through a temporary file + rename so the manager can never read half a line.
# The introduction is carried over from whatever the file currently says, minus an existing
# "[...] " prefix: the module's own blurb stays in module.prop (nothing here duplicates it) and a
# second call rewrites the same shape instead of nesting brackets.
set_status() {
  # A caller that did not set MODDIR must not end up writing /module.prop: `MODDIR=x . lib.sh`
  # does not leave the variable visible to functions called afterwards, so this is cheap
  # insurance against exactly that ordering mistake (every real caller sets it first).
  [ -n "$MODDIR" ] || return 0
  p="$MODDIR/module.prop"
  tmp="${p}.tmp.$$"
  intro=$(sed -n 's/^description=\[[^]]*\][[:space:]]*//p' "$p")
  # Only a description WITHOUT brackets is adopted whole.  "Stripped to empty" is a different
  # case - it means the brackets are all there was - and taking the whole line there would put the
  # old value back inside the new brackets, nesting them on every write.
  if [ -z "$intro" ] && ! grep -q '^description=\[' "$p"; then
    intro=$(sed -n 's/^description=//p' "$p")
  fi
  { grep -v '^description=' "$p"; printf 'description=[%s] %s\n' "$1" "$intro"; } > "$tmp" &&
    chmod 644 "$tmp" && mv -f "$tmp" "$p"
}

unload() {
  rmmod susfs_samsung_lkm 2>/dev/null ||
    { [ -x /data/adb/ksu/bin/busybox ] && /data/adb/ksu/bin/busybox rmmod susfs_samsung_lkm 2>/dev/null; }
}

# Decide the current state and write it.  The ladder, cheapest and most reliable signal first:
#
#   disable/remove flag            -> nothing will be loaded again        -> "Worldline Shifting..."
#   susfs_probe = absent, no log   -> installed, never activated yet      -> "Worldline Shifting..."
#   susfs_probe = absent, log kept -> a load was tried and did not take   -> "< 0%"
#   probe ok, 0 layers armed       -> in the kernel but doing nothing     -> "< 0%"
#   probe ok, some layers armed    -> "1.0485%"
#   probe ok, all layers armed     -> branch route: "1.048596%", table route: "1.130205%"
#   nothing readable               -> "1.0485%"
#
# The last line is deliberate: a status that cannot be determined must not claim Steins;Gate.
# "route=" comes from the ko's /proc/susfs_status; without that node the route cannot be told
# apart, and an all-armed kernel shows 1.048596% either way.
status_refresh() {
  if [ -e "$MODDIR/disable" ] || [ -e "$MODDIR/remove" ]; then
    set_status 'Worldline Shifting...'
    return 0
  fi

  # susfs_probe: 0 = the protocol answers (the ko is in the kernel), 10 = provably absent,
  # anything else = cannot tell, which is treated as "not in the kernel" below.
  if [ -x "$MODDIR/bin/susfs_probe" ]; then
    if ! "$MODDIR/bin/susfs_probe" >/dev/null 2>&1; then
      # load.log is created (truncated) by late-load.sh before it decides anything, so its
      # presence means "a load was attempted".  No log at all = installed and still untouched.
      if [ -e "$MODDIR/load.log" ]; then
        set_status '< 0%'
      else
        # 方括号里是"还没跳线"：世界线正在变动，还没落到任何一条上。
        set_status 'Worldline Shifting...'
      fi
      return 0
    fi
  fi

  armed=""
  total=""
  route=""
  if [ -r /proc/susfs_status ]; then
    total=$(sed -n 's/^total=\([0-9][0-9]*\).*/\1/p' /proc/susfs_status)
    armed=$(sed -n 's/^total=[0-9][0-9]* armed=\([0-9][0-9]*\).*/\1/p' /proc/susfs_status)
    route=$(sed -n 's/.*route=\([a-z]*\).*/\1/p' /proc/susfs_status)
  elif [ -r /proc/susfs_path ] && [ -r /proc/susfs_kstat ]; then
    # Older ko without the status node: the LSM layer's nodes only exist when sus_path armed,
    # which is the best signal left.  The route stays unknown.
    armed=full
    total=full
  fi

  if [ -z "$total" ] || [ -z "$armed" ]; then
    set_status '1.0485%'
    return 0
  fi

  case "$armed:$total" in
    "")              set_status '1.0485%' ;;     # 读不出来 → 按"部分"，不猜
    "0:"*)           set_status '< 0%' ;;        # 一层都没起来 → 等于没装
    "$total:$total")
      if [ "$route" = table ]; then
        set_status '1.130205%'                   # 全部生效但走了表项 → α 吸引域
      else
        set_status '1.048596%'                   # 全部生效且走 B 分支 → Steins;Gate
      fi ;;
    *)               set_status '1.0485%' ;;     # 部分生效 → 差几位数
  esac
}
