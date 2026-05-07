// Dev vtable + bestiary registry — the kernel device model (P4-A).
//
// Per ARCHITECTURE.md §9.2. Every kernel device implements `struct Dev`,
// the verbatim Plan 9 vtable (with C99 const-correct typing). At boot,
// each Dev's `init` is called once; thereafter, attach/walk/open/read/
// write/close drive the device's namespace via Spoors.
//
// Naming (post-rename, see handoff 024):
//   - Plan 9 `Chan` -> Spoor.
//   - Plan 9 `devtab[]` -> bestiary[] (medieval-fauna catalog; on-brand
//     for the marsupial OS).
//   - The `Dev` typename + Walkqid kept verbatim (the conceptual fit is
//     already perfect; renaming would be churn).
//
// At v1.0 P4-A:
//   - bestiary[] is a fixed-size sentinel-terminated array. Devs are
//     registered by dev_register(). Order of registration is the boot
//     init order.
//   - dev_init() walks the bestiary calling each ->init(). Registered
//     devs whose init is NULL are skipped.
//   - dev_lookup_by_dc / dev_lookup_by_name are linear scans (bestiary
//     fits in cache; v1.0 has < 16 devs, lookup is rare).
//   - devnone is registered first by dev_init; it's the no-op stub for
//     unconfigured Spoors and an audit guard for "you forgot to attach
//     to a real Dev" bugs.
//
// Phase 4+ extensions (per ROADMAP §6.1):
//   - kernel-internal Devs land in P4-B (cons / null / zero / random),
//     P4-C (proc), P4-D (ctl), P4-E (ramfs).
//   - userspace-driver Devs (virtio-blk / virtio-net / virtio-input /
//     virtio-gpu) land in P4-I/J/K/L; they implement the vtable in
//     terms of 9P round-trips to a userspace driver process.

#ifndef THYLACINE_DEV_H
#define THYLACINE_DEV_H

#include <thylacine/types.h>

struct Spoor;
struct Walkqid;
struct Block;       // 9P-style block I/O carrier; defined when bread/bwrite-using devs land

// The Plan 9 Dev vtable. ARCH §9.2 verbatim with C99 const additions on
// input strings (read-only inputs that were `char *` in 9front; we
// preserve the original typing for buffer params + `void *`/`u8 *` for
// I/O regions).
struct Dev {
    int          dc;     // device character ('-' for none, 'c' for cons, ...)
    const char  *name;   // human-readable name; matches bestiary entry

    // Lifecycle: called by the kernel.
    //   reset()    — re-initialize the dev (Plan 9 `dev->reset`);
    //                kernel-driven on hardware reset.
    //   init()     — called once by dev_init() during boot.
    //   shutdown() — called at orderly shutdown.
    void   (*reset)(void);
    void   (*init)(void);
    void   (*shutdown)(void);

    // Namespace ops.
    //   attach(spec) — produce a Spoor for the root of this dev's
    //                  namespace. spec is dev-specific (e.g., partition
    //                  number for a disk dev). Returns NULL on failure.
    //   walk(c, nc, name, nname) — advance from c through the path
    //                              components. Either reuses nc OR
    //                              ignores it and returns a fresh Spoor
    //                              inside the Walkqid. Walkqid->nqid
    //                              tells the caller how many components
    //                              succeeded.
    //   stat(c, dp, n) — write 9P stat to dp[0:n]; returns bytes
    //                    written or -1 on failure.
    struct Spoor   *(*attach)(const char *spec);
    struct Walkqid *(*walk)(struct Spoor *c, struct Spoor *nc,
                            const char **name, int nname);
    int             (*stat)(struct Spoor *c, u8 *dp, int n);

    // Open / create / close.
    //   open(c, omode) — transition c from "walked" to "opened". Returns
    //                    c on success (typically with c->flag |= COPEN).
    //   create(...)    — create a new file in c's directory.
    //   close(c)       — release any per-Spoor resources held while open.
    //                    Called by spoor_clunk on its way to spoor_unref.
    struct Spoor *(*open)(struct Spoor *c, int omode);
    void          (*create)(struct Spoor *c, const char *name, int omode, u32 perm);
    void          (*close)(struct Spoor *c);

