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
struct poll_waiter; // <thylacine/poll.h>; the hook a polling thread installs on .poll
struct t_stat;      // <thylacine/syscall.h>; the SYS_FSTAT native metadata record

// Per-call component cap for Dev.walk_attrs (POUNCE). Vtable-level so
// non-9P Devs and the resolver need no wire header; dev9p _Static_asserts
// it equals the wire's P9_MAX_WALK (the Twalkgetattr per-op bound).
#define DEV_WALK_ATTRS_MAX 16

// Distinguished Dev.walk_attrs return: THIS SESSION's backing server does
// not implement the fused op (Twalkgetattr is a Stratum extension; netd and
// any plain 9P2000.L peer answer it Rlerror ENOSYS). The resolver falls back
// to the per-component loop -- correctness identical, RPC count higher.
// Distinct from NULL (a REAL walk failure at the first component). Callers
// must NOT walkqid_free it. dev9p latches the answer per-session, so after
// the one probe RPC the sentinel returns instantly. Native implementations
// (devramfs) never return it.
extern struct Walkqid dev_walk_attrs_unsupported;
#define DEV_WALK_ATTRS_UNSUPPORTED (&dev_walk_attrs_unsupported)

// The Plan 9 Dev vtable. ARCH §9.2 verbatim with C99 const additions on
// input strings (read-only inputs that were `char *` in 9front; we
// preserve the original typing for buffer params + `void *`/`u8 *` for
// I/O regions).
struct Dev {
    int          dc;     // device character ('-' for none, 'c' for cons, ...)
    const char  *name;   // human-readable name; matches bestiary entry

    // A-2d (IDENTITY-DESIGN.md sections 3.7.1 + 9.6): when true, the kernel
    // rwx-permission layer enforces owner/group/other access at walk/open/
    // create for Spoors backed by this Dev (reading mode/uid/gid via
    // .stat_native against the Proc's principal_id + groups). When false the
    // chokepoint skips the rwx check (handle-RIGHT gating only -- the v1.0
    // status quo). devramfs = true (system-owned -> real per-principal
    // enforcement); dev9p = true (A-3b: the host-bake stamps the pool
    // PRINCIPAL_SYSTEM-owned [mkfs --root-uid + stratumd --bake-owner-uid] +
    // SO_PEERCRED carries the connecting principal, so the server-reported uids
    // reconcile with the PRINCIPAL_SYSTEM boot chain -- the boot chain owns the
    // baked tree, so enforcement does not brick).
    bool         perm_enforced;

