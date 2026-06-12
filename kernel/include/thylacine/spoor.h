// Spoor — kernel-internal handle to a position within a Dev's namespace
// (P4-A).
//
// Per ARCHITECTURE.md §9 + ROADMAP.md §6. A Spoor is the Plan 9 `Chan`
// (renamed at the Phase 4 entry per the marsupial-naming pass): the
// thing that flows through walk / open / read / write / close. Created
// by `dev->attach()` at namespace mount; advanced by `dev->walk()`;
// converted to a read/write cursor by `dev->open()`; freed by
// `spoor_clunk()` (which calls `dev->close()` on its way out).
//
// At v1.0 P4-A:
//   - SLUB-allocated. SLUB freelist write at free clobbers `magic` at
//     offset 0; subsequent dereference of a freed Spoor extincts on
//     the magic check (use-after-free defense, mirroring burrow.c).
//   - Refcount + per-Spoor spinlock. Single-CPU at v1.0; the lock is
//     here so the SMP-safe refcount upgrade (Phase 5+) doesn't need a
//     struct change.
//   - The Walkqid pattern is preserved verbatim from Plan 9 — `walk`
//     returns a Walkqid carrying the resulting Spoor + the qids of the
//     successfully-walked path components (variable-length tail).
//
// State invariant (v1.0 single-CPU):
//   alive iff (ref > 0)
//   ref tracks BOTH handle holders AND in-flight walk/open/read paths
//   (any code path that has a `struct Spoor *` it intends to dereference
//   holds a ref; the ref is released by spoor_unref or spoor_clunk).
//
// Phase 4+ extensions:
//   - Spoor identity for 9P-mounted namespaces (the dev becomes the 9P
//     client; aux carries the session + fid).
//   - Spoor refcount becomes atomic when the syscall surface goes SMP.
//
// Naming (post-rename, see handoff 024):
//   - Plan 9 `Chan` -> Spoor (animal-track word, follows the walk verb).
//   - Plan 9 `Walkqid` kept (no thylacine-themed alternative reads
//     better than the standard term — it's a 9P concept).

#ifndef THYLACINE_SPOOR_H
#define THYLACINE_SPOOR_H

#include <thylacine/spinlock.h>
#include <thylacine/types.h>

struct Dev;
struct Spoor;

// Plan 9 9P qid — identity of a path within a single Dev. Wire format
// is 1 byte type + 4 bytes vers + 8 bytes path (13 bytes). The in-kernel
// struct adds padding so it's pointer-aligned for cheap copy.
struct Qid {
    u64 path;       // unique within the dev
    u32 vers;       // file version (modify counter)
    u8  type;       // QTDIR | QTAPPEND | QTFILE | ...
    u8  pad[3];
};

_Static_assert(sizeof(struct Qid) == 16,
               "struct Qid pinned at 16 bytes (8 path + 4 vers + 1 type + 3 pad)");

// 9P qid type bits per 9P2000.
#define QTDIR     0x80      // is a directory
#define QTAPPEND  0x40      // append-only
#define QTEXCL    0x20      // exclusive use (only one open allowed)
#define QTAUTH    0x08      // authentication file
#define QTTMP     0x04      // not-backed-up temporary file
#define QTFILE    0x00      // plain file

// Spoor flag bits.
#define COPEN     (1u << 0)    // open() has succeeded; offset is meaningful
#define CMSG      (1u << 1)    // message-style (read returns whole records)
#define CWALKONLY (1u << 3)    // #81: a T_OPATH (O_PATH) navigation handle -- NOT
                               // opened for byte I/O; sys_read/write/readdir reject
                               // it (-1) so the perm_check-exempt O_PATH open is not
                               // a read-bypass (IDENTITY-DESIGN section 9.4 #81)
#define CSRVCLIENT (1u << 2)   // devsrv byte-conn Spoor: CLIENT endpoint (read s2c
                               // / write c2s, the mirror of the server endpoint).
                               // Set on the Spoor devsrv_open returns for a byte-
                               // mode connect; clear = server endpoint (from
                               // SYS_SRV_ACCEPT, read c2s / write s2c). stalk-3b-β.

// SPOOR_MAGIC — sentinel set at spoor_alloc; checked at spoor_ref /
// spoor_unref / spoor_clunk. SLUB's freelist write at free clobbers
// offset 0; subsequent operations on a freed Spoor see magic != SPOOR_MAGIC
// and extinct cleanly.
#define SPOOR_MAGIC 0x53504F4F52BAD2EAULL    // 'SPOOR\0' || 0xBA 0xD2 0xEA

struct Spoor {
    u64           magic;       // SPOOR_MAGIC; clobbered by SLUB on free
    int           dc;          // matches dev->dc; cached for cheap dispatch
    u32           devno;       // Plan 9 Chan.dev: per-instance device number.
                               // qid.path is unique only WITHIN a (dc, devno)
                               // pair -- e.g. every dev9p session shares dc='9'
                               // but gets a distinct devno (spoor_next_devno),
                               // so two attach sessions' roots (both qid.path=0)
                               // do not collide. Static single-instance Devs
                               // (devramfs/devsrv/devproc/...) leave it 0.
                               // The mount table keys on (dc, devno, qid.path)
                               // -- the full Plan 9 (type, dev, qid) identity.
    struct Dev   *dev;         // back-pointer; set at spoor_alloc

    struct Qid    qid;         // path identity within the (dev, devno)

    spin_lock_t   lock;        // per-Spoor lock; reserved for Phase 5+ SMP
    int           ref;         // refcount; spoor_alloc sets to 1
    u32           flag;        // COPEN | CMSG
    int           mode;        // omode after open() succeeds; 0 pre-open

