# 18 — Territory primitives (P2-E)

The Plan 9 territory — a process's view of the resource tree, composed via `bind` and `unmount`. Per `ARCHITECTURE.md §9.1`. v1.0 P2-Eb lands the kernel-internal API; the syscall surface lands at Phase 5+ (the v1.0 kernel only has internal callers — tests + the future ramfs init flow).

---

## Purpose

A `Territory` (process group, Plan 9 idiom — confusing name; it's actually the *territory* group, not the POSIX session group) holds one process's territory: a directed graph of bindings. Each Proc has its own `Territory` (private territory at v1.0; RFNAMEG-shared territories are Phase 5+).

Key invariants (proven in `specs/territory.tla`):

- **Cycle-freedom (ARCH §28 I-3)**: the bind graph is acyclic. Adding a bind that would close a cycle is rejected.
- **Isolation (ARCH §28 I-1)**: structural — `bindings[p]` and `bindings[q]` for p ≠ q are independent function values; no operation modifies two procs simultaneously.

---

## Public API — `<thylacine/territory.h>`

```c
#define PGRP_MAGIC      0x50475250C0DEFADEULL
#define PGRP_MAX_BINDS  8                       // v1.0 cap; Phase 5+ → growable RB tree

typedef u32 path_id_t;                          // abstract; Phase 4 → struct Spoor *

struct PgrpBind {
    path_id_t src;                              // bound content
    path_id_t dst;                              // mount point; walking dst yields src
};

struct Territory {
    u64               magic;                    // PGRP_MAGIC
    int               ref;                      // refcount; rfork(RFNAMEG) shares (P5+)
    int               nbinds;                   // valid entries in binds[]
    struct PgrpBind   binds[PGRP_MAX_BINDS];
};
_Static_assert(sizeof(struct Territory) == 16 + 8 * PGRP_MAX_BINDS, ...);

void          territory_init(void);                  // bootstraps kpgrp; before proc_init
struct Territory  *kpgrp(void);                      // accessor for kproc's Territory
struct Territory  *territory_alloc(void);                 // SLUB-allocate empty (ref=1)
struct Territory  *territory_clone(struct Territory *parent);  // deep copy; rfork(RFPROC)
void          territory_ref(struct Territory *p);
void          territory_unref(struct Territory *p);

int           bind(struct Territory *p,
                   path_id_t src, path_id_t dst);   // 0=ok, -1=cycle, -2=dup, -3=full, -4=self
int           unmount(struct Territory *p,
                      path_id_t src, path_id_t dst); // 0=ok, -1=not-bound

int           territory_nbinds(struct Territory *p);
u64           territory_total_created(void);
u64           territory_total_destroyed(void);
```

### `bind(territory, src, dst)` — return semantics

| Return | Meaning |
|---|---|
| `0`  | success; edge `dst -> src` added |
| `-1` | cycle would be created (existing edges would form `src -> ... -> dst`, then `dst -> src` closes loop) |
| `-2` | edge already exists (idempotent re-bind; v1.0 is "ok-but-error" for caller diagnostics) |
| `-3` | binds[] full (PGRP_MAX_BINDS reached) |
| `-4` | self-bind (`src == dst`); treated as a degenerate length-1 cycle |

The cycle check is a fixed-point iteration:
- Starting from `{src}`, repeatedly add `e.src` for any existing edge `(e.src, e.dst)` where `e.dst` is in the current set.
- After fixed point, if `dst` is in the set, the new edge would close a cycle.

Maps directly to `specs/territory.tla`'s `WouldCreateCycle(p, src, dst)`:
```
WouldCreateCycle(p, src, dst) ==
    \/ src = dst
    \/ dst \in Reachable(p, {src})
```

---

## Implementation

`kernel/territory.c` (~150 LOC).

### Territory lifecycle

- `territory_init`: SLUB cache + `kpgrp` (kproc's empty Territory; ref=1). Called from `boot_main` BEFORE `proc_init` (kproc needs a territory at allocation).
- `territory_alloc`: SLUB-allocate via `kmem_cache_alloc(KP_ZERO)`. Sets magic + ref=1. Returns NULL on OOM.
- `territory_clone`: allocate fresh + deep-copy `parent->binds[0..nbinds-1]`. Models the spec's `ForkClone` action.
- `territory_ref` / `territory_unref`: refcount. `territory_unref(p)` frees via `kmem_cache_free` when ref hits 0. NULL-safe; `kpgrp` rejects unref.

### Cycle detection (`would_create_cycle`)

Algorithm: O(N²) worst case where N = `territory->nbinds`. For PGRP_MAX_BINDS=8 that's 64 inner iterations — well under any latency budget.

```c
static bool would_create_cycle(const struct Territory *territory,
                               path_id_t src, path_id_t dst) {
    if (src == dst) return true;
    path_id_t reachable[PGRP_MAX_BINDS + 1];
    int n = 0; reachable[n++] = src;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < territory->nbinds; i++) {
            path_id_t e_src = territory->binds[i].src;
            path_id_t e_dst = territory->binds[i].dst;
            if (path_in(reachable, n, e_dst) && !path_in(reachable, n, e_src)) {
                reachable[n++] = e_src;
                changed = true;
            }
        }
    }
    return path_in(reachable, n, dst);
}
```

### Integration with rfork

`kernel/proc.c::rfork` (P2-Eb update): on `RFPROC` (the only flag supported at v1.0), allocates the child's territory via `territory_clone(parent->territory)`. If territory_clone fails (OOM), rolls back the proc allocation (transitions to ZOMBIE so `proc_free`'s lifecycle gate passes; `proc_free` then handles `territory_unref(NULL)` no-op for the partial state, OR territory_unref(child_pgrp) if it succeeded).

`kernel/proc.c::proc_free`: extended to call `territory_unref(p->territory)` before `kmem_cache_free`. At v1.0 each Proc has refcount=1 on its private territory, so `territory_unref` frees the territory slot. Phase 5+ shared territories decrement refcount and free at last release.

`struct Proc` grew 80 → 88 bytes (added `struct Territory *territory`).

---

## Spec cross-reference

`specs/territory.tla` at P2-Ea:
- 5 actions: `Init`, `Bind`, `BuggyBind`, `Unbind`, `ForkClone`.
- 1 invariant: `NoCycle` (ARCH §28 I-3).
- 2 configs: `territory.cfg` (clean; 625 distinct states) + `namespace_buggy.cfg` (BuggyBind skips cycle check; counterexample at depth 4 / 95 states).

Mapping:

| Spec action | Source location |
|---|---|
| `Init` | `kernel/territory.c::territory_init` |
| `Bind(p, src, dst)` | `kernel/territory.c::bind` |
| `Unbind(p, src, dst)` | `kernel/territory.c::unmount` |
| `ForkClone(parent, child)` | `kernel/territory.c::territory_clone` (called by `kernel/proc.c::rfork`) |
| `BuggyBind(p, src, dst)` | (none — bug class statically prevented; bind() always calls `would_create_cycle` before insert) |

| Spec invariant | Source enforcement |
|---|---|
| `NoCycle` | `bind`'s `would_create_cycle` precondition; rejects with `-1` if a cycle would form. Plus `src == dst` rejected with `-4` (length-1 cycle). |

Isolation (I-1) is structural — see spec preamble.

---

## Tests

- `territory.bind_smoke`: alloc Territory, bind a few non-cyclic edges, verify nbinds tracking + idempotent rebind detection (-2) + unmount round-trip.
- `territory.cycle_rejected`: build chain `a → b → c` via two binds; attempt cycle-closing bind `c → a`; verify `-1`. Self-bind `5 → 5` rejected with `-4`. Bind table unchanged after rejection.
- `territory.fork_isolated`: parent binds; territory_clone child; parent binds more; child binds independently; verify each Territory's nbinds reflects its own ops only.

---

## Known caveats / footguns

### `path_id_t` is u32 at v1.0

Phase 4's 9P client + Spoor integration replaces `path_id_t` with `struct Spoor *` (or qid for the RB-tree key per ARCH §9.1). The cycle-detection algorithm is agnostic to the concrete representation, but callers using numeric IDs at v1.0 must NOT rely on numeric ordering or arithmetic.

### PGRP_MAX_BINDS = 8

Sufficient for v1.0's test scenarios + the eventual ramfs/proc/dev/net mount sequence at boot. Container init flows that bind > 8 mount points hit `-3` (full). Phase 5+ replaces with a growable RB tree.

The cap is small intentionally: SLUB's KP_ZERO is a byte-by-byte loop; larger Territories make `territory_alloc` slower per call, which compounds at the `proc.rfork_stress_1000` test rate. 8 entries × 8 bytes = 64-byte binds[] keeps the total Territory under ~80 bytes and keeps the rfork stress under the 500 ms boot budget on QEMU.

### RFNAMEG (shared territory) is not implemented

`rfork(RFNAMEG)` extincts at v1.0 (per `kernel/proc.c::rfork`'s flag check). The Territory `ref` field exists for forward-looking sharing semantics but is always `1` at v1.0. Phase 5+ syscall surface lands the share path:
- `rfork(RFPROC | RFNAMEG)`: child shares parent's territory; territory_ref instead of territory_clone.
- territory_unref decrements; frees only at last reference.

### Single-CPU lifecycle at v1.0

bind / unmount / territory_clone are not internally synchronized; concurrent callers on different CPUs would race. At v1.0 P2-Eb only the boot CPU calls these (rfork is single-CPU; tests run on boot). Phase 5+ adds a per-Territory lock when multi-thread Procs and shared territories make concurrent access possible.

### Mount union semantics not modeled

ARCH §9.1 specifies MREPL / MBEFORE / MAFTER / MCREATE flags for `bind`. v1.0 P2-Eb's `bind` is unflagged: an edge is either present or absent. Mount-union ordering (lookup priority) is Phase 5+. The cycle-freedom invariant doesn't depend on union semantics.

### `mount` (9P-server-attaching variant) is not implemented

ARCH §9.1's `mount(fd, afd, old, flags, aname)` attaches a 9P server. v1.0 P2-Eb has no 9P client; `mount` lands at Phase 4 alongside the 9P client. The structural invariants (cycle-freedom, isolation) carry over identically.

---

## Status

| Component | State |
|---|---|
| `territory.h` API + `territory.c` impl | Landed (P2-Eb) |
| `struct Proc.territory` field | Landed (P2-Eb) |
| `rfork(RFPROC)` clones territory | Landed (P2-Eb) |
| `proc_free` releases territory | Landed (P2-Eb) |
| `territory_init` bootstrap | Landed (P2-Eb; before `proc_init`) |
| Cycle detection in `bind` | Landed (P2-Eb) |
| In-kernel tests | 3 added: `territory.bind_smoke`, `territory.cycle_rejected`, `territory.fork_isolated` |
| Spec `territory.tla` + buggy config | Landed (P2-Ea) |
| Per-Territory lock | Phase 5+ |
| RFNAMEG shared territory | Phase 5+ |
| Mount-union flags (MREPL/MBEFORE/MAFTER) | Phase 5+ |
| `mount` (9P-server-attaching) | Phase 4 |
| RB tree key=qid (replacing flat array) | Phase 5+ when bind count growth justifies |
| Walk / path-resolution | Phase 4+ (with Spoor + 9P) |