    // SYS_LSEEK is permitted only on Devs whose read/write HONOR the byte offset
    // (file content with a meaningful position): devramfs + dev9p set this true.
    // Stream / service / control Devs (devsrv conn+registry, devproc, devnotes,
    // devcons, devnone, devctl) leave it false (the zero default) -> lseek returns
    // -1 (POSIX ESPIPE). Was inferred from `.stat_native != NULL` until RW-4 R2-F2:
    // #957 (devsrv) and A-4b (devproc) added .stat_native to non-seekable Devs for
    // fstat, silently regressing lseek to succeed-on-an-unused-offset; an explicit
    // flag decouples "can fstat" from "can seek".
    bool         seekable;

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
    //   stat_native(c, out) — Thylacine-native fstat surface (P6-pouch-
    //                    stratumd-boot sub-chunk 16b-gamma). Fill *out
    //                    with the file's metadata in struct t_stat shape.
    //                    Returns 0 on success, -1 on failure. A NULL slot
    //                    means "no native stat available" — SYS_FSTAT on
    //                    a Spoor backed by such a Dev returns -1. Devs
    //                    that have real file metadata (devramfs in
    //                    particular) implement this; trivial leaf Devs
    //                    (devcons / devnull / devzero / devnotes) leave
    //                    the slot NULL.
    //   wstat_native(c, valid, mode, uid, gid) — Thylacine-native chmod/chown
    //                    surface (A-2a; IDENTITY-DESIGN.md §9.5). Apply the
    //                    subset of (mode, uid, gid) selected by the `valid`
    //                    mask (T_WSTAT_* bits) to the file backed by c.
    //                    Returns 0 on success, -1 on failure. NULL-permitted
    //                    (like .stat_native / .fsync): a NULL slot => SYS_WSTAT
    //                    returns -1. dev9p -> Tsetattr; read-only / metadata-
    //                    less Devs (devramfs at v1.0) leave it NULL. The
    //                    handler has already validated the mask + value bounds;
    //                    the Dev forwards to its backing.
    //                    A Dev exposing this MUST set .perm_enforced = true
    //                    (#47 audit F1): SYS_WSTAT's fd gate is kind-only, so
    //                    perm_wstat_check -- which runs only when
    //                    perm_enforced -- is the ONLY write-authority check on
    //                    the metadata path. dev_register extincts on a
    //                    wstat-capable Dev that does not enforce.
    struct Spoor   *(*attach)(const char *spec);
    struct Walkqid *(*walk)(struct Spoor *c, struct Spoor *nc,
                            const char **name, int nname);
    int             (*stat)(struct Spoor *c, u8 *dp, int n);
    int             (*stat_native)(struct Spoor *c, struct t_stat *out);
    int             (*wstat_native)(struct Spoor *c, u32 valid,
                                    u32 mode, u32 uid, u32 gid);

    // walk_attrs(c, nc, names, name_lens, nname, sts) — the walk-fused getattr
    // (POUNCE; docs/POUNCE-DESIGN.md §4). OPTIONAL (NULL-permitted, like
    // .fsync): a NULL slot means the resolver keeps the per-component
    // walk + stat_native loop — correctness identical, only the RPC count
    // differs. Only Devs whose backing can sample per-step attributes in one
    // operation set it (dev9p -> Twalkgetattr; devramfs -> the RAM table).
    //
    // Walks `nname` REAL components (the resolver never passes "." / "..")
    // from c in ONE operation, filling sts[0..nqid) with each walked
    // component's attributes. Names are (ptr, len) pairs — NOT necessarily
    // NUL-terminated (name_lens defines each extent; every len is
    // 1..SYS_WALK_OPEN_NAME_MAX); 1 <= nname <= DEV_WALK_ATTRS_MAX.
    //
    // The Walkqid contract SHARPENS Dev.walk's reuse-nc rule:
    //   - nc != NULL (the BIND form): nc is the caller's spoor_clone(c). On a
    //     FULL walk (w->nqid == nname) nc is transitioned (own backing fid for
    //     dev9p; qid = the leaf's) and w->spoor == nc. On a PARTIAL walk
    //     (w->nqid < nname) nc is left UNTOUCHED (still shallow-sharing c's
    //     aux — the caller detaches + unrefs it) and w->spoor == NULL: nothing
    //     was bound, nothing to clunk. This mirrors the Twalkgetattr session
    //     rule (new_fid binds only on a full walk).
    //   - nc == NULL (the QUERY form): a pure sample — no Spoor transitions,
    //     no fid binds on either end (dev9p sends newfid=P9_NOFID), and
    //     w->spoor == NULL always. The 1-RPC stat.
    //   - NULL return: the walk failed at the FIRST component (or transport /
    //     OOM). Nothing bound; nc untouched.
    struct Walkqid *(*walk_attrs)(struct Spoor *c, struct Spoor *nc,
                                  const char **names, const size_t *name_lens,
                                  int nname, struct t_stat *sts);