    s64           offset;      // read/write cursor; advanced on read/write

    void         *aux;         // dev-private state; opaque to spoor.c
};

_Static_assert(__builtin_offsetof(struct Spoor, magic) == 0,
               "magic must be at offset 0 — SLUB freelist write on free "
               "clobbers it (use-after-free defense)");

// Walkqid: the result of `dev->walk` — carries the Spoor positioned at
// the deepest successful step plus the qids of every step that walked
// successfully. Plan 9 idiom; preserved verbatim per ARCH §9.2.
//
// The flexible array `qid[]` is a flat tail allocated alongside the
// Walkqid itself; total size is `sizeof(struct Walkqid) + nqid *
// sizeof(struct Qid)`.
struct Walkqid {
    struct Spoor *spoor;       // Spoor at the deepest successful step
    int           nqid;        // count of qids in qid[] (0 if walk failed at first step)
    struct Qid    qid[];       // flexible array
};

// Bring up the spoor subsystem. Allocates the SLUB cache. Idempotent
// guard extincts on re-call. Must run after slub_init.
void spoor_init(void);

// SLUB-allocate a fresh Spoor backed by `d`. ref starts at 1 (the caller's
// reference), magic set, dc cached from d->dc, qid zeroed, flag/mode/offset
// zeroed, aux NULL, lock initialized. Returns NULL on:
//   - d == NULL
//   - SLUB OOM
//
// The caller is the sole holder of the returned ref; spoor_unref or
// spoor_clunk drops it.
struct Spoor *spoor_alloc(struct Dev *d);

// Increment ref. Maps to "another consumer is taking the Spoor"
// (handle_dup of a KOBJ_SPOOR; Phase 4 9P client tag holders).
//
// Extincts on NULL / corrupted magic / ref <= 0 (UAF defense).
void spoor_ref(struct Spoor *c);

// Decrement ref. If ref reaches 0, free the Spoor (releases SLUB slot;
// magic is clobbered).
//
// Extincts on corrupted magic or ref <= 0. NULL is a safe no-op (mirrors
// burrow_unref).
//
// IMPORTANT: after the unref that drops ref to 0, `c` is INVALID.
void spoor_unref(struct Spoor *c);

// Clone: allocate a fresh Spoor copying dc / dev / qid / flag / mode /
// offset / aux from `c`. ref of the new Spoor starts at 1; `c`'s ref is
// unchanged. Used by `dev->walk` when it needs an independent cursor for
// the new position.
//
// Returns NULL on:
//   - c == NULL or corrupted
//   - SLUB OOM
//
// At v1.0 P4-A the clone is a shallow field copy. Devs that hold an aux
// pointing to ref-counted state (e.g., a 9P fid) MUST take their own ref
// inside dev->walk before populating the new Spoor's aux; spoor_clone
// itself does NOT touch aux semantics.
struct Spoor *spoor_clone(struct Spoor *c);

// Mint a fresh per-instance device number (Plan 9 Chan.dev) for a
// multi-instance Dev's attach. Monotonic from 1; 0 is the static
// single-instance default. dev9p stamps each attach session's root Spoor
// with one of these so the mount table's (dc, devno, qid.path) key
// distinguishes two concurrent 9P sessions whose roots both have qid.path 0.
u32 spoor_next_devno(void);

// Clunk: the canonical "I'm done with this Spoor" entry. Calls dev->close
// (if non-NULL and the Spoor was opened) before dropping the caller's ref
// via spoor_unref.
//
// At v1.0 P4-A `dev->close` is invoked unconditionally (devnone's close
// is a no-op; per-Dev close hooks decide what to do based on flag &
// COPEN). After the call returns the Spoor pointer is INVALID.
//
// NULL is a safe no-op.
void spoor_clunk(struct Spoor *c);

// Cumulative diagnostic counters. Tests use these to verify lifecycle
// transitions:
//   - spoor_total_allocated increments on every successful spoor_alloc /
//     spoor_clone.
//   - spoor_total_freed increments on every actual SLUB free.
//   - At any state, (allocated - freed) == live Spoor count.
u64 spoor_total_allocated(void);
u64 spoor_total_freed(void);

// =============================================================================
// Walkqid allocation (P4-C — devs that implement non-trivial walks).
// =============================================================================
//
// walkqid_alloc: kmalloc a Walkqid + room for `max_qids` qids in the
// flexible array. Returns NULL on OOM. The caller fills in `spoor` +
// `nqid` + `qid[]` and returns it from `dev->walk`. The walk caller
// owns the Walkqid memory and must walkqid_free when done.
//
// Devs that always succeed in O(1) walk (single-file leaves like devnull)
// don't use this — they return NULL from walk to signal "no walking
// supported". Devs with a directory namespace (devproc, future devctl,
// devramfs) use walkqid_alloc.
struct Walkqid *walkqid_alloc(int max_qids);
void            walkqid_free(struct Walkqid *w);

// spoor_stat_native — fetch a Spoor's native metadata via its Dev's
// stat_native vtable slot (the perm_check / fstat companion). Returns 0 on
// success (out populated), -1 if `c`/`out` is NULL or the Dev has no
// stat_native. Shared by the syscall handlers (fstat / walk-open / wstat /
// open) and the stalk resolver (the per-component X-search stat fetch).
struct t_stat;
int spoor_stat_native(struct Spoor *c, struct t_stat *out);

#endif  // THYLACINE_SPOOR_H
