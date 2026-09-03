// stalk -- the per-Proc multi-component pathname resolver (Plan 9 `namec`,
// renamed for the Thylacine bestiary: the predator stalks its quarry along a
// path through the namespace to the target Spoor).
//
// Binding design: docs/STALK-DESIGN.md (signed off 2026-06-02). Invariant I-28
// (ARCHITECTURE.md section 28): path-resolution containment + per-component
// X-search + mount-cross permission. stalk-2 adds Plan 9 `domount`: after
// resolving a component, stalk crosses to the mounted tree iff the resolved
// Spoor's (dc, devno, qid.path) identity matches a mount-table entry
// (territory.c::mount_lookup). Crossing is "on descent" -- a Spoor is crossed
// the moment it is used as a directory to walk through, and the quarry is
// crossed at the end (so opening a mount point opens the mounted root), EXCEPT
// under STALK_MOUNT. stalk-3 adds the namespace-resident /srv consumer.
//
// Vocabulary (user-approved thematic names):
//   - stalk  : the resolver.
//   - trail  : the in-call stack of resolved Spoors the resolver follows and
//              that `..` pops back along (bounded at `start` -- the chroot/pivot
//              boundary -- so `..` can never escape; I-28).
//   - quarry : the target Spoor the resolver returns.
//
// Lifetime: every resolved Spoor on the trail is an owned clone (its own fid
// for dev9p; qid-only for devramfs); on return the resolver clunks every trail
// entry EXCEPT the quarry, which carries the caller's ref. `start` is BORROWED
// (the caller owns it -- a handle's Spoor or the Territory root_spoor) and is
// never reffed or clunked.

#ifndef THYLACINE_STALK_H
#define THYLACINE_STALK_H

#include <thylacine/types.h>

struct Proc;
struct Spoor;
struct t_stat;   // <thylacine/syscall.h>; the stalk_stat metadata sink

// amode -- what stalk does at the final (quarry) component.
#define STALK_WALK  0   // resolve only; do NOT open (the O_PATH / walkable-base
                        // case -- a navigation / chroot target). The quarry IS
                        // crossed (a walked mount point yields the mounted
                        // root). A create PARENT uses STALK_CREATE instead (the
                        // union create-member selection differs).
#define STALK_OPEN  1   // resolve + Dev.open(quarry, omode) (the byte-I/O case).
                        // The quarry is crossed before opening.
#define STALK_MOUNT 2   // resolve to the mount point's OWN identity (the final
                        // component is NOT crossed) + do NOT open. SYS_MOUNT /
                        // SYS_UNMOUNT use this so MREPL re-keys the same
                        // underlying mount point even when it already hosts a
                        // mount (Plan 9 Amount). Intermediate components still
                        // cross normally (you can mount onto /a/b where /a is
                        // itself a mount).
#define STALK_STAT  3   // resolve for METADATA only (POUNCE; SYS_STAT): like
                        // STALK_WALK (quarry crossed, never opened), but when
                        // the final run resolves via Dev.walk_attrs the leaf's
                        // attrs return in the fused reply and NO quarry Spoor /
                        // fid is ever materialized (the walk-QUERY form -- the
                        // 1-RPC stat). Callers use stalk_stat(); passing
                        // STALK_STAT to stalk()/stalk_err() (no stat sink)
                        // degrades to STALK_WALK behavior.
#define STALK_CREATE 4  // resolve a create PARENT (Plan 9 Acreate). Identical to
                        // STALK_WALK -- resolve-only, no open, quarry crossed --
                        // EXCEPT at a UNION final quarry: it crosses to the
                        // FIRST MCREATE (writable) member instead of member 0,
                        // so a create lands in the union's writable mount
                        // (ARCH 9.5); a union with NO MCREATE member fails
                        // -T_E_ACCES (no writable target). A non-union parent
                        // crosses to the mounted root exactly as STALK_WALK.
#define STALK_REMOVE 5  // resolve a REMOVE PARENT (unlink / rmdir / rename
                        // source). Identical to STALK_WALK EXCEPT at a
                        // UNION final quarry, where it returns the mount
                        // point UNCROSSED (like STALK_MOUNT): the caller
                        // then selects the member that HOLDS the leaf
                        // (stalk_union_member_holding), so a remove acts on
                        // the entry's own member, not member 0 or the
                        // MCREATE member (UM-7 F3). A non-union parent
                        // crosses to the mounted root exactly as STALK_WALK.

