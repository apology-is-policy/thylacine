---
id: sub-kernel-path
type: sub
title: "Path — refcounted copy-on-walk namespace names"
parent: moc-kernel-namespace
code: ["kernel/path.c", "kernel/include/thylacine/path.h"]
audit: hard
guarded-by: [inv-i33]
validated-by: [gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/STALK-DESIGN.md"]
created: 2026-08-01
updated: 2026-08-03
---
## Purpose

`struct Path` is the refcounted, immutable namespace-name string every
Spoor carries (#66) — the Plan 9 4th-edition `Chan.path`, adapted: the
cleaned name by which a Spoor was reached (`/srv/stratum`, `/bin/joey`).
It feeds the introspection readers only — `SYS_FD2PATH = 71`,
`/proc/<pid>/ns` (via the territory-side `PgrpMount.mp_path`, #66b), the
future `/proc/<pid>/fd` (#66c) — and is STRICTLY NON-LOAD-BEARING
([[inv-i33]]). Thylacine's Path is Plan 9's MINUS the `mtpt`
mount-history: `stalk` resolves `..` against its own in-call trail, never
against a Path, so the Path is a pure name string.

## Contract

```c
struct Path { int ref; u32 len; char s[]; };   // s NUL-terminated, starts '/'

struct Path *path_make_root(void);                     // "/" (ref == 1)
struct Path *path_addelem(parent, name, namelen);      // COPY-on-walk append
struct Path *path_parent(const struct Path *p);        // pop last element
void path_ref(struct Path *p);  void path_unref(struct Path *p);  // NULL-safe
u64 path_total_allocated(void); u64 path_total_freed(void);       // diagnostics
```

- `path_addelem` NEVER mutates `parent` — it allocates a fresh Path
  (`.` → a copy; `..` → `path_parent`; else `parent + "/" + name`, the
  root un-doubled). NULL on OOM, `namelen == 0` (the #66a SA-1
  totality guard — a trailing-slash path is unrepresentable), or a
  result over `SYS_OPEN_PATH_MAX` — and the WALK STILL SUCCEEDS with a
  NULL "unknown" name (the I-33 fail-soft).
- `path_ref`/`path_unref` are atomic (`t_atomic_fetch_*_acqrel_int`) and
  extinct on a ≤ 0 pre-value — the spoor UAF/double-free diagnostic
  discipline. NULL is a valid "unknown name" no-op.
- A name from `fd2path` is the path the Spoor was REACHED by, not a live
  lookup — it may be unknown OR stale after a rename/unmount, and is
  never a re-open key (#66a F1, [[fnd-66a-r1-f1]]).

## Mechanism

`path_alloc_raw` kmallocs `sizeof(Path) + len + 1` with `KP_ZERO` (the
trailing NUL pre-placed), ref 1 via a relaxed store — the Path is
unpublished until the caller stores it into a Spoor's `->path`.
`path_parent` scans for the last `/` ("/a/b" → "/a"; "/a" → "/"; "/" →
"/"). The spoor-side hooks (`kernel/spoor.c`, [[sub-kernel-spoor]] —
which owns the `->path` field): `spoor_clone` SHARES the
parent's Path (O(1) incref — the hot walk path runs it on every hop,
including failing ones); `spoor_path_extend` reads the shared Path as the
base, allocates the extended one, installs it, and unrefs the old (safe
precisely because `path_addelem` never mutates — read-then-replace);
`spoor_path_transplant` re-points at another Spoor's Path (mount cross +
the open=connect adoption); `spoor_free_internal` drops the ref. The
resolver hook sites are exactly three: stalk (per step + cross transplant
+ adopt transplant), `sys_walk_open_handler` (single addelem post-walk,
before the result-cross), `sys_walk_create_handler` (the created child).
A missed site is an incomplete-but-never-wrong name.

## Data structures

`struct Path` — flexible-array string, `ref` the ONLY concurrently-mutated
field (the string is immutable once built → lockless reads are safe).
Lifetime is subordinate to the Spoor's: each referencing Spoor holds
exactly one ref for its whole life (NULL at alloc, shared at clone,
dropped at free). The territory-side `PgrpMount.mp_path` (#66b) holds one
ref per table entry across the four hooks (append / MREPL-displace /
unmount-remove / clone-share + final-release) — that lifecycle belongs to
the territory sweep's dossier.

## Concurrency

No lock. Immutable string + atomic ref; set-before-publish discipline on
`Spoor.path` writes (every write site is thread-local or pre-publish —
the #66a grep-complete sound set). `g_path_allocated`/`g_path_freed` are
non-atomic diagnostics (the balance tests run single-threaded — the
spoor-counter precedent).

## Invariants enforced

- **[[inv-i33]]** — this substrate IS the invariant's mechanism half:
  write-only-from-the-resolver, fail-soft NULL, immutable-once-built,
  lifetime subordinate to the Spoor.

## Error paths

Every constructor returns NULL on OOM / bound-violation and the caller
proceeds nameless — no error propagates to a resolution.
`path_ref`/`path_unref` on a freed Path extinct (diagnostic, not an error
return).

## Performance

O(1) share per clone; one allocation + copy per successful resolution
step (`path_addelem` is O(parent + name)); nothing on the failure-unwind
path allocates.

## Prosecution

- The refcount balances on EVERY create/destroy/replace path (the #66a
  round traced 9; the balance is pinned by `path.ref_balance` +
  the no-leak asserts inside `stalk.path_*`).
- No resolver READ of `->path` may ever appear (I-33's grep-complete
  obligation — a read site converts cosmetic corruption into a
  resolution/permission defect).
- `path_addelem` must stay total: `namelen == 0` rejected; the newlen
  arithmetic overflow-guarded; "." / ".." handled (the single-hop
  walk-open can pass them even though stalk never does).
- The immutability property is load-bearing for lockless readers
  (`fd2path` copies under the Spoor's pin, no `path_ref`).

## Seams

- The confined-Proc name disclosure (#66a F4, [[fnd-66a-r1-f4]]): chroot
  name residue + inherited-fd names let a confined Proc read the OUTER
  namespace layout via fd2path — no authority impact; the v1.x
  re-stamp-at-chroot is the recorded fix.
- `/proc/<pid>/fd` (#66c) — deferred on the #926 handle-table-lifetime
  restructure; the substrate is ready.

## Caveats

- A Path is diagnostic provenance, not identity: two Spoors reaching one
  object by different names carry different Paths; a rename leaves every
  live Path stale. Consumers must treat the bytes as untrusted display
  data (the #66a withdrawn-set note: control bytes in names are the walk
  layer's pre-existing validation domain, and the /proc renderers must
  not trust them).

## Provenance

(generated — incoming `touched` backlinks, newest first; never
hand-written)