    // open_cached(c, names, name_lens, nname, sts) — the FID-LIFECYCLE fidless
    // cached open (docs/FID-LIFECYCLE-DESIGN.md §3.3; refines I-38). OPTIONAL
    // (NULL-permitted, like .walk_attrs): stalk calls it on the FINAL run of a
    // plain read-only STALK_OPEN resolution (omode == 0 exactly) BEFORE the
    // normal bind walk. Names are (ptr, len) pairs like walk_attrs; 1 <= nname
    // <= DEV_WALK_ATTRS_MAX.
    //
    // The Dev attempts, wholly internally: an RPC-free eligibility hint; ONE
    // FORCED-WIRE query walk of the run (no fid binds on either end — the
    // close-to-open revalidation, which MUST be server-fresh, never served
    // from a cache); a snapshot of the leaf's full content at the fresh
    // content-version; and the mint of an OPENED read-only fidless Spoor.
    //
    //   - Success: returns the opened Spoor (one owned ref; wire-free to
    //     destroy) AND fills sts[0..nname) with the walk's FRESH per-component
    //     records. The CALLER (the resolver) then MUST run its fail-ordering
    //     post-scan on those records — per-component X-search, mount-membership
    //     scan, final-hop R/W perm — and destroy the Spoor on any denial or
    //     mount crossing; permission policy stays in the resolver (I-28/I-22),
    //     never in the Dev.
    //   - NULL: not servable (ineligible mode/type, coverage miss, budget,
    //     stale, OOM, wire failure). Nothing is bound and nothing is revealed
    //     to the caller; sts may be scribbled. The caller proceeds with the
    //     normal walk + open path, whose outcome is the observable one.
    struct Spoor *(*open_cached)(struct Spoor *c, const char *const *names,
                                 const size_t *name_lens, int nname,
                                 struct t_stat *sts);

    // Open / create / close.
    //   open(c, omode) — transition c from "walked" to "opened". Returns
    //                    c on success (typically with c->flag |= COPEN).
    //   create(c, name, omode, perm, gid)
    //                  — create `name` in the directory c (whose fid the
    //                    caller has already clone-walked so c is a private
    //                    cursor at the parent dir), then OPEN it. On success
    //                    c is mutated to refer to the new opened object and
    //                    returned (Plan 9 create semantics; mirrors open's
    //                    return shape). `perm` low 9 bits = mode; the DMDIR
    //                    bit selects a directory. `gid` is the creator's
    //                    primary group, carried into the 9P gid field (the
    //                    Dev treats it as an opaque number -- identity-
    //                    agnostic). Returns NULL on failure (the caller
    //                    spoor_clunks c, which clunks the walked fid).
    //                    A read-only Dev returns NULL.
    //   close(c)       — release any per-Spoor resources held while open.
    //                    Called by spoor_clunk on its way to spoor_unref.
    struct Spoor *(*open)(struct Spoor *c, int omode);
    struct Spoor *(*create)(struct Spoor *c, const char *name, int omode,
                            u32 perm, u32 gid);
    void          (*close)(struct Spoor *c);

    // I/O.
    //   read / write   — byte-stream I/O at offset.
    //   bread / bwrite — block-style I/O carrying a struct Block (used
    //                    by network + 9P transport devs).
    long           (*read)(struct Spoor *c, void *buf, long n, s64 off);
    struct Block  *(*bread)(struct Spoor *c, long n, s64 off);
    long           (*write)(struct Spoor *c, const void *buf, long n, s64 off);
    long           (*bwrite)(struct Spoor *c, struct Block *bp, s64 off);

    // Durability + enumeration (FS-mutation foundation; IDENTITY-DESIGN.md
    // section 9.2). Both are NULL-permitted (like .poll / .stat_native -- NOT
    // among the section-9.2 vtable-coverage 16); only Devs that genuinely back
    // these set them.
    //   fsync(c, datasync) — flush durable state (datasync 0 = full, 1 = data
    //                        only). NULL slot => SYS_FSYNC returns -1 (the Dev
    //                        has no durability barrier). dev9p -> Tsync;
    //                        in-memory-durable Devs (devramfs) implement a
    //                        no-op success.
    //   readdir(c, buf, n, off) — read the next run of 9P2000.L dirents into
    //                        buf at the Spoor's offset; return bytes (0 =
    //                        end-of-directory). NULL slot => SYS_READDIR
    //                        returns -1 (not a readdir-able directory).
    int            (*fsync)(struct Spoor *c, u32 datasync);
    long           (*readdir)(struct Spoor *c, void *buf, long n, s64 off);

