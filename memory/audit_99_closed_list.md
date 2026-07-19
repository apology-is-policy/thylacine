# Audit closed list — #99 (SYS_WALK_CREATE / #102 errno-loss fix)

Do-not-re-report preamble for any future round on this surface.

**Scope**: `kernel/dev9p.c` (dev9p_create errno record + invalidate-on-EEXIST +
the accessor), `kernel/include/thylacine/dev9p.h` (create_errno field + decl),
`kernel/syscall.c` (sys_walk_create_handler create-NULL return; stale comment),
`kernel/test/test_dev9p.c` + `kernel/test/test.c` (the regression),
`usr/go-fs/main.go` (6b race + 6c O_EXCL probes), `go-thylacine
src/os/file_thylacine.go` (retry-Open).

**Reviewers**: Fable-5-max holotype (`MODEL(start)==MODEL(end)==claude-fable-5`)
+ concurrent self-audit + the SMP gate (the F1 catch). CONVERGED: the SMP gate
(2/10 boots), the holotype (F1 [P1]), and the self-audit all independently found
the SAME gap AND prescribed the SAME fix.

## Round 1: 0 P0 / 1 P1 / 0 P2 / 4 P3 — NOT dirty; all fixed/documented

- **F1 [P1] — FIXED (SMP-gate + holotype, converged).** Propagating `-EEXIST`
  is not enough under a race: a loser's `Open`->ENOENT installs a NEGATIVE
  `(parent,name)` dentry, and its retry-`Open` serves that stale negative
  RPC-free -> ENOENT -> the open-or-create spuriously fails (go-fs 6b failed
  2/10 boots; boot-fatal). The *success* path already dropped the dentry
  (`larder_dentry_invalidate_name`, dev9p.c:1361) but the **EEXIST arm returned
  NULL before reaching it**. FIX: both `dev9p_create` failure arms, on
  `rc == -T_E_EXIST`, call `larder_dentry_invalidate_name(&p->client->larder,
  parent_path, name, name_len)` before `return NULL`. EEXIST *proves* the file
  exists, so the negative is stale; the invalidate ALSO bumps the Larder gen so
  a concurrent negative-install that snapshotted the old gen is skipped (the
  re-cache race). The loser's own EEXIST-invalidate runs right before its
  retry-Open -> it sees the file. **Verified 10/10 under the 8-way go-fs 6b
  race.** Regression: `dev9p.create_errno_propagates_eexist` now also asserts
  the `(parent,dup)` dentry is dropped (non-vacuous: reverting the invalidate
  leaves the pre-seeded positive entry standing).

- **F2 [P3] — FIXED.** The mkdir failure arm recorded the errno but did not latch
  `fid_suspect` (asymmetric with the lcreate arm; the G2 backstop contract). A
  mkdir-only workload could re-park a stale fid. FIX: `p->fid_suspect = true` in
  the mkdir `rc != 0` arm.

- **F3 [P3] — FIXED.** The dir-create mid-sequence failures (fid-pool exhausted /
  walk-to-child / lopen after a successful `Tmkdir`) collapsed to the generic
  `-1`. FIX: record the real errno (`-T_E_NOMEM` for the fid-pool arm; `rc`/`-T_E_IO`
  for the walk arm; `rc` for the lopen arm) + latch `fid_suspect` where a by-name
  op erred. A dir that WAS created but could not be opened no longer reports an
  opaque `-1`.

- **F4 [P3] — FIXED.** No deterministic test pinned the handler's `return
  dev9p_create_errno(nc)` (the kernel test pins the accessor; go-fs 6b is
  schedule-dependent). FIX: `go-fs` step 6c -- a single-threaded O_CREATE|O_EXCL
  create of an existing file must report `EEXIST` via `os.IsExist` (reverting the
  record OR the handler return surfaces EPERM -> IsExist false -> fails).

- **F5 [P3] — DOCUMENTED.** The Go retry is one-shot; a create/unlink storm racing
  this path could surface ENOENT where POSIX loops. Vanishing once F1 is fixed
  kernel-side (10/10). Documented in `file_thylacine.go` (a bounded Open/Create
  loop is the fuller shape only for a workload v1.0 does not exercise).

## Verified sound (do not re-litigate)

- **Clamp exactness** `(e <= -2 && e >= -4095) ? e : -1`: every boundary checked
  (0/-1 -> -1; -2/-4095 pass; -4096/positive/garbage -> -1). Double-guarded by
  the client's `map_error` bounding the Rlerror ecode to [1,4095] (I-14).
- **Sign/width**: int -17 sign-extends through `(s64)` to x0; go asm decode ->
  Errno(17)=EEXIST (`_Static_assert(T_E_EXIST==17)`); `IsExist` fires; pouch
  [-4095,-2] passthrough decodes identically.
- **Lifetime / no-sharing**: `nc` is handler-local (spoor_clone publishes
  nothing; the clone-walk gives a fresh `priv_alloc(KP_ZERO)`, create_errno born
  0); the accessor read precedes `spoor_clunk(nc)`; the handler is the sole
  `->create` caller (grep-verified) -> no torn read, no peer reach, no lock.
- **Non-dev9p Devs byte-identical**: `priv_of`'s dc+magic gate -> the accessor
  returns -1 for devramfs + every create-stub Dev (prior blanket -1 preserved).
- **Fid lifecycle**: both failure arms leave `p->fid` at the parent clone; only
  the record + invalidate are added -> no new clunk/leak/double-clunk.
- **Go O_EXCL path untouched + improved** (a separate branch; an excl create of
  an existing file now surfaces real EEXIST); O_TRUNC/O_APPEND through the retry
  correct; the retry is inert pre-kernel-fix (`IsExist(EPERM)` is false).

**Gates**: default suite `dev9p.create_errno_propagates_eexist` PASS + boot OK +
go8d OK + `go-fs` 6b/6c PASS + the SMP gate (10/10 default-smp4 on the F1 fix).