// amode FLAG (OR'd into one of the four base amodes above; DISTRO D-1):
// do NOT follow a symlink at the FINAL component. Intermediate symlinks are
// ALWAYS followed (they are directory positions). The POSIX no-follow ops:
// lstat (the phenotype AT_SYMLINK_NOFOLLOW), open(O_NOFOLLOW), and the mount
// POINT (STALK_MOUNT implies this flag internally -- the no-cross-final
// precedent extends to no-follow-final). A TRAILING SLASH on the path
// OVERRIDES the flag (POSIX 4.13: "link/" names the directory the link
// resolves to, so following is forced -- measured Linux behavior, including
// under O_NOFOLLOW). Disposition of an un-followed final link by base amode:
//   STALK_OPEN  -> T_E_LOOP (Linux O_NOFOLLOW: cannot open a symlink).
//   STALK_WALK  -> the link ITSELF is the quarry (qid carries QTSYMLINK).
//   STALK_STAT  -> the link's own record (the lstat shape).
//   STALK_MOUNT -> the link's own identity is the mount point.
// Any OTHER bit outside (STALK_AMODE_MASK | STALK_NOFOLLOW) is rejected
// LOUDLY (the stalk-1 F1 amode-guard discipline).
#define STALK_NOFOLLOW   0x100
#define STALK_AMODE_MASK 0xFF

// Total symlink expansions permitted per resolution (Linux SYMLOOP parity;
// the POSIX floor is 8). Exceeded -> T_E_LOOP. Cycles terminate here: the
// resolver never marks visited nodes, exactly like Linux -- a loop simply
// burns the budget.
#define STALK_MAX_FOLLOWS 40

// Trail depth cap: the maximum number of path components stalk resolves. An
// over-deep path (including a '..'-heavy path that pushes past the cap before
// popping) fails cleanly rather than overflowing the fixed trail array.
#define STALK_MAX_DEPTH 40

// stalk -- resolve `path` (`pathlen` bytes, NUL-free; the caller has already
// copied it from user space and rejected embedded NUL) from `start` to a target
// Spoor.
//
//   p        : the calling Proc (for the per-component perm_check; the handler
//              passes current_thread()->proc, a test passes a synthetic Proc).
//   start    : the base Spoor -- BORROWED. The handler selects it: the
//              Territory root_spoor for an absolute walk, or a dirfd's Spoor for
//              a relative one. stalk never refs or clunks it.
//   path     : the path, '/'-separated. Empty components (leading '/', '//')
//              collapse; "." is a no-op; ".." pops the trail (contained at
//              `start`). Each real component is <= SYS_WALK_OPEN_NAME_MAX bytes.
//   amode    : STALK_WALK or STALK_OPEN.
//   omode    : the Plan 9 open mode (OREAD/OWRITE/ORDWR/OEXEC + OTRUNC); used
//              for the final-hop perm_check and Dev.open under STALK_OPEN.
//
// Returns the resolved Spoor (the quarry; ref == 1, opened iff STALK_OPEN) or
// NULL on any failure (missing component, permission denied, depth overflow,
// OOM, open failure). The caller installs the handle and derives its rights.
struct Spoor *stalk(struct Proc *p, struct Spoor *start,
                    const char *path, u64 pathlen, int amode, u32 omode);

// stalk_err -- the errno-aware core (the errno-rollout arc; ERRORS.md). Identical
// to stalk(), but on a NULL return writes the cause to *errp (OPTIONAL -- may be
// NULL) as a POSITIVE T_E_<NAME> code: T_E_NOENT (missing component), T_E_ACCES
// (perm_check denial), T_E_INVAL (structural reject), or a propagated / T_E_IO
// otherwise. NEVER T_E_PERM (== 1, which collides with the generic -1 sentinel).
// On a non-NULL return *errp is unspecified. The caller returns -*errp so a
// missing path surfaces as -T_E_NOENT (Go os.IsNotExist) instead of the bare -1
// (which Go's Linux-shaped decode renders EPERM). stalk() == stalk_err(...,NULL).
struct Spoor *stalk_err(struct Proc *p, struct Spoor *start,
                        const char *path, u64 pathlen, int amode, u32 omode,
                        int *errp);

// stalk_exec (VIVARIUM section 13) -- stalk_err plus a phenotype report: on
// success *crossed_pheno is set true iff the resolution crossed an MPHENO_LINUX
// mount (the /viv/bin subtree scope; the section-12.1 rule-1 second declaration
// channel). The caller inits *crossed_pheno = false; a failed walk leaves it
// untouched (fail-safe -> native). Only the exec resolver uses this.
struct Spoor *stalk_exec(struct Proc *p, struct Spoor *start,
                         const char *path, u64 pathlen, int amode, u32 omode,
                         int *errp, bool *crossed_pheno);

