---
id: chg-2026-09-06-9p-identity-absorb
type: chg
title: "absorb docs/reference/100 (9P identity presentation, A-3): the cross-cutting security surface folded across 7 dossiers + a new n_uname trust-stamp seam, then multi-redirect stub -- 66 absorbed / 91 live"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-pouch-net
  - sub-kernel-ninep-client
  - sub-kernel-perm
  - sub-kernel-syscall-dispatch
  - sub-stratum-server
  - sub-substrate-build
  - sub-kernel-ninep-attach
established: []
closed: []
opened:
  - seam-nuname-trust-stamp
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The A-3 identity-presentation surface -- the security-critical reconciliation of
a 9P server's connecting identity with Thylacine's durable per-Proc
`principal_id`, which lets kernel rwx enforcement (A-2d) activate against dev9p
without bricking boot. Cross-cutting; effort max; every atom verified against the
code before folding. Seven folds + one new seam + a multi-redirect stub.

**M1 -> sub-pouch-net.** The `getsockopt(SO_PEERCRED)` shim marshals the
kernel-stamped `principal_id -> ucred.uid` + `primary_gid -> ucred.gid` (0006
patch L976-977; was a `0/0` stub). The unforgeable local identity channel (the
kernel fills the principal at `SYS_srv_peer`, so a connecting Proc cannot forge
it). Also noted the stale contradicting top-of-file comment (L850-859 still says
"uid 0 at v1.0") above the live marshal. The Contract had recorded only the
`SYS_SRV_PEER` mapping, not the principal content.

**A-3c F1 -> sub-kernel-ninep-client (a REWRITE, HIGH).** The dossier ACTIVELY
CONTRADICTED the code: Error-convention/Error-paths/Caveats all said the Rlerror
ecode passes "verbatim / u32-unbounded here / bounded at dev9p, not here."
Ground truth (`9p_client.c:116`): `map_error` bounds the wire ecode to `[1,4095]`
(`ecode == 0 || ecode > 4095 -> -EIO`) BEFORE negating, closing the signed-
overflow UB of `-(int)0x80000000` (a kernel halt reachable by any hostile Rlerror
on any op; traps under UBSan) and folding `Rlerror(ecode=0)`-as-success into
`-EIO`. Rewrote all three lines to match the code AND the sibling
sub-kernel-ninep-wire (which already asserted map_error bounds it -- the two now
agree instead of contradicting).

**F1 -> sub-kernel-perm.** Enumerated the `rights_for_omode` handle-rights table
(OREAD->R / OWRITE->W / ORDWR->RW / OEXEC->R read-implied / +OTRUNC->+W) and the
caller-policy disclaim: `RIGHT_TRANSFER` and the `T_OPATH` born-`R|W` base are
NOT omode-derived (`perm.c:72-73`) -- they live at the syscall walk-open site.
The omode matched-pair + OEXEC execute-to-read leak were already present.

**M4 + F2 + A-3c attach_err_to_ret + T_OPATH -> sub-kernel-syscall-dispatch.**
The A-3 syscall-path atoms, all in syscall.c (this dossier's code): the
FS-mutation identity gate (rename BOTH parent dirs + unlink parent run
`perm_check(PERM_W|PERM_X)` gated on `perm_enforced`, 4431-4438/4535-4540), the
walk-open handle-rights caller policy (`rights_for_omode | RIGHT_TRANSFER`;
T_OPATH born `R|W` no-transfer, 2901/3520), M4 (`n_uname = principal_id`, 2276),
and A-3c's `attach_err_to_ret` (2170, refining attach return `{-1,fd}->{-errno,fd}`,
surfacing the Tattach `-EACCES`) folded into the existing "two error conventions"
section (the same window-clamp pattern it already described).

**M2 -> sub-stratum-server + sub-substrate-build.** The host-bake owner override
`--bake-owner-uid`/`-gid` (Stratum run.c:264-268/386-406, `bake_owner_enabled`
default-false memset-0, `(uid_t)-1` per-axis sentinel) overriding `auth_uid`/gid
at the create chokepoint; and the bake-VALUE pass (`build.sh` bake_owner=4294967294
PRINCIPAL_SYSTEM via `stratum-mkfs --root-uid` for the pool ROOT + `stratumd
--bake-owner-uid` for every baked file -- the no-brick property). The
n_uname-ignored/SO_PEERCRED-live story + `stratum_host_tools_stale` were already
present.

**M5 -> new seam-nuname-trust-stamp.** The n_uname identity-forward trust-stamp
gate was recorded in doc 100 as a v1.x seam but was UN-HOMED in the vault graph:
sub-kernel-ninep-attach's L251 "swept there" pointer named no node, and neither
seam-845 (tag-generations) nor seam-stratum-notify-peercred (the notify socket)
is it. Authored the seam (surface: syscall-dispatch + ninep-attach) and fixed the
dangling attach pointer.

**Verified PRESENT, no fold:** ninep-attach M6 (`out_err` / `-T_E_ACCES`
dataset-scope), ninep-dev9p (`perm_enforced = true` flip), stratum-session
(`--datasets-allowed` -> `Rlerror(EACCES)`).

**Stub:** docs/reference/100 -> multi-redirect across all the above + inv-i22.
"What it got wrong": the ninep-client contradiction (fixed), the un-homed M5 seam
(fixed), the foreign inode.h/stratum-mkfs citations (orphan, not blocking).

No code touched; no audit owed (all dossiers already audit:hard where relevant --
this is doc absorption, not a code change). view-absorption: 65 -> 66 absorbed,
91 live.
