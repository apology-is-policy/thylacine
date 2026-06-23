#ifndef THYLACINE_ENV_H
#define THYLACINE_ENV_H

#include <thylacine/spinlock.h>
#include <thylacine/types.h>

// Per-Proc environment group (ARCH section 9.7 / G15) -- the Plan 9 Egrp idiom.
// A table of name->value pairs surfaced as the per-Proc /env directory (devenv,
// dc='E'). Thylacine's native ABI passes no Unix envp at spawn, so the
// environment is a namespace object: write /env/NAME to set, read /env/NAME to
// get, readdir /env to list. A spawned child inherits a COPY via env_clone_into
// (the Plan 9 default-copy-on-rfork, like territory_clone); the reserved RFENVG
// (share semantics) stays deferred, so ref is always 1 at v1.0.
//
// The Env is lazily allocated (a Proc with no environment carries env == NULL,
// read as empty). Every access takes env->lock -- peer threads of a Proc share
// one p->env (the multi-thread-shared-state discipline, like the handle table
// and Territory; Go drives many M-threads through one Proc). Names are inline
// (no per-name allocation); values are kmalloc'd individually (so the Env struct
// stays modest and a long value is not a large contiguous allocation). Each live
// entry carries a MONOTONIC id (assigned at create, never reused within an Env):
// a /env/NAME Spoor's qid.path is that id, and every Dev op re-resolves
// id->entry under the lock, so a variable removed between walk and read fails
// clean rather than resolving to a different variable (the net-3d slot-reuse
// discipline, applied preemptively).

#define ENV_NAME_MAX     64        // var name incl. the NUL
#define ENV_VALUE_MAX    4096      // var value, raw bytes (kmalloc'd; DoS floor)
#define ENV_MAX_ENTRIES  64        // per-Proc cap (DoS floor; composes I-32)

struct EnvEntry {
    u64   id;                       // monotonic; 0 == free slot (never reused)
    char *value;                    // kmalloc'd, len bytes; NULL iff len == 0
    u32   len;                      // value length (<= ENV_VALUE_MAX)
    char  name[ENV_NAME_MAX];       // NUL-terminated
};

struct Env {
    int              ref;           // 1 at v1.0 (RFENVG sharing deferred); accessed
                                    // via __atomic_* (the territory_ref/unref pattern)
    spin_lock_t      lock;          // serializes every access
    u64              next_id;       // monotonic id source (>= 1; never reuses)
    int              count;         // live entries (the DoS bound)
    struct EnvEntry  entries[ENV_MAX_ENTRIES];
};

struct Proc;

// --- lifecycle (proc.c) -----------------------------------------------------
// env_clone_into deep-copies parent's env into child (fresh ref=1); a NULL
// parent->env leaves child->env NULL (empty). Returns 0 on success, -1 on OOM
// (child->env stays NULL -> the proc_free rollback's env_free is a clean no-op).
// env_free drops the ref + frees on the last drop (every value + the struct);
// NULL-tolerant; clears p->env so a second call no-ops.
int  env_clone_into(struct Proc *child, struct Proc *parent);
void env_free(struct Proc *p);

// --- per-name ops (devenv) --------------------------------------------------
// Each resolves p->env (lazily allocating only on the create path) and takes
// env->lock internally; all are sound under concurrent peer-thread access.
//
//   env_lookup   name -> id (0 if absent / no env). The walk step.
//   env_create   create an empty entry (or return the existing id); 0 on
//                failure (OOM / bounds / bad name). The SYS_WALK_CREATE step.
//   env_truncate drop the value (len=0); for OTRUNC on open/create.
//   env_read     copy value[off, off+n) into buf; bytes copied, 0 at/after EOF,
//                or -1 if the entry is gone (the monotonic-id guarantee).
//   env_write    place buf[0, n) at value[off, off+n), extending len; bytes
//                written, or -1 (entry gone / would exceed ENV_VALUE_MAX / OOM).
//   env_unset    remove by name; true if removed.
//   env_iter     the readdir step -- the live entry with the smallest id
//                strictly greater than after_id (so cookies strictly increase);
//                copies its name; true if one exists.
u64  env_lookup(struct Proc *p, const char *name, u32 name_len);
u64  env_create(struct Proc *p, const char *name, u32 name_len);
void env_truncate(struct Proc *p, u64 id);
long env_read(struct Proc *p, u64 id, s64 off, void *buf, long n);
long env_write(struct Proc *p, u64 id, s64 off, const void *buf, long n);
bool env_unset(struct Proc *p, const char *name, u32 name_len);
bool env_iter(struct Proc *p, u64 after_id, u64 *out_id, char *name_out,
              u32 name_cap, u32 *name_len_out);

#endif /* THYLACINE_ENV_H */