// stalk_stat -- resolve `path` and fill *out with the LEAF's metadata without
// installing anything (POUNCE; the SYS_STAT core). The X-search is identical
// to a STALK_WALK resolution (POSIX stat authority = the path X-search only;
// the leaf's own R/W are irrelevant). On the fast path (the final run's Dev
// implements walk_attrs and the leaf is not a mount point) NO quarry Spoor or
// fid is ever created -- the attrs arrive fused with the walk. Fallback paths
// (Dev without walk_attrs / leaf mount point / zero-component path) resolve a
// quarry, stat_native it, and clunk it -- today's exact O_PATH+fstat shape.
//
// `flags` (D-1): 0 = follow a final symlink (POSIX stat); STALK_NOFOLLOW =
// the lstat shape (the final link's OWN record -- on the fused fast path the
// leaf record IS it, zero extra cost). Any other bit -> T_E_INVAL.
//
// Returns 0 (out filled) or -1 with the cause in *errp (OPTIONAL; same codes
// as stalk_err).
int stalk_stat(struct Proc *p, struct Spoor *start,
               const char *path, u64 pathlen, u32 flags,
               struct t_stat *out, int *errp);

// stalk_cross_mounts -- Plan 9 `domount`, exposed for the single-hop walk
// syscalls (SYS_WALK_OPEN) so they cross mounts identically to stalk()/SYS_OPEN.
// Tests `probe`'s (dc, devno, qid.path) identity against `p`'s mount table; if
// it is a mount point, mints an INDEPENDENT clone-walk of the mounted source and
// follows a mount-over-mount chain to the leaf. `probe` is NOT consumed -- the
// caller decides whether to clunk it.
//
//   *out == NULL, return 0 : probe is not a mount point (no crossing).
//   *out != NULL, return 0 : crossed; *out is OWNED (caller clunks it).
//   return -1              : probe IS a mount point but minting the crossed
//                            Spoor failed; *out == NULL; probe still owned.
// VIVARIUM section 13: `crossed_pheno` is a SET-ONLY accumulator -- true if this
// cross (or any hop of a mount-over-mount chain) went through an MPHENO_LINUX
// mount. The caller inits it false; NULL for callers that do not resolve for exec.
int stalk_cross_mounts(struct Proc *p, struct Spoor *probe, struct Spoor **out,
                       bool *crossed_pheno);

// stalk_union_has_child (UM) -- does the directory `dir` (a crossed member root)
// contain an entry named `name` (length `namelen`, NOT NUL-terminated)? A raw
// Dev.walk existence probe: NO permission check and NO symlink expansion (dedup
// is about NAME PRESENCE, not access). Used by the union readdir merge to drop a
// later member's entry whose name an earlier member already provides (first-
// member-wins). Returns false on any miss / walk failure / over-long name.
bool stalk_union_has_child(struct Proc *p, struct Spoor *dir,
                           const char *name, u32 namelen);


// stalk_union_member_holding (UM, UM-7 F3) -- the member of the union at
// `point` (a pre-cross mount point with >= 2 members) that HOLDS the entry
// `leaf` (NUL-terminated): snapshot every member ATOMICALLY, cross each in
// declared order, and return the FIRST whose Dev.walk resolves `leaf` (the
// same first-hit rule as the walk). The returned Spoor is the crossed MEMBER
// ROOT (ref-held, mount-point name transplanted) -- the directory a remove of
// `leaf` must act on. Plan 9 union skip: a member that fails to cross / is not
// a directory / denies X-search / lacks `leaf` is SKIPPED; NULL means no member
// holds it (caller answers -T_E_NOENT). *errp is set only on a clone OOM.
struct Spoor *stalk_union_member_holding(struct Proc *p, struct Spoor *point,
                                         const char *leaf, int *errp);

// stalk_union_create_member (UM) -- the create-target member of the union at
// `point`: the FIRST member (declared order) carrying MCREATE, crossed to its
// leaf root (ref-held, mount-point name transplanted). NULL + *errp==0 means
// no MCREATE member (caller answers -T_E_ACCES); NULL + *errp==T_E_IO means the
// chosen member failed to cross. Exposed (UM-8c) for the fd/rename dest that
// resolves a union point and must route a create through its writable member.
struct Spoor *stalk_union_create_member(struct Proc *p, struct Spoor *point,
                                        int *errp);
#endif // THYLACINE_STALK_H
