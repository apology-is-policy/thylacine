# 30 — Dev vtable + Spoor lifecycle (P4-A)

The kernel device model. Every kernel-internal device — `/dev/cons`, `/dev/null`, `/proc`, `/ctl`, future userspace-driver-backed devs — implements the verbatim Plan 9 `Dev` vtable from `ARCHITECTURE.md §9.2`. Per-device state flows through `Spoor` handles (Plan 9 `Chan`, renamed at the Phase 4 entry per the marsupial-naming pass — see handoff 024). P4-A lands the infrastructure: the `Dev` vtable struct, the `bestiary[]` registry, the `Spoor` SLUB-backed lifecycle (alloc / ref / unref / clone / clunk), and the `devnone` no-op stub that anchors unconfigured Spoors and serves as the bestiary's first occupant.

P4-B will add the trivial real Devs (`cons`, `null`, `zero`, `random`); P4-C/D/E add `/proc`, `/ctl`, `/ramfs`. P4-F+ wires VirtIO transport and userspace-driver Devs.

---

## Purpose

Two layers, mutually load-bearing:

- **`struct Dev`** (the vtable): the contract every device satisfies. ARCH §9.2 verbatim — 16 ops + `dc` + `name`. Identity is `dc` (the device character); name is human-readable.
- **`struct Spoor`** (the per-position handle): walked through a Dev's namespace by `dev->walk`; opened by `dev->open`; freed by `spoor_clunk`. Mirrors Plan 9's `Chan` semantics with C99 typing.

The `bestiary[]` array is the kernel-wide registry — sentinel-terminated, populated by `dev_register`, walked by `dev_init` to drive each Dev's one-time `init` hook.

Naming rationale (per handoff 024): the rename `Chan -> Spoor` was user-driven during Phase 4 entry. "Spoor" is the animal-track word — the trail an animal leaves and the hunter follows; the verb-noun pair "walk a spoor" reads naturally for the Plan 9 `walk(Chan, name)` -> `Spoor + qid` operation. `devtab -> bestiary` follows the medieval-fauna-catalog pattern; on-brand for the marsupial OS.

---

## Public API — `<thylacine/dev.h>`

```c
#define BESTIARY_MAX 32

struct Dev {
    int          dc;
    const char  *name;

    void   (*reset)(void);
    void   (*init)(void);
    void   (*shutdown)(void);

    struct Spoor   *(*attach)(const char *spec);
    struct Walkqid *(*walk)(struct Spoor *c, struct Spoor *nc,
                            const char **name, int nname);
    int             (*stat)(struct Spoor *c, u8 *dp, int n);

    struct Spoor *(*open)(struct Spoor *c, int omode);
    void          (*create)(struct Spoor *c, const char *name, int omode, u32 perm);
    void          (*close)(struct Spoor *c);

    long           (*read)(struct Spoor *c, void *buf, long n, s64 off);
    struct Block  *(*bread)(struct Spoor *c, long n, s64 off);
    long           (*write)(struct Spoor *c, const void *buf, long n, s64 off);
    long           (*bwrite)(struct Spoor *c, struct Block *bp, s64 off);

    void          (*remove)(struct Spoor *c);
    int           (*wstat)(struct Spoor *c, u8 *dp, int n);
    struct Spoor *(*power)(struct Spoor *c, int on);
};

extern struct Dev *bestiary[];        // sentinel-terminated
extern struct Dev  devnone;

void          dev_init(void);
int           dev_register(struct Dev *d);
struct Dev   *dev_lookup_by_dc(int dc);
struct Dev   *dev_lookup_by_name(const char *name);
int           dev_count(void);
```

### Vtable typing — deviation from Plan 9

ARCH §9.2 uses the Plan 9 9front signatures verbatim (`char *name`, `char *spec`, `void *buf`). The P4-A header upgrades input strings to `const char *` (read-only inputs) and the `write` buffer to `const void *` — a C99 const-correctness layer that doesn't change the vtable shape. The output buffers (`stat`, `wstat`'s `dp`) and the `read` buffer remain non-const because the dev fills them.

### `dev_register(d)` — return semantics

| Return | Meaning |
|---|---|
| `>= 0` | success: index where d was placed in `bestiary[]`. |
| extincts | d is NULL, or d->dc collides, or d->name collides, or `bestiary[]` would exceed `BESTIARY_MAX`. |

Collision is treated as an extinction (not a soft error) because every collision is a programmer bug at boot — there's no recovery path that would rescue a system whose bestiary is internally inconsistent.

### `dev_init()` — boot sequence

1. `spoor_init()` — Spoor SLUB cache.
2. `dev_register(&devnone)` — devnone always first.
3. (Future P4-B+ devs register themselves here.)
4. Walk `bestiary[]` calling each non-NULL `d->init()`.
5. Print the dev banner: `dev:  N registered (none, ...)`.

