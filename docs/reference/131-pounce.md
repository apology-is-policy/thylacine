# 131 — POUNCE: fused walk+getattr resolution

**Status**: COMPLETE (P-1 stratum `a14a2cf`; P-2 thyla `a4a4d97`; P-3 thyla
`d9fdff5`; P-4 thyla `b2d21c68` + go `f569e39` + the in-guest probe
`d7e543f9`; P-5 focused Fable-5-max audit CLOSED CLEAN **0 P0 / 0 P1 /
0 P2 / 1 P3** [F1 tracked, #372] + concurrent self-audit [24 checks, no
findings] + the SMP gate PASS 40/40 [default+UBSan × smp4/smp8 N=10, 0
corruption] — `memory/audit_pounce_closed_list.md`). Binding design:
`docs/POUNCE-DESIGN.md` (§4 carries the P-3 as-built). The resolver-side
mechanics are ALSO documented in `docs/reference/104-stalk.md` ("The
POUNCE — component batching with kernel-side checks"); this doc is the
arc-level reference: the wire op, the `Dev.walk_attrs` vtable contract,
the two implementations, the capability latch, `SYS_STAT`, the consumers,
and the measurements. The authoritative audit-prosecution list is the
ARCH §25.4 POUNCE row.

## Purpose

Path resolution was the on-device `go build`'s dominant cost: ~75k
metadata round trips per build (Tgetattr 29.3k / Twalk 23.3k / Tclunk
22.6k — the #368 recount), because every component of every path paid a
separate Twalk and every X-search/stat paid a separate Tgetattr, each a
~50-150 µs 9P round trip to stratumd. POUNCE collapses a run of
components into ONE fused RPC that returns the walked qids AND a full
getattr record per component — so one `os.Stat` goes ~13 RPCs → 1, with
the per-component X-search checks preserved bit-for-bit kernel-side.

Naming: the thylacine stalks (per-component, patient), then pounces (one
fused strike down the path).

## The wire op — Twalkgetattr 140 / Rwalkgetattr 141

A Stratum 9P extension (`kernel/include/thylacine/9p_wire.h:185`;
stratum-side `src/9p/server.c::h_walkgetattr`, landed P-1):

- `Twalkgetattr(tag, fid, newfid, request_mask, nwname, wname[nwname])` —
  Twalk's shape plus a getattr `request_mask`. `newfid` may be
  `P9_NOFID`: the QUERY form — walk + report attrs, bind nothing (the
  no-fid stat).
- `Rwalkgetattr(nwqid, {valid, qid, attrs}[nwqid])` — one element per
  walked component; each element is one Rgetattr body (the shared element
  parser: `9p_wire.c:668`).
- **Session rule** (mirrors Twalk): `newfid` binds ONLY on a FULL walk
  (`nwqid == nwname`); a partial walk binds nothing. `P9_NOFID` never
  binds. Builder/parser: `p9_build_twalkgetattr` /
  `p9_parse_rwalkgetattr` (`9p_wire.h:573/584` — the parser validates
  strictly: nwqid bounded, per-element length checks).
- Client op: `p9_client_walkgetattr` (`9p_client.c:1003`) — a standard
  #841 pipelined op, nothing special in the engine.

Registry note: 140/141 sit past the Stratum extension range in use.
The Tweft/Tweftio collision with Stratum's Tfadvise/Tpin (#371) was
RESOLVED 2026-07-07: the Weft quartet moved 134-137 -> 142-145 and the
cross-project registry now lives in `docs/9P-EXTENSIONS.md`.

## `Dev.walk_attrs` — the optional vtable slot

`kernel/include/thylacine/dev.h:147-173`:

```c
struct Walkqid *(*walk_attrs)(struct Spoor *c, struct Spoor *nc,
                              const char **names, const size_t *name_lens,
                              int nname, struct t_stat *sts);
```

- NULL-permitted (a Dev without it resolves per-component, unchanged).
- Walks up to `DEV_WALK_ATTRS_MAX` (16, `_Static_assert`-pinned ==
  `P9_MAX_WALK` in dev9p.c) REAL components — the caller (stalk) never
  passes `.` or `..`.
- Names are `(ptr, len)` pairs — a deliberate deviation from
  `Dev.walk`'s NUL-terminated names, so stalk's path buffer is sliced
  zero-copy.
- Fills `sts[0..nqid)` — one `struct t_stat` per walked component,
  server-fresh (fetched IN the same server round as the walk: zero
  staleness, unlike an attr cache).
- **The strict shape contract** (the P-3 self-audit SA-11 fix): a FULL
  walk with non-NULL `nc` must transition `nc` and return
  `w->spoor == nc` (BIND form); a PARTIAL walk leaves `nc` UNTOUCHED
  with `w->spoor == NULL`; the QUERY form (`nc == NULL`) binds nothing,
  `w->spoor == NULL` always. stalk validates the shape
  (`shape_ok`) and fails closed on a violation — pushing an
  untransitioned `nc` would later clunk the parent's SHARED fid.
- May return `DEV_WALK_ATTRS_UNSUPPORTED` (see the latch below) — a
  distinguished STATIC sentinel object (`dev.h:50-59`, defined in
  `dev.c`) that must NEVER reach `walkqid_free` (it is not heap).

## dev9p implementation + the per-session capability latch

`kernel/dev9p.c:346` (`dev9p_walk_attrs`); vtable at `dev9p.c:886`.

- Heap-allocates the `p9_attr[nname]` scratch (`kmalloc`; ~2.5 KB at 16
  components — kernel stacks are 16 KB, so this stays off-stack), builds
  the name slices, issues `p9_client_walkgetattr`.
- BIND form allocates a fid for `nc`; QUERY form sends `P9_NOFID`.
- Attr mapping via `t_stat_from_p9_attr` (`dev9p.c:293`) — factored from
  `dev9p_stat_native`, preserving the A-3 valid-mask fail-closed trio
  (mode/uid/gid must be server-reported or the stat fails).
- **The capability latch** (the netd lesson, found by the first P-3 boot
  gate): `/net` is served by netd, a 9P server that does NOT implement
  the Stratum extension — it answers op 140 with `Rlerror(E_NOSYS)`
  cleanly (`usr/netd/src/server.rs` dispatch `_ =>
  self.err(tag, p9::E_NOSYS)`). `struct p9_client.wga_unsupported`
  (`9p_client.h`; init at `9p_client.c:884`) latches on the FIRST
  `-T_E_NOSYS` from a Twalkgetattr (`dev9p.c:402`); every later
  `dev9p_walk_attrs` on that session returns
  `DEV_WALK_ATTRS_UNSUPPORTED` RPC-free (`dev9p.c:360`), and stalk falls
  back to the audited per-component loop. Cost: one wasted RPC per
  session lifetime. Only ENOSYS is classified (there is no
  T_E_OPNOTSUPP in the errno registry; appending one is ABI-bearing per
  ERRORS.md). Any OTHER error → NULL (resolution fails; nothing bound).

## devramfs implementation

`kernel/devramfs.c:413` (`devramfs_walk_attrs`); `devramfs_stat_qid`
(`devramfs.c:336`) factored from `devramfs_stat_native` so the walk-fused
records and SYS_FSTAT report identical bytes. A local u64 qid cursor
walks the table; `nc->qid` is assigned only on a full walk, so the strict
shape contract falls out naturally. Per-component names are NUL-copied
into a bounded local for the existing `walk_one` (devramfs's internal
lookup wants NUL-terminated).

## The stalk pounce (resolver side)

`kernel/stalk.c` — full mechanics in `104-stalk.md`; the invariant-
bearing points:

- **THE FAIL-ORDERING INVARIANT** (the audit's #1 target): the post-scan
  consumes the batch strictly LEFT-TO-RIGHT; an X-denial at component k
  MASKS everything past k INCLUDING a partial walk's miss →
  `T_E_ACCES`, never `T_E_NOENT` (no existence probe under a forbidden
  dir). Base X-checked pre-batch; thereafter `sts[j-1]` vouches for
  component j; a partial walk's miss is reported NOENT only after its
  parent `sts[k-1]` passes the X-check.
- **Mount-mid-run split**: the batch may walk PAST a mount point
  server-side (including a FULL underlying walk when the underlying dir
  has a same-named child — those tail results are junk). The post-scan
  tests each walked component's would-be identity via
  `mount_is_point_id(territory, dc, devno, qid_path)`
  (`kernel/territory.c`; membership-only, under `ns_lock`, no ref) and
  on a hit discards the batch, re-walks the validated prefix, pushes the
  mount point, and lets the existing cross-on-descent machinery cross +
  X-check the MOUNTED root. The leaf of a full BIND walk is exempt (the
  quarry/descent cross owns it).
- **`..` disables the pounce** (`path_has_dotdot`, `stalk.c:148`): runs
  compress intermediates into ONE trail entry, so a `..` pop into a
  pounced run has no Spoor to land on. Worst case = exactly today's
  per-component behavior. `logical_depth` (components consumed; monotone
  since pounce_ok excludes `..`) enforces the `STALK_MAX_DEPTH` surface
  the compressed trail no longer measures.
- **Carried attrs**: a run's leaf record (`carried`/`carried_valid`)
  seeds the next run's base X-check and the STALK_OPEN final-hop R/W
  check (open = walkgetattr + lopen = 2 RPCs); invalidated on every
  tip-changing event (cross-on-descent, quarry cross, `..` pop, old-path
  hop push, split push).

## STALK_STAT + SYS_STAT = 88

- `STALK_STAT` (`stalk.h`): the final run of a stat resolution is the
  walk-QUERY — leaf attrs return fused; NO handle, Spoor, or fid ever
  exists (the 1-RPC stat). Fallbacks (a walk_attrs-less Dev, a
  mount-point leaf [POSIX: stat of a mount point reports the mounted
  root], a zero-component path) materialize a quarry +
  `spoor_stat_native` + clunk. `stalk_stat` wrapper: `stalk.c:701`.
- `SYS_STAT(path_va, path_len, stat_va)` (`syscall.h:1587`;
  `sys_stat_handler` `syscall.c:1507`; testable inner
  `sys_stat_for_proc` `syscall.c:1475`): SYS_OPEN's prologue (copy-in,
  NUL reject, LS-4 cwd join for a relative path) + per-byte `t_stat`
  copy-out. Resolution errnos pass through (`-T_E_NOENT`/`-T_E_ACCES`);
  arg/copy faults return bare -1.

## Consumers (P-4)

- **Go fork** (`f569e39`): `os.Stat`/`os.Lstat` →
  `syscall.Stat` = ONE `SYS_STAT` (was O_PATH open + fstat + close = 3
  syscalls ≈ 13 RPCs).
- **libthyla-rs**: `fs::metadata` → `stat_path`
  (`usr/lib/libthyla-rs/src/fs/metadata.rs`); `exists()` deliberately
  keeps its open-based contract.
- **pouch** (`0019-pouch-stat.patch`): musl's `fstatat.c` replaced — the
  AT_FDCWD/absolute forms ride `SYS_thyla_stat 88` (aarch64 musl has no
  `__NR_stat` alias, so pre-0019 the whole `stat(path)` family was a
  silent ENOSYS hole; only `fstat(fd)` was wired). `AT_EMPTY_PATH+""` →
  fstat(fd); relative+real-dirfd → -ENOSYS (honest).
- In-guest probes (every boot, boot-fatal): joey native
  (`t_stat_path("/system.key")` vs fstat + miss → -ENOENT) + pouch
  (`/pouch-hello`: stat vs fstat field equality + miss → ENOENT — the
  0019 wiring proof).

## Tests

`kernel/test/test_stalk.c` (the pounce battery): `pounce_engaged`
(non-vacuity: 1 batched call / 0 per-component walks),
`pounce_acces_masks_noent` (THE fail-ordering regression),
`pounce_parity_nowa` (9-case A/B vs a walk_attrs-less twin Dev),
`pounce_full_walk_past_mount` (the discard + split + cross),
`stat_query` (attrs + no-materialization via `spoor_total_allocated`),
`stat_mount_leaf`, `sys_stat_for_proc` (absolute/relative/NOENT/ACCES/
rejects), `pounce_unsupported_fallback` (the latch: sentinel → 3
per-component walks + ACCES intact). Plus `test_devramfs.c::walk_attrs`
(query/bind/partial shapes) and `test_dev9p.c::walk_attrs` (a loopback
responder speaking Rwalkgetattr: bind full / partial / query). The whole
PRE-EXISTING stalk battery now runs THROUGH the pounce (every stalkfix
carries walk_attrs) — its unchanged expectations are the parity proof.

## Performance (the P-4 close, 2026-07-07)

- **Controlled kernel-side pair** (same aged pool + old toolchain, ONLY
  the kernel changed): go4c build2-warm 4192 → 3006 ms (−28%) — the
  pounce collapses even the OLD 3-syscall stat emulation ~13 → ~3 RPCs
  (O_PATH open = 1 fused walkgetattr; fstat = 1; clunk = 1).
- **The gofmt 91-pkg pair on the fresh twins** (the #34 method; the NEW
  baseline pair for future chunks — the aged pool is gone): cold
  **21.8s / 22.1s**, warm **8.3s / 8.2s** (two boots, twin-restore
  before each; warm is a TRUE full hit, `-v` prints 0 pkgs). Cold vs the
  pre-POUNCE fresh-pool CF-2f 26.9s/27.6s ≈ −19-21%. The warm number is
  NOT comparable to the 3.5-4.8s records measured on OLDER pool fixtures
  — see the recount.
- **The RT recount** (per-phase 9P op deltas, temp instrumented boot;
  baseline #368: Tgetattr 29.3k / Twalk 23.3k / Tclunk 22.6k ≈ 75k
  metadata RTs across a bench window):
  - gofmt-cold window: Twalk **382** (collapsed), Twalkgetattr 6419,
    Tgetattr 6578, Tclunk 3944 — the metadata trio ≈ 17k, vs 75k-class
    before. Tread 39k now dominates the stream.
  - gofmt-warm window (the 8.2s): ~30k ops total — Tread 16.1k (53%),
    Twalkgetattr 4190, Tgetattr 4247, Tclunk 2226, **Twalk 10**. Op
    count collapsed; ~275 µs/op average vs the ~140 µs healthy class →
    the warm cost is PER-OP server-side, not volume: warm reads exactly
    the `/go-cache` tree the cold build just dirtied, and on a FRESH
    pool that uncommitted burst sits in leaf-root chains the #367
    mini-consolidation structurally cannot fold (the aged pool's
    internal roots folded into buffers). Owned by task #59 (#367,
    root-caused, Stratum-side). The POUNCE is exonerated by its own
    recount: fewer ops, each slower for a queued Stratum reason.
    Corroboration: hello build2-warm reads 4.2-4.3s on the fresh twins
    vs 2.9s on a churned pool (post-commit compacted) — same kernel,
    same toolchain, pool state only.
  - **The 1:1 signature**: Tgetattr ≈ Twalkgetattr in EVERY window
    (6590:6528, 6578:6419, 4247:4190) — the residual Tgetattr is the
    per-stalk BASE X-check `stat_native` on the run's starting Spoor
    (each single-run stalk = 1 base Tgetattr + 1 Twalkgetattr), NOT the
    fd-fstat class.

### The Phase-2 attr-cache gate decision (POUNCE-DESIGN §8)

**DEFERRED — the gate condition is not met.** The cache was gated on
"residual fd-fstat Tgetattr still dominates"; the recount shows the
fd-fstat class has already collapsed into the fused op, and the actual
residual Tgetattr is the per-stalk base X-check (1:1 with Twalkgetattr,
~14% of the warm window's ops ≈ ~1.2s of 8.2s). It is dwarfed by (a)
the Tread stream (53%) and (b) the #367 per-op server cost (~2x). The
narrower future candidate is a base-Spoor attr memo (the start Spoor's
own record seeding the first run's X-check — the `carried` idea
extended across stalks), recorded here as a seam, NOT built: it needs
an invalidation story (staleness on the root's perms) and the data says
the next lever is #367 + the read stream, not this.

## Known caveats / seams

- The Phase-2 attr cache: DEFERRED per the gate decision above.
- The per-stalk base X-check costs one Tgetattr on the run's starting
  Spoor (the 1:1 signature) — the base-Spoor attr memo is the recorded
  seam if it ever matters.
- **P-5 audit F1 [P3], tracked as #372**: on a LATCHED session (netd's
  `/net`) the pounce block pays its base X-check Tgetattr before the
  sentinel returns, then the per-component fall-through X-checks the
  same parent again — 2 Tgetattr per component on that fallback path
  (idempotent, no correctness impact; `/net` paths are shallow
  setup-only ops). Fix = reorder the base X-check after the sentinel
  arm (sound per POUNCE-DESIGN §6 no-side-effect walks); deferred at
  close to avoid churning the just-audited deny/release ladder.
- A `..`-bearing path takes the per-component loop (by design).
- The latch is per-SESSION (per p9_client): a mixed-capability boot
  (stratumd yes, netd no) probes once per session, then stays on the
  right path for each.
- `Dev.walk_attrs` is kernel-internal; no EL0 surface changes shape when
  a Dev gains/loses the slot.