    // Rename + unlink (FS-mutation foundation FS-gamma; IDENTITY-DESIGN.md
    // §9.3). Both NULL-permitted (like .fsync / .readdir); only Devs that
    // genuinely back them set them (dev9p at v1.0). The directory Spoors are the
    // caller's looked-up cursors (NOT clone-walked — renameat/unlinkat operate
    // on the dirfid by name without transitioning it, unlike .create).
    //   rename(olddir, oldname, newdir, newname) — move `oldname` under olddir
    //     to `newname` under newdir; an existing destination is atomically
    //     replaced (POSIX rename / 9P Trenameat). olddir + newdir must share the
    //     dev (the SYS_RENAME handler checks) AND, for dev9p, the same session
    //     (dev9p_rename checks). Returns 0 on success, -1 on failure. NULL slot
    //     => SYS_RENAME returns -1.
    //   unlink(parent, name, flags) — remove `name` under parent; flags 0 = a
    //     non-directory, SYS_UNLINK_REMOVEDIR = an empty directory (9P
    //     Tunlinkat). Returns 0 / -1. NULL slot => SYS_UNLINK returns -1.
    int            (*rename)(struct Spoor *olddir, const char *oldname,
                             struct Spoor *newdir, const char *newname);
    int            (*unlink)(struct Spoor *parent, const char *name, u32 flags);

    // Readiness probe — the SYS_POLL plumbing (§23.3; specs/poll.tla).
    //   poll(c, events, pw)
    //     Returns the subset of `events` currently ready on c. If `pw`
    //     is non-NULL, atomically registers it on the object's poll-
    //     hook list under the object's own lock — the load-bearing
    //     register-then-observe step. If `pw` is NULL the call is
    //     sample-only (the post-wake re-scan).
    //
    // A NULL .poll slot means the fd is always ready for the requested
    // events — the POSIX-correct answer for a regular file. Only Devs
    // with genuine readiness state (devpipe at v1.0; devsrv at
    // P5-poll-b) implement a real .poll.
    short         (*poll)(struct Spoor *c, short events, struct poll_waiter *pw);

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
extern struct Dev devfull;        // dc='f'  — full disk (writes fail; reads NUL-fill). P6 sub-chunk 11.
extern struct Dev devrandom;      // dc='r'  — CSPRNG (RNDR-backed at v1.0)
extern struct Dev devnotes;       // dc='n'  — per-Proc note queue (fd-first view). P6 sub-chunk 13a.

// Kernel-internal directory Devs (P4-C+).
extern struct Dev devproc;        // dc='p'  — /proc/<pid>/{status,cmdline,ctl,ns}
extern struct Dev devctl;         // dc='C'  — /ctl/{procs,memory,devices,kernel-base,sched}
extern struct Dev devramfs;       // dc='m'  — /ramfs/<file> from cpio newc initrd
extern struct Dev devdev;         // dc='d'  — /dev char-device directory (#57b)
extern struct Dev devhw;          // dc='H'  — DTB hardware inventory tree (Menagerie devhw)
extern struct Dev devpci;         // dc='P'  — /hw/pci mediated PCI topology (Menagerie 6b)
extern struct Dev devenv;         // dc='E'  — /env per-Proc environment (G15, Go Stage 4a)

// devramfs diagnostics (used by tests).
int  devramfs_file_count(void);
int  devramfs_synth_dir_count(void);   // count of synthetic mount-point dirs (srv/proc/ctl/dev/hw)
bool devramfs_initialized(void);

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