Idempotent guard extincts on re-call.

---

## Public API — `<thylacine/spoor.h>`

```c
#define SPOOR_MAGIC 0x53504F4F52BAD2EAULL   // 'SPOOR\0' || 0xBA 0xD2 0xEA

struct Qid {
    u64 path;       // unique within the dev
    u32 vers;       // file version
    u8  type;       // QTDIR | QTAPPEND | QTFILE | ...
    u8  pad[3];
};
_Static_assert(sizeof(struct Qid) == 16, ...);

#define QTDIR 0x80
#define QTAPPEND 0x40
#define QTEXCL 0x20
#define QTAUTH 0x08
#define QTTMP  0x04
#define QTFILE 0x00

#define COPEN (1u << 0)
#define CMSG  (1u << 1)

struct Spoor {
    u64           magic;       // SPOOR_MAGIC; clobbered by SLUB on free
    int           dc;          // matches dev->dc
    struct Dev   *dev;         // back-pointer
    struct Qid    qid;         // path identity within the dev
    spin_lock_t   lock;        // reserved for Phase 5+ SMP
    int           ref;         // refcount; spoor_alloc sets to 1
    u32           flag;        // COPEN | CMSG
    int           mode;        // omode after open()
    s64           offset;      // read/write cursor
    void         *aux;         // dev-private
};
_Static_assert(__builtin_offsetof(struct Spoor, magic) == 0, ...);

struct Walkqid {
    struct Spoor *spoor;
    int           nqid;
    struct Qid    qid[];       // flexible array
};

void          spoor_init(void);
struct Spoor *spoor_alloc(struct Dev *d);
void          spoor_ref(struct Spoor *c);
void          spoor_unref(struct Spoor *c);
struct Spoor *spoor_clone(struct Spoor *c);
void          spoor_clunk(struct Spoor *c);

u64           spoor_total_allocated(void);
u64           spoor_total_freed(void);
```

### `spoor_alloc(d)` — return semantics

| Return | Meaning |
|---|---|
| non-NULL | success: fresh Spoor with magic set, dc cached from `d->dc`, dev = d, ref = 1, flag/mode/offset = 0, aux = NULL, qid zeroed. The caller is the sole holder. |
| NULL | failure: d == NULL or SLUB OOM. |

### `spoor_unref(c)` — invalidation

After the unref that brings ref to 0, the c pointer is **invalid**. The SLUB freelist clobbers the magic at offset 0; spoor.c additionally writes `c->magic = 0` explicitly so a stale-pointer dereference between free and SLUB-list-write extincts cleanly rather than reading plausible-looking stale fields.

Tests use `spoor_total_freed()` deltas to verify free transitions rather than dereferencing the freed pointer.

### `spoor_clone(c)` — semantics

Allocates a fresh Spoor copying `dc / dev / qid / flag / mode / offset / aux`. The new Spoor has its own ref=1; the source's ref is unchanged. `aux` is shallow-copied — Devs whose aux owns refcounted state (e.g., a 9P fid in Phase 4+) **must** take their own ref inside `dev->walk` before populating the clone's aux.

### `spoor_clunk(c)` — semantics

Calls `c->dev->close(c)` if non-NULL, then `spoor_unref(c)`. The dev's close hook is responsible for releasing per-Spoor resources held outside the Spoor struct (e.g., a 9P fid). Idempotent across not-yet-opened Spoors — devnone's close is a no-op; future Devs gate work on `(c->flag & COPEN)`.

### Defensive checks (extinct on violation)

- NULL where required (`spoor_ref`).
- Corrupted magic (`spoor_ref` / `spoor_unref` / `spoor_clone` / `spoor_clunk`) — UAF defense.
- `spoor_unref` on a zero-ref Spoor — refcount underflow.
- `spoor_clone` of a zero-ref Spoor — already-freed identity.
- `spoor_alloc` before `spoor_init` — bring-up ordering bug.

---

## devnone — the no-op stub

`kernel/devnone.c` defines `struct Dev devnone` with `dc='-'` and `name="none"`. Every op is a safe no-op or graceful failure:

| Op | Returns / Effect |
|---|---|
| `reset` / `init` / `shutdown` / `close` / `create` / `remove` | no-op (void) |
| `attach` / `walk` / `open` / `bread` / `power` | NULL |
| `stat` / `read` / `write` / `bwrite` / `wstat` | -1 |

devnone is registered first by `dev_init()`. It serves two distinct roles:

1. **Sentinel anchor** for `Spoor`s that haven't been attached to a real Dev (e.g., test scaffolding, error paths, future driver-supervision recovery).
2. **Audit guard** — any production Spoor with `dev == &devnone` is a bug (you forgot to attach to the right Dev). Future audit rounds can grep the boot-time bestiary banner to confirm the production list contains `none` plus the expected real Devs.

