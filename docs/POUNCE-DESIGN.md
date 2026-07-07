# POUNCE — fused walk+getattr path resolution (the metadata round-trip fix)

Status: DESIGN (scripture; user-voted 2026-07-07 — "Both (ext + cache)" with
phased sequencing: Phase 1 = the wire extension + the stalk fast path +
`SYS_STAT`; Phase 2 = an attr cache ONLY if the post-Phase-1 residual
measurement justifies it). Implementation follows this commit in sub-chunks
P-1..P-5 (§9); the focused Fable audit closes the arc.

Naming: **pounce** is the stalk's terminal act — `stalk()` tracks the quarry
one component at a time; the pounce takes the whole run in a single bound.
The kernel-side batching fast path is the pounce; the wire op keeps the
descriptive 9P-convention name `Twalkgetattr`/`Rwalkgetattr` (the Tsync /
Treflink precedent — wire ops name what they do; `Tweft` is thematic only
because it names the Weft subsystem's op).

---

## 1. The problem, measured

The 2026-07-07 measurement session (tasks #59/#60/#63; the mission register)
established that the on-device `go build`'s dominant cost is the **metadata
round-trip stream**, not tool paging (the Image cache HITs on every re-exec —
the telemetry boot refuted the earlier re-page-in theory), not the CF-2 pool
(84% of ops run at in-flight depth 1), and not compile CPU:

- Op types through 98,304 pool admissions: **Tgetattr 29,330 / Twalk 23,300 /
  Tclunk 22,603 / Tread 17,968 / Twrite 614** — ~95% metadata-read.
- `build2-warm` = **4192 ms with every tool image cached and the go cache
  fully populated** — approximately pure metadata RTs.
- ~75k RTs per cold gofmt build at ~0.1-0.15 ms each ≈ 8-11 s of the 26.9 s.

The per-op anatomy is the multiplier. One `os.Stat("/goroot/src/fmt/print.go")`
today (4 components under the dev9p root):

| step | RPC | count |
|---|---|---|
| X-search on each parent (`stalk.c:227` — dev9p is `perm_enforced` since A-3, so every hop calls `spoor_stat_native(parent)`) | Tgetattr | 4 |
| per-component walk (`stalk.c:247` drives `dev->walk(parent, nc, names, 1)` — ONE name per RPC) | Twalk | 4 |
| intermediate trail unwind | Tclunk | 3 |
| `SYS_FSTAT` on the O_PATH handle | Tgetattr | 1 |
| handle close | Tclunk | 1 |
| **total** | | **13** |

The 1.26:1 Tgetattr:Twalk ratio in the histogram is this anatomy (one X-search
getattr per walk hop, plus fd-based fstats). The wire op below collapses the
whole resolution to **1 RPC** for a stat and **2** for an open.

## 2. Prior art (the fork research)

- **Plan 9**: the original 9P `Tstat` was a single round trip. Per-file
  permission checks are done SERVER-side at walk/open time using the attach
  identity, so Plan 9 never pays a client-side per-component getattr. Its
  mount driver (devmnt) walks per-lookup with no dcache — Plan 9 accepts walk
  RTs; its servers are usually local.
- **9P2000.L** split stat into walk+getattr+clunk — the 3-RT stat is a known
  .L cost.
- **Linux v9fs** answers with the dentry cache + per-dentry fid cache
  (repeated paths skip the walk) and `cache=loose` attr caching (staleness
  window accepted). The first lookup of a deep path still walks per-component.
- **NFSv4** answers with COMPOUND: LOOKUP+GETATTR fused in one round trip —
  the same fusion this design adopts.
- **Microsoft 9P2000.W** (the WSL2 dialect) added a fused walk+getattr op for
  exactly this cost on exactly this protocol family — the strongest precedent
  that the fusion is the correct structural fix.
- **Thylacine/Stratum extension idiom**: Tsync 128 / Treflink 130 /
  Tfallocate 132 / Tweft 134 / Tweftio 136 — both wire ends are ours; adding
  an op is the established, audited pattern.

Why not the alternatives (voted down 2026-07-07):
- **Attr cache only** (v9fs `cache=loose` analog): cheaper, but it caches a
  PRIVILEGE input — a `chmod -x` through another session (the per-user proxy)
  is invisible until TTL expiry, a stale X grant on a security check; and it
  cuts only the getattr third (13 -> 9 RPCs). It survives as the
  data-gated Phase 2 for the residual fd-fstat traffic, with its own design
  round if the numbers call for it.
- **Fid/dentry cache** (the full v9fs model): the biggest structural change
  (rename/unlink invalidation, fid budget, a new cache subsystem on the I-28
  surface); v1.x-sized. The pounce makes deep paths ~1 RPC regardless, which
  removes most of its value at v1.0.

## 3. The wire op: `Twalkgetattr` (140) / `Rwalkgetattr` (141)

Numbering note (P-1 discovery): the Stratum extension enum does NOT end at
Tfallocate 132/133 as the Weft-era numbering believed — Stratum also assigns
Tfadvise 134/135 + Tpin 136/137 + Tunpin 138/139 (the last two reserved),
so Thylacine's kernel<->netd Tweft/Tweftio (134-137) already collide with
those LATENTLY (disjoint server domains — no wire confusion is reachable
today), and 138/139 was taken. 140/141 is free in BOTH registries; the
cross-project registry reconciliation is tracked as #371.

```
Twalkgetattr  tag[2] fid[4] newfid[4] request_mask[8] nwname[2] nwname*(wname[s])
Rwalkgetattr  tag[2] nwqid[2] nwqid*(getattr_body)

getattr_body = valid[8] qid[13] mode[4] uid[4] gid[4] nlink[8] rdev[8]
               size[8] blksize[8] blocks[8]
               atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8]
               ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8]
               gen[8] data_version[8]
```

Semantics:

- **Walk rules are byte-identical to Twalk** (same fid gates, same
  `STM_9P_MAX_WALK`/`P9_MAX_WALK` = 16 bound, same partial-walk rule: a
  missing/non-dir component stops the walk, `nwqid < nwname`, and `newfid` is
  NOT bound). One deviation, explicit and additive:
- **`newfid == P9_NOFID` (0xffffffff) is permitted** and means *walk-query*:
  the server walks and samples but binds NOTHING — there is nothing to clunk.
  (Classic Twalk rejects NOFID; this op defines it.) This is what makes a
  pure stat 1 RPC.
- **One `getattr_body` per walked component**, in walk order — the exact
  Rgetattr body layout, every element regular (no compact intermediate form;
  irregular wire formats breed parser bugs). `request_mask` applies to every
  component (the kernel requests the union of what the X-search needs —
  MODE|UID|GID — and what the leaf consumer wants; the server has each
  step's inode in hand, so the marginal cost is packing).
- **Attr sampling is per-step, after the step** — the same coherence as
  today's sequential `Twalk; Tgetattr` pair (no cross-component atomicity is
  claimed or needed; 9P has none today).
- Reply size: 153 B/component x 16 max ≈ 2.5 KiB — fits every msize in use.
- Errors: exactly Twalk's (EBADF / EINVAL on a non-NODE or open fid / EPROTO).

Server side (`stratum src/9p/server.c`): `h_walkgetattr` reuses `h_walk`'s
machinery verbatim — the same AUX_XATTR/open-fid gates, the same
0-bindings 3-phase fast path (pin, unlocked `verify_fresh_snapshot` +
`walk_components`, bind-or-error under `s->lock`) — plus a per-component
attr fetch (the same lookup `h_getattr` performs, driven from each walked
step's (ds, ino)); with NOFID it skips the `walk_finish_locked` fid bind.
It runs under the CF-2 dispatch pool like any op.

## 4. The kernel client + the Dev seam

- `p9_client_walkgetattr(...)` — the wire driver, `p9_client_walk`'s shape
  plus the attr array out-param and the NOFID mode. No new tag/fid semantics:
  one tag, at most one newfid installed (never installed for NOFID/partial) —
  `specs/9p_client.tla`'s tag/fid model is unchanged; the 4 buggy cfgs re-run
  as the pre-commit gate at impl (no new spec module per the 2026-05-23
  broadening; prose validation per this doc + the audit).
- A new OPTIONAL Dev vtable slot:

```c
// Walk nname components from src in ONE operation, filling sts[0..nqid)
// with each walked component's attributes (the walk-fused getattr). The
// Walkqid contract is Dev.walk's (reuse-nc; nqid short on partial walk).
// sts_query: when nc == NULL the walk is a QUERY (no Spoor transitioned,
// nothing to clunk — dev9p maps this to newfid=NOFID).
struct Walkqid *(*walk_attrs)(struct Spoor *src, struct Spoor *nc,
                              const char **names, int nname,
                              struct t_stat *sts);
```

- `dev9p` implements it via the wire op. `devramfs` implements it natively
  (attrs are in RAM — free), so stalk keeps ONE fast-path shape across both
  perm-enforced Devs. Every other Dev leaves the slot NULL and keeps
  today's per-component loop — correctness is identical, only the RPC count
  differs.

## 5. The stalk pounce (the batching fast path)

`stalk()` currently: per component — cross-on-descent, X-search the parent
(one `stat_native` RPC on a perm-enforced Dev), `spoor_clone` + 1-name
`dev->walk` (one RPC), push the trail. The pounce replaces the inner loop
when `parent->dev->walk_attrs != NULL`:

1. **Gather the run**: the maximal sequence of consecutive REAL components
   (a `.` / `..` boundary ends the run — those are resolver-side, as today;
   `STALK_MAX_DEPTH` and `SYS_WALK_OPEN_NAME_MAX` bounds unchanged), capped
   at `P9_MAX_WALK`.
2. **One `walk_attrs` call** for the run (clone+walk fused, exactly one wire
   RPC on dev9p).
3. **Post-scan LEFT-TO-RIGHT — the fail-ordering invariant (§6)**: for each
   returned component k in order: (a) X-check the PARENT of k — the base's
   attrs from the pre-pounce `stat_native` (one RPC per run, or carried from
   the previous run's leaf attrs), then each k's own attrs vouch for k+1;
   (b) check component k against the territory mount table
   (`(dc, devno, qid)` — the table is small and this scan is RAM-only).
4. **Mount point mid-run**: the batch walked PAST it server-side; take the
   resolution only UP TO the mount point — clunk the batched fid, re-walk
   the prefix to the mount point (one extra RPC), cross, and resume pouncing
   inside the mounted tree. Mid-path crossings are rare (build paths cross
   at most `/home/<user>` early) and mount points are stable, so the retry
   cost is negligible; correctness = today's cross-on-descent exactly.
5. **The trail**: a pounced run pushes ONE owned Spoor (the run's leaf) —
   intermediates never materialize as fids, which is where the Tclunk
   savings come from. `..` popping degrades gracefully: a pop at a run
   boundary pops the run's leaf; a path that interleaves `..` densely simply
   gets shorter runs (worst case = today's per-component behavior).

## 6. Security: the fail-ordering invariant (I-28 preserved)

The per-component X-search semantics are preserved BYTE-IDENTICALLY in
outcome; only the transport changes:

- **Same checks, same order, fresh attrs**: every component's X bits are
  still checked, in path order, against attrs sampled by THIS resolution
  (no cache, no TTL — the attrs arrive fused with the walk). Phase 1
  introduces ZERO attr staleness.
- **The fail-ordering invariant**: the post-scan consumes results strictly
  left-to-right, and an X-denial at component k MASKS everything past k —
  including a deeper walk-miss. A partial walk whose miss lies at-or-past an
  X-denied component MUST report `T_E_ACCES`, never `T_E_NOENT`, so a caller
  cannot probe existence under a forbidden directory. (Today's sequential
  code gets this for free by checking before walking; the pounce walks
  first server-side and must therefore enforce the masking in the post-scan.
  This is the audit's primary prosecution target.)
- **Check-after-walk is sound**: the server walking past a component the
  caller lacks X on has no observable effect — a 9P walk has no side
  effects beyond fid creation, the batched fid is clunked on denial, no
  attrs or data are surfaced to EL0, and the errno is the same ACCES.
  TOCTOU shape is unchanged (both orders are unsynchronized snapshots; 9P
  has no cross-op locking today).
- `SYS_STAT` (§7) carries the identical X-search; reading the LEAF's attrs
  requires only the path X-search (POSIX stat semantics — today's
  O_PATH+fstat emulation already grants exactly this: O_PATH skips the R/W
  check, SYS_FSTAT is kind-gated only, #46).

No new ARCH §28 invariant: the pounce REALIZES I-28's existing obligation at
lower cost; the fail-ordering property is a named audit obligation in the
§25.4 row, not a new invariant number.

## 7. `SYS_STAT` (= 88): path-stat in one syscall

Today a path stat is emulated (Go port, libthyla-rs, pouch): O_PATH
walk-open + SYS_FSTAT + close — 3 syscalls, 13 RPCs. `SYS_STAT` collapses
it:

- ABI: `SYS_STAT(path_va, path_len, stat_out_va)` -> 0 / -errno; fills the
  80-byte `struct t_stat` (the A-2a layout, unchanged).
- Kernel: a new `STALK_STAT` amode — resolve like `STALK_WALK` but the
  FINAL run uses the QUERY walk (`walk_attrs` with `nc == NULL` -> dev9p
  `newfid = P9_NOFID`): the leaf's attrs return in the reply and NO quarry
  Spoor/fid ever exists — nothing to clunk. On a Dev without `walk_attrs`,
  fall back to walk + `stat_native` + clunk (correct everywhere).
- Result: `os.Stat` = **1 RPC** on the pool (vs 13). Symlinks do not exist
  at v1.0 (G11), so stat == lstat; the Go port routes both `os.Stat` and
  `os.Lstat` here, libthyla-rs `fs::metadata` likewise, pouch `stat`/
  `lstat`/`fstatat(path)` via the 0001 seam.
- The amode joins `stalk_err`'s LOUD amode gate (`stalk-1 audit F1`: any new
  amode gets its own final-hop arm; a missed arm fails closed).

## 8. What Phase 1 does NOT do (the Phase-2 gate)

- **No attr caching across syscalls.** `SYS_FSTAT` on an open fd remains a
  live Tgetattr (a program holding an fd expects fresh size/mtime). The
  walk-fused attrs are consumed within the one resolution and dropped.
- After P-4 lands, re-measure (the #368 method: one instrumented-pool boot +
  the bench pair). IF the residual fd-fstat Tgetattr traffic still dominates
  the metadata time, Phase 2 designs the bounded attr cache (TTL +
  qid.vers validation + the cross-session staleness story) in its own
  design round. The stop-if-done criterion: metadata RTs no longer the
  dominant term of the cold build (compile CPU should dominate a build).

## 9. Implementation plan (sub-chunks; each lands green + tested)

| # | scope | proof |
|---|---|---|
| P-1 | Stratum: `h_walkgetattr` (140/141) + NOFID + per-component attrs; host tests (walk parity vs Twalk+Tgetattr; NOFID binds nothing; partial-walk attr prefix; pool-path) | stratum suite + host 9P test |
| P-2 | Kernel wire + client: codec (`p9_build_twalkgetattr`/`p9_parse_rwalkgetattr`) + `p9_client_walkgetattr`; loopback tests | kernel tests; `9p_client.tla` buggy cfgs re-run |
| P-3 | `Dev.walk_attrs` slot + dev9p impl + devramfs impl; `stalk` pounce (run gather, fail-ordering post-scan, mount-split) + `STALK_STAT` + `SYS_STAT` | kernel tests: pounce parity vs per-component loop; ACCES-masks-NOENT; mount-mid-run split; `..` runs; boot OK |
| P-4 | Consumers: Go port `os.Stat`/`os.Lstat` -> SYS_STAT; libthyla-rs `fs::metadata`; pouch stat family; re-measure (bench pair + instrumented-pool op recount) | bench + RT histogram delta |
| P-5 | Focused Fable-5-max audit (the §25.4 row) + SMP gate (default+UBSan x smp4/smp8, N>=10) + docs (reference doc + Stratum 20-9p.md) | audit close + gate |

Audit prosecution focus (the §25.4 row carries the authoritative copy): the
fail-ordering invariant (ACCES masking, no existence probe under a denied
dir); walk-parity (pounce result == per-component loop for every path
shape: `.`/`..` runs, mount-mid-run, partial walk, over-long component,
depth cap); fid lifecycle (the batched fid on every early-exit path — deny,
mount-split, OOM — exactly one clunk; NOFID installs nothing on either
end); the NOFID server arm (no fid leak on partial walk); Rwalkgetattr
parser bounds (nwqid vs body length, per-element truncation); `SYS_STAT`
uaccess (path in, 80-byte t_stat out, the #3 errno clamp).

## 10. Expected effect (honest ranges)

- os.Stat 13 -> 1 RPC; open+read+close ~10 -> ~4-5.
- The ~75k-RT stream: path-resolution ops (~22k, matching the clunk count)
  drop from ~6-13 RPCs each to 1-3 -> total stream roughly halves or
  better; warm build2 4.2 s -> ~2 s expected; cold 26.9 s -> ~20-23 s
  (compile CPU + tool first-page-in + writes remain). The #367 Stratum
  create-path fix (chunk c) and the residual fstat traffic (Phase 2, if
  taken) own the rest.