    // I/O.
    //   read / write   — byte-stream I/O at offset.
    //   bread / bwrite — block-style I/O carrying a struct Block (used
    //                    by network + 9P transport devs).
    long           (*read)(struct Spoor *c, void *buf, long n, s64 off);
    struct Block  *(*bread)(struct Spoor *c, long n, s64 off);
    long           (*write)(struct Spoor *c, const void *buf, long n, s64 off);
    long           (*bwrite)(struct Spoor *c, struct Block *bp, s64 off);

    // Admin.
    //   remove — delete the file represented by c.
    //   wstat  — modify metadata (mode, length, atime/mtime, etc.).
    //   power  — set device power state (Plan 9 ARM-laptop heritage; we
    //            keep the slot for runtime PM hooks Phase 5+).
    void          (*remove)(struct Spoor *c);
    int           (*wstat)(struct Spoor *c, u8 *dp, int n);
    struct Spoor *(*power)(struct Spoor *c, int on);
};

// Bestiary (devtab) — sentinel-terminated array of registered Devs.
// Filled by dev_register(); the sentinel (NULL pointer) marks the end
// of the table. Iterators stop at the first NULL.
//
// At v1.0 P4-A the size is bounded — adding a Dev beyond the cap is a
// boot-time extinction. The cap is generous (32) for v1.0's expected
// 7-12 devs (cons, null, zero, random, proc, ctl, ramfs + ~5 driver
// classes) plus headroom.
#define BESTIARY_MAX 32

extern struct Dev *bestiary[];        // sentinel-terminated; size <= BESTIARY_MAX + 1

// devnone — the no-op stub Dev (dc='-'). All ops are safe no-ops or
// graceful failures (-1 / NULL). Anchors Spoors that haven't been
// attached to a real Dev yet, and serves as an audit guard: any Spoor
// observed with dev == &devnone in production code is a bug (you forgot
// to attach).
extern struct Dev devnone;

// Kernel-internal trivial Devs (P4-B): single-file leaf Devs that
// expose one resource each. The dc characters are unique kernel-wide.
extern struct Dev devcons;        // dc='c'  — UART console (write-only at v1.0)
extern struct Dev devnull;        // dc='0'  — bit bucket (writes consumed; reads return 0/EOF)
extern struct Dev devzero;        // dc='z'  — produces zeroes
extern struct Dev devrandom;      // dc='r'  — CSPRNG (RNDR-backed at v1.0)

// =============================================================================
// Shared helpers for leaf-file Devs (P4-B).
// =============================================================================
//
// Most kernel-internal Devs at v1.0 are single-file leaves: one Spoor
// per opener; no walk; open succeeds for any mode; close releases the
// COPEN flag. Devs implement the read/write semantics that matter and
// inherit the lifecycle plumbing from these helpers.
//
// dev_simple_attach: spoor_alloc + populate qid (path=0, vers=0, type
// = the dev's qtype — typically QTFILE for a single-file leaf).
//
// dev_simple_open: mark COPEN + record the omode. Returns c on success,
// NULL if c is NULL.
//
// dev_simple_close: clear COPEN. Per-Dev close hooks that need to
// release aux state should call dev_simple_close last.
struct Spoor *dev_simple_attach(struct Dev *d, u8 qtype);
struct Spoor *dev_simple_open(struct Spoor *c, int omode);
void          dev_simple_close(struct Spoor *c);

// Bring up the device subsystem. Sequence:
//   1. spoor_init()             — SLUB cache for struct Spoor.
//   2. dev_register(&devnone)   — devnone is always first.
//   3. (future P4-B+ devs register themselves before this point or via
//      their own init shims called from dev_init.)
//   4. Walk bestiary[] calling each non-NULL dev->init().
//
// Idempotent guard extincts on re-call. Must run after slub_init.
void dev_init(void);

// Append `d` to bestiary[]. Extincts on:
//   - d == NULL
//   - d->dc collides with an already-registered dev
//   - d->name collides with an already-registered dev
//   - bestiary[] would exceed BESTIARY_MAX entries
//
// Returns the index where the dev was placed (>= 0). Lookups by dc and
// by name will find it after this call returns.
int dev_register(struct Dev *d);

// Look up a registered Dev by its device character. Returns NULL if no
// dev matches. Linear scan; v1.0 cost is negligible (< 16 entries).
struct Dev *dev_lookup_by_dc(int dc);

// Look up a registered Dev by its name (string-equality). Returns NULL
// on no match.
struct Dev *dev_lookup_by_name(const char *name);

// Number of Devs registered (excluding the sentinel). Tests use this to
// verify boot-time registration count.
int dev_count(void);

#endif  // THYLACINE_DEV_H
