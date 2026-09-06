---
id: chg-2026-09-06-fs-permission-absorb
type: chg
title: "absorb docs/reference/99-fs-permission (A-2 rwx + ownership): fold the SYS_WSTAT handler + devramfs enforcement, multi-redirect stub across 8 surfaces"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: [sub-kernel-syscall-dispatch, sub-kernel-content]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The A-2/A-2a/A-2d file-permission + ownership reference. Rich + cross-layer, so
verified atom-by-atom against SIX owning dossiers (`quaestor owner`), not stubbed
on a bare "covered". Result: most atoms already carried, two genuine folds, one
security atom that turned out UNDOCUMENTED, and three stale claims in the doc to
name.

WHERE EACH ATOM LIVES (verified, not assumed):
- `t_stat` ABI (uid@72/gid@76) -> abi-t-stat -- AHEAD of the doc: the struct is
  88 bytes now (#100 appended devno@80), the doc froze at 80; 7 mirrors vs 3.
- perm.c enforcement (perm_check owner-first, want_for_omode/rights_for_omode,
  perm_wstat_check, proc_in_group, want==0 fail-closed, I-22) -> sub-kernel-perm
  (fresh from last run's F1 fold) -- AHEAD: adds CAP_DAC_OVERRIDE, CAP_CHOWN, the
  T_WSTAT_SIZE content-not-metadata arm the doc omits.
- walk-open access gate (X-search per component + R/W-per-omode) -> sub-kernel-
  stalk L100-106/L416 -- more accurate than the doc, which attributed it to the
  handler; it lives in the resolver.
- #81 O_PATH read-bypass close (CWALKONLY reject on read/write/readdir; once
  leaked 0400 /system.key) -> sub-kernel-stalk L657 + sub-kernel-spoor (the flag).
- dev9p_stat_native/wstat_native + the T_WSTAT_* == P9_SETATTR_* asserts + the
  Rgetattr valid-mask fail-closed -> sub-kernel-ninep-dev9p -- AHEAD: dev9p
  perm_enforced is now TRUE (A-3b landed + flipped it; the doc says false/deferred).
- #47 "fchmod on O_RDONLY is correct" semantic -> sub-pouch-fs L133 + sub-kernel-
  dev L93 (already carried).

FOLDS (two genuine gaps):
1. sub-kernel-syscall-dispatch: the complete SYS_WSTAT handler (sys_wstat_for_proc)
   as the third FS identity gate, beside last run's F2 rename/unlink gate. The doc
   is A-2a-era (MODE/UID/GID only); the CODE has grown a T_WSTAT_SIZE/ftruncate
   axis, and its #81-class truncate-via-O_PATH close -- an O_PATH (CWALKONLY)
   handle's RIGHT_WRITE is HOLLOW (perm_check-exempt at open), so a truncate
   through it is rejected, else it mutates a file the caller has no W on -- was
   UNDOCUMENTED anywhere in the vault (a security atom surfaced by verify-before-
   fold). Folded code-accurately: validation, the kind-gate-not-rights-gate
   metadata authority (#47/#46), the SIZE content/metadata split, the truncate
   close, and the perm_wstat_check placement (the check is perm's; the placement
   is the handler's).
2. sub-kernel-content: devramfs's perm_enforced=true + the PRINCIPAL_SYSTEM/
   GID_SYSTEM stat_native stamp made explicit -- the boot FS is the one enforced
   backing, and boot survives enforcement because the un-elevated PRINCIPAL_SYSTEM
   traverser owns everything it touches (I-22). The dossier carried "system-owned"
   as a property; this adds the flag + the stamp + the doesn't-brick-boot reason.

WHAT THE DOC GOT WRONG (named in the stub): (a) its HEADER says A-2d enforcement
"is not yet built" while its own Status/body says "A-2d: LANDED (devramfs-live)"
-- a stale intro over a current body; (b) t_stat "80 bytes" -- it is 88 (devno);
(c) "dev9p perm_enforced = false, deferred to A-3" -- A-3b landed and flipped it
true; (d) no T_WSTAT_SIZE axis at all (code grew past the A-2a snapshot).

No inv-note guarded-by added (the I-22 enforcer is perm.c, not these two files;
prose [[inv-i22]] refs only). Render clean; lint 0-fail. view-absorption 71 -> 72.