The dc='-' character is unused in the Plan 9 device-character space (cons='c', proc='p', ether='e', ...); `'-'` is the conventional "stub slot" sigil.

---

## Implementation

`kernel/spoor.c` (~150 LOC), `kernel/dev.c` (~120 LOC), `kernel/devnone.c` (~110 LOC).

### Lifecycle

- **`spoor_init`**: SLUB cache + idempotency guard. Cache allocated with no `KMC_PANIC_ON_FAIL` flag — userspace OOM via `spoor_alloc` returns NULL rather than extincting.
- **`spoor_alloc_internal`** (static): the shared body for `spoor_alloc` + `spoor_clone`. SLUB-allocate via `kmem_cache_alloc(KP_ZERO)`, set `magic`, `dc`, `dev`, init lock, set `ref=1`, leave qid/flag/mode/offset/aux as zero.
- **`spoor_alloc(d)`**: thin wrapper around `spoor_alloc_internal` — kept distinct so the two ref-creation sites have explicit names.
- **`spoor_clone(c)`**: validates source magic + ref > 0, calls `spoor_alloc_internal(c->dev)`, then field-copies qid / flag / mode / offset / aux.
- **`spoor_free_internal`** (static): symmetric internal helper invoked when ref reaches 0. Validates magic + ref == 0, clobbers magic, calls `kmem_cache_free`, increments `g_spoor_freed`.

### Refcount ops

```c
void spoor_unref(struct Spoor *c) {
    if (!c) return;
    if (c->magic != SPOOR_MAGIC) extinction(...);
    if (c->ref <= 0) extinction(...);
    c->ref--;
    if (c->ref == 0) spoor_free_internal(c);
}
```

NULL-safe (mirrors `burrow_unref`); the dual check on magic + ref defends against use-after-free and refcount underflow.

### `spoor_clunk(c)` — close-then-unref

```c
void spoor_clunk(struct Spoor *c) {
    if (!c) return;
    if (c->magic != SPOOR_MAGIC) extinction(...);
    if (c->ref <= 0) extinction(...);
    if (c->dev && c->dev->close) c->dev->close(c);
    spoor_unref(c);     // may invalidate c
}
```

The close hook fires before the unref. The unref may free the Spoor; after that point `c` is invalid.

### Bestiary

```c
struct Dev *bestiary[BESTIARY_MAX + 1];
static int  g_dev_count;
```

`dev_register` walks the prefix to the current count, checking for `dc` + `name` collision; appends d at `g_dev_count`; bumps `g_dev_count`; writes the sentinel NULL at the new tail. Linear scan; v1.0's < 16 devs make this O(N²) for boot-time registration but each step touches < 1 cache line.

Lookup is linear-scan from `bestiary[0]` to the first NULL.

### `dev_init` walk

```c
void dev_init(void) {
    if (g_dev_init_done) extinction(...);
    spoor_init();
    dev_register(&devnone);
    int initialized = 0;
    while (initialized < g_dev_count) {
        struct Dev *d = bestiary[initialized];
        if (d && d->init) d->init();
        initialized++;
    }
    g_dev_init_done = true;
    /* print banner */
}
```

The watermark `initialized` lets a `dev->init()` registered later devs (e.g., a virtio probe that fans out to multiple instances); each new dev gets `init()` called in its turn, exactly once.

### Bootstrap order (post-P4-A)

```
slub_init → territory_init → handle_init → burrow_init → vma_init →
asid_init → proc_init → thread_init → sched_init → dev_init
```

`dev_init` runs after `sched_init` so any future driver-side `dev->init` that allocates a Spoor + walks has a working scheduler context. devnone's init is a no-op so the order is loose at P4-A; it tightens once P4-B/C/D devs land.

---

## Spec cross-reference

P4-A is impl-only (no new TLA+ module). The lifecycle is single-CPU at v1.0 — `spoor_alloc / spoor_unref / spoor_clone / spoor_clunk` follow the same shape as `burrow.c`'s refcount discipline, which `specs/burrow.tla::NoUseAfterFree` already proves.

The dual-refcount pattern from `burrow.tla` does NOT apply to Spoor: Spoor has a single refcount (no separate "mappings" axis). The simpler "alive iff ref > 0" invariant is enforced by the impl-side magic check + ref dual-check at decrement.

When P4-F lands the 9P client + Phase 4's userspace driver paths, `specs/9p_client.tla` (per-session tag uniqueness, fid lifecycle) will pin the cross-Spoor invariants that emerge from the userspace driver protocol.

---

## Tests

`kernel/test/test_dev.c` — 9 tests:

- `dev.boot_registration_smoke` — `dev_count() >= 1` after boot; `dev_lookup_by_dc('-')` and `dev_lookup_by_name("none")` both return `&devnone`; devnone identity self-check.
- `dev.lookup_unknown` — unknown dc + unknown name + NULL name return NULL (no crash).
- `dev.devnone_ops_smoke` — every devnone op invoked; verifies the documented sentinel return.
- `spoor.alloc_unref_round_trip` — alloc + unref → freed; counters increment exactly +1/+1.
- `spoor.ref_lifecycle` — alloc + ref + 2x unref → freed at ref=0.
- `spoor.clone_lifecycle` — alloc + clone produces a NEW Spoor; both freed independently.
- `spoor.clone_copies_state` — `qid / flag / mode / offset / dev / dc` all carry over from source to clone.
- `spoor.clunk_dispatches_close` — `spoor_clunk` invokes `dev->close` exactly once before the unref. Uses an instrumented test-only Dev (dc='%', name="test_only") whose close hook increments a counter.
- `spoor.alloc_10k_no_leak` — 1000 alloc/unref cycles (the boot-budget compromise; the full 10K exit criterion lands at P4-B alongside the real `/dev/null` Dev).

---

## Status

| Component | State |
|---|---|
| `dev.h` API + `dev.c` infrastructure | Landed (P4-A) |
| `spoor.h` API + `spoor.c` lifecycle | Landed (P4-A) |
| `devnone.c` no-op stub | Landed (P4-A) |
| `bestiary[]` + `dev_register` + `dev_lookup_by_*` | Landed (P4-A) |
| `dev_init()` boot wire-up in main.c | Landed (P4-A) |
| In-kernel tests | 9 covering bestiary registration, devnone ops, Spoor lifecycle, ref/unref balance, clone semantics, clunk dispatch, no-leak loop. |
| 10K no-leak on `/dev/null` (ROADMAP §6.2 exit criterion) | Held to P4-B (P4-A runs 1000 iters against devnone). |
| Spec coverage | None added at P4-A (impl-only; reuses burrow.tla's refcount pattern). `specs/9p_client.tla` covers Spoor cross-session invariants when Phase 4 9P client lands. |
| Phase 4 trip-hazards | #157 still open (forward-looking P0 — Phase 4 blocker before any chunk that exec's a fresh userspace process). P4-A is kernel-only and does not trigger #157. |

---

## Known caveats / footguns

### `c` becomes invalid after the unref/clunk that frees it

After `spoor_unref` or `spoor_clunk` brings ref to 0, the c pointer is invalid. spoor.c clobbers `c->magic = 0` before the SLUB-list write so a stale-pointer dereference extincts on the magic check; tests use `spoor_total_freed()` deltas rather than dereferencing freed pointers.

### `spoor_clone` shallow-copies aux

`aux` is a `void *`; `spoor_clone` shallow-copies it. Devs whose aux points to refcounted state (e.g., a 9P fid object in Phase 4+) MUST take their own ref inside `dev->walk` before populating the new Spoor's aux. spoor.c does NOT interpret aux semantics.

### `dev_register` is single-CPU at v1.0

The bestiary mutation path is not internally synchronized; v1.0 P4-A only the boot CPU registers Devs (during `dev_init`). Phase 5+ adds a guard if late-registration paths emerge (e.g., hot-plug at runtime).

### bestiary sized at compile time

`BESTIARY_MAX = 32` is generous for v1.0 (devnone + ~7 kernel-internal + ~5 driver classes = ~13). Late-Phase 4 may need to grow if many virtio instance variations register — bumping the constant is a one-line change. Phase 5+ may switch to a growable structure if late-registration becomes a routine pattern.

### `_Static_assert(sizeof(struct Spoor)) deliberately absent

The struct Spoor includes `enum`-free fields and pointer-sized members; total size is implementation-defined under different ABIs. The struct layout pin is on `__builtin_offsetof(struct Spoor, magic) == 0` (which the SLUB freelist write defense depends on) and `sizeof(struct Qid) == 16` (which the wire-format expectation depends on).

### Dev `close` hook may run before `open`

`spoor_clunk` invokes `dev->close` unconditionally for any non-NULL hook, even if the Spoor was never `open`'d. Devs with hot resources must gate work on `(c->flag & COPEN)`. devnone's close is a no-op (no resources to gate); P4-B's cons / null / zero / random follow the gate-on-COPEN pattern.

---

## References

- `docs/ARCHITECTURE.md` §9 — Territory and device model. ARCH §9.2 is the verbatim vtable spec.
- `docs/ROADMAP.md` §6 — Phase 4 deliverables + exit criteria.
- `docs/handoffs/024-phase3-close-phase4-rename.md` — naming rationale for Spoor / bestiary / devnone.
- `docs/reference/20-burrow.md` — pattern reference for refcount + magic + counter discipline.
