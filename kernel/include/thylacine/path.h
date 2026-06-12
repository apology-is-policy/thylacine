// Path -- the refcounted, copy-on-walk namespace name carried by every Spoor
// (#66). The Plan 9 4th-edition `Chan.path`, adapted: the cleaned namespace
// name by which a Spoor was reached (`/srv/stratum`, `/bin/joey`,
// `/proc/3/ns`). See ARCHITECTURE.md section 9.6.9 + invariant I-33.
//
// The Path is STRICTLY NON-LOAD-BEARING (I-33): the resolver appends to it but
// never reads it to make a decision, so a wrong / stale / absent / failed-to-
// allocate Path can change only the cosmetic content of the introspection
// readers (SYS_FD2PATH, /proc/<pid>/fd, /proc/<pid>/ns), never a resolution
// outcome or a permission decision. A path-alloc failure leaves the Path NULL
// and the WALK STILL SUCCEEDS.
//
// Thylacine's Path is Plan 9's MINUS the `mtpt` mount-point history: `stalk`
// resolves `..` lexically against its own in-call trail (contained at
// `root_spoor`, I-28), never against a Path, so the Path is a pure name string.
//
// Lifetime: subordinate to the Spoor's. Every Spoor that references a Path
// holds exactly one ref on it for the Spoor's whole life (NULL at spoor_alloc,
// shared via path_ref at spoor_clone, dropped at spoor_free_internal). A Path
// frees with its last referencing Spoor; shared Paths (across clones) are
// independently refcounted. The string `s` is IMMUTABLE once built (path_addelem
// always allocates a fresh Path, never mutates a shared one), so the only
// concurrently-mutated field is the atomic `ref` -- a Path string is safe to
// read locklessly.

#ifndef THYLACINE_PATH_H
#define THYLACINE_PATH_H

#include <thylacine/types.h>

// struct Path: a refcounted, immutable namespace-name string.
//   ref : atomic refcount; 1 at creation; shared (incref) by spoor_clone.
//         path_ref / path_unref extinct on a <= 0 pre-value (UAF diagnostic,
//         mirroring spoor's ref discipline).
//   len : strlen(s) (cached; excludes the NUL).
//   s   : the cleaned, NUL-terminated namespace path. ALWAYS begins with '/'
//         (paths are rooted). Immutable after creation.
struct Path {
    int  ref;
    u32  len;
    char s[];   // flexible array; NUL-terminated
};

// path_make_root: allocate a Path holding "/" (ref == 1). The seed for a
// Territory's root_spoor. Returns NULL on OOM (the caller leaves the Spoor's
// path NULL = "unknown" -- never fatal).
struct Path *path_make_root(void);

// path_addelem: allocate a NEW Path = parent extended by one cleaned component
// (Plan 9 addelem; copy-on-walk -- NEVER mutates `parent`). ref == 1 (owned by
// the caller). `parent` is borrowed, not consumed.
//
//   parent == NULL                  -> NULL  (unknown stays unknown)
//   name is "."                     -> a fresh copy of parent (a no-op step)
//   name is ".."                    -> parent with its last element popped
//                                      ("/a/b" -> "/a"; "/a" -> "/"; "/" -> "/")
//   otherwise                       -> parent + "/" + name
//                                      (root "/" + "x" -> "/x"; "/a" + "x" -> "/a/x")
//
// Returns NULL on OOM OR if the result would exceed SYS_OPEN_PATH_MAX (the walk
// then proceeds with a NULL "unknown" path -- I-33). `name` is `namelen` bytes;
// it need not be NUL-terminated.
struct Path *path_addelem(const struct Path *parent, const char *name, u64 namelen);

// path_parent: allocate a NEW Path = `p` with its last element popped (the "..""
// case, factored for spoor_path_walked). NULL -> NULL.
struct Path *path_parent(const struct Path *p);

// path_ref / path_unref: atomic refcount. unref frees the storage at 0. Both
// NULL-safe (a NULL Path == "unknown name", a valid state). Extinct on a <= 0
// pre-value (use-after-free / double-free diagnostic).
void path_ref(struct Path *p);
void path_unref(struct Path *p);

// Cumulative diagnostic counters (test discipline, mirroring spoor's):
// (allocated - freed) == the live Path count at any quiescent point.
u64 path_total_allocated(void);
u64 path_total_freed(void);

#endif  // THYLACINE_PATH_H
