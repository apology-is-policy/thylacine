// Process group (territory) — Plan 9 Territory, Thylacine adaptation (P2-Eb).
//
// Per ARCHITECTURE.md §9.1 + specs/territory.tla. A Territory owns a process's
// territory: the set of bindings that map paths to other paths (or 9P
// resources, when the 9P client lands at Phase 4). At v1.0 P2-Eb the
// territory is a flat list of edges with abstract path identifiers
// (`path_id_t = u32`); bind() enforces cycle-freedom (ARCH §28 I-3) via
// transitive-closure DFS. RFNAMEG-shared territories (multiple Procs
// sharing one Territory) are deferred to Phase 5+ syscall surface; the
// refcount field exists so the share path can land without re-shaping
// the struct.
//
// The kernel-internal `bind` and `unmount` operate on Territory pointers
// directly (no syscall layer at v1.0). When the Phase 5+ syscall layer
// lands, it dispatches to these via the calling Proc's territory pointer.
//
// State invariant (proven in specs/territory.tla NoCycle): for every
// Territory `p` and every path `x`, `x` is not reachable from its own
// binding set via the transitive closure. Equivalently: there is no
// non-empty walk x -> ... -> x in the bind graph.

#ifndef THYLACINE_PGRP_H
#define THYLACINE_PGRP_H

#include <thylacine/types.h>

// PGRP_MAGIC — sentinel set at territory_alloc; checked at territory_unref final
// release. SLUB's freelist write at kmem_cache_free clobbers offset 0,
// catching double-free.
#define PGRP_MAGIC 0x50475250C0DEFADEULL    // 'PGRP' || 0xC0DE'FADE

// Bind table size. v1.0 P2-Eb: small fixed cap (8 edges per territory).
// Sufficient for v1.0's bind/cycle/fork tests + the eventual ramfs +
// /proc + /dev + /net mounts (typical 4-6 binds at boot). Phase 5+
// replaces with a growable RB tree per ARCH §9.1's design intent.
//
// Kept small at v1.0 because rfork stress (proc.rfork_stress_1000)
// performs 1000 territory_clone calls; each clone triggers KP_ZERO of the
// full Territory struct via kmem_cache_alloc, and SLUB's KP_ZERO byte-loop
// is unoptimized — making the struct large enough to exceed the boot
// budget under QEMU emulation. 8 entries × 8 bytes = 64 bytes binds[],
// keeping the total struct under ~80 bytes.
#define PGRP_MAX_BINDS  8

// Path identifier. At v1.0 abstract `u32` — bind/unmount take whatever
// numeric ID the caller decides on (tests pick small integers). When the
// 9P client lands at Phase 4, this becomes a `struct Spoor *` (or `qid_t`
// for the RB-tree key per ARCH §9.1). The cycle-freedom logic is
// agnostic to the concrete representation.
typedef u32 path_id_t;

// One edge in the bind graph: walking `dst` produces `src` per ARCH §9.1
// ("bind(old, new) — attach old's contents at new"). Spec maps:
//   - PgrpBind.dst   <- spec's `dst` (the mount point)
//   - PgrpBind.src   <- spec's `src` (the bound content)
// An edge `(src, dst)` is `dst -> src` in the walk direction.
struct PgrpBind {
    path_id_t src;
    path_id_t dst;
};

struct Territory {
    u64               magic;     // PGRP_MAGIC
    int               ref;       // refcount; rfork(RFNAMEG) shares (Phase 5+)
    int               nbinds;    // number of valid entries in binds[]
    struct PgrpBind   binds[PGRP_MAX_BINDS];
};

_Static_assert(sizeof(struct Territory) == 16 + 8 * PGRP_MAX_BINDS,
               "struct Territory size pinned (P2-Eb baseline). Adding a "
               "field grows the SLUB cache; update this assert "
               "deliberately so the change is intentional.");
_Static_assert(__builtin_offsetof(struct Territory, magic) == 0,
               "magic must be at offset 0 (SLUB freelist write "
               "clobbers it on free, double-free defense)");

// Bring up the territory subsystem. Allocates the territory SLUB cache and
// kproc's initial Territory (empty bindings; ref=1). Must be called after
// slub_init and BEFORE proc_init (so kproc has a territory at allocation
// time).
void territory_init(void);

// Accessor for kproc's initial Territory.
struct Territory *kpgrp(void);

// SLUB-allocate a fresh empty Territory with ref=1. Returns NULL on OOM.
struct Territory *territory_alloc(void);

// Allocate a new Territory and copy parent's bindings into it (refcount
// initialized to 1). Models the spec's ForkClone action — used by
// rfork(RFPROC) without RFNAMEG. Returns NULL on OOM.
struct Territory *territory_clone(struct Territory *parent);

// Increment/decrement refcount. territory_unref frees the struct when the
// last reference goes away. Both are safe to call on NULL.
void territory_ref(struct Territory *p);
void territory_unref(struct Territory *p);

// Bind: add the edge `dst -> src` to `territory`'s bind graph. Walking `dst`
// in this territory will subsequently produce `src` (per ARCH §9.1
// semantics). Cycle-checked: rejects if adding the edge would create a
// cycle.
//
// Return values:
//    0   success.
//   -1   would create a cycle (rejects per `WouldCreateCycle` in
//        territory.tla; corresponds to Plan 9's "territory cycle:
//        cannot bind X onto Y" errstr).
//   -2   the edge already exists (idempotent rebind is a no-op-error
//        at v1.0; future MREPL/MBEFORE/MAFTER union semantics treat
//        this differently).
//   -3   binds[] full (PGRP_MAX_BINDS reached).
//   -4   trivial self-bind (src == dst); treated as a cycle.
//
// At v1.0 P2-Eb the data structure is single-CPU only; multi-Proc
// concurrent bind is Phase 5+ when sharing is permitted (Territory will gain
// a per-Territory lock).
int bind(struct Territory *territory, path_id_t src, path_id_t dst);

// Unmount: remove the edge `dst -> src` from `territory`'s bind graph.
// Returns 0 on success, -1 if the edge does not exist.
int unmount(struct Territory *territory, path_id_t src, path_id_t dst);

// Diagnostic.
int  territory_nbinds(struct Territory *territory);
u64  territory_total_created(void);
u64  territory_total_destroyed(void);

#endif // THYLACINE_PGRP_H
