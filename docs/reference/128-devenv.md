# 128 — devenv: the per-Proc environment device (`/env`, G15)

## Purpose

`devenv` (`dc='E'`, mounted at `/env`) is the per-Proc **environment group** — the
Plan 9 `Egrp`/`devenv` idiom. A Proc's environment is a directory of name->value
files: `read /env/NAME` for the value, `write`/`create` `/env/NAME` to set it,
`readdir /env` to list the names, `unlink /env/NAME` to remove one.

Thylacine's native ABI passes **no Unix `envp`** at spawn — the environment is a
namespace object, not a syscall argument. A spawned child inherits a **deep copy**
of its parent's environment via `env_clone_into` (the Plan 9 default-copy-on-rfork,
exactly as the namespace is cloned by `territory_clone`). The Go runtime reads this
tree at startup (`runtime/env_thylacine.go::goenvs` enumerates `/env` and reads each
value) to populate `os.Environ` / `os.Getenv` — closing the G15 gap with **no envp
ABI** anywhere in the stack.

The mount is **global** (one `devenv`, mounted once in the boot namespace) but every
operation resolves the **calling** Proc's own environment
(`current_thread()->proc->env`), so a Proc sees only its own variables — the
per-Proc-content-behind-a-global-mount shape, exactly like `/proc/self`. This is the
crux of the **I-1** isolation property (below).

Design: `docs/ARCHITECTURE.md` §9.7. Scripture commit `6d47cef`; kernel `002764b`.

## The namespace

```
/env                  the environment root          (QTDIR)
/env/<NAME>           one variable's value file     (QTFILE; raw bytes)
```

`<NAME>` is a single component: non-empty, `< ENV_NAME_MAX` (64) bytes, no `/` and
no embedded NUL. The value is opaque raw bytes up to `ENV_VALUE_MAX` (4096). There
are **no sub-directories** — `create` with `DMDIR` is rejected.

`os.Getenv("GOROOT")` reads `/env/GOROOT`; `os.Setenv` writes it; `os.Environ`
enumerates `/env` (the Go runtime caches the snapshot at startup — see *Semantics*).

## Implementation

The data + lock + monotonic-id discipline live in **`kernel/env.c`** (`struct Env`);
**`kernel/devenv.c`** is the thin `Dev` vtable over the `env_*` primitives.

### Qid encoding

```
qid.path == 0          the /env root directory (QTDIR)
qid.path  > 0          a variable's value file; the path IS the entry's monotonic id (QTFILE)
```

Entry ids start at 1 (`Env.next_id`), so a value file's `qid.path` never collides
with the root's `0`. The id is **monotonic** — assigned at create, **never reused**
within an `Env` (an `unset` frees the slot by zeroing its id but never decrements
`next_id`). Every Dev op re-resolves `id -> entry` under `env->lock`, so a variable
removed between a walk (which mints the id into `qid.path`) and a later read/write
(which resolves it) fails **clean** (`-1`) rather than resolving to a different
variable that happened to reuse the slot. This is the net-3d slot-reuse discipline,
applied preemptively.

### Walk / read / write / readdir / create / unlink

- **walk** (`devenv_walk` → `walk_one`): honours the #57a reuse-`nc` contract — a
  non-NULL `nc` is mutated in place and returned as `wq->spoor` (a 0-element walk
  returns it unchanged with `nqid == 0`, the shape `clone_walk_zero` needs to cross
  the `/env` mount); a NULL `nc` clones (the legacy direct-call shape the kernel
  tests drive). `..` from any entry returns the root. From the root, a name resolves
  via `env_lookup`; from a value file, a child walk fails (a file has no children).
- **read** (`devenv_read` → `env_read`): a value file copies `value[off, off+n)`;
  returns bytes read, `0` at/after EOF, `-1` if the entry is gone. Reading the root
  directory returns `-1` (it enumerates via readdir).
- **write** (`devenv_write` → `env_write`): a value file places `buf[0, n)` at
  `value[off, off+n)`, growing into a fresh `kmalloc`'d buffer (values carry no spare
  capacity). Rejected if `off + n > ENV_VALUE_MAX`. Writing the root returns `-1`.
- **readdir** (`devenv_readdir` → `env_iter` + `emit_dirent`): emits one 9P2000.L
  dirent per live entry, in strictly-increasing-id order. The `cookie` is the entry's
  monotonic id (strictly increasing, never 0 — so it composes with the SYS_READDIR
  handler's #955 non-advancing-cursor EOD guard without ever truncating a real
  listing). Whole entries only; resumes from the last id.
- **create** (`devenv_create` → `env_create`): mints (or returns the existing) entry
  id, then transitions the (handler-cloned, private) parent Spoor into the new value
  file in place (Plan 9 create semantics; mirrors `dev9p_create`). `DMDIR` rejected;
  `OTRUNC` resets the value.
- **open** (`devenv_open`): `OTRUNC` on a value file resets it (the `os.WriteFile` /
  `os.Create` `O_TRUNC` flow); the root has no truncate.
- **unlink** (`devenv_unlink` → `env_unset`): removes by name (`os.Remove`).
  `SYS_UNLINK_REMOVEDIR` rejected (no directories).

There is **no per-Spoor private state** — the value lives in the Proc's `Env`, keyed
by id — so `close` is the trivial `dev_simple_close` (no `aux`, no UAF surface).
`perm_enforced == false`: a Proc's environment is its own; there is nothing to leak
across the per-Proc resolution, so the device is visibility-not-authority (like
`/proc`). The byte buffers reaching `read`/`write`/`readdir` are **kernel** scratch
buffers bounded by `SYS_RW_MAX` (the syscall handlers bounce + validate the user VA),
so `devenv`'s raw byte access is memory-safe with respect to user VAs.

## Data structures (`kernel/include/thylacine/env.h`)

```c
#define ENV_NAME_MAX     64    // var name incl. the NUL
#define ENV_VALUE_MAX    4096  // var value, raw bytes (kmalloc'd; DoS floor)
#define ENV_MAX_ENTRIES  64    // per-Proc cap (DoS floor; composes I-32)

struct EnvEntry {
    u64   id;                  // monotonic; 0 == free slot (never reused)
    char *value;               // kmalloc'd, len bytes; NULL iff len == 0
    u32   len;                 // value length (<= ENV_VALUE_MAX)
    char  name[ENV_NAME_MAX];  // NUL-terminated
};

struct Env {
    int              ref;      // 1 at v1.0 (RFENVG sharing deferred); via __atomic_*
    spin_lock_t      lock;     // serializes every access
    u64              next_id;  // monotonic id source (>= 1; never reuses)
    int              count;    // live entries (the DoS bound)
    struct EnvEntry  entries[ENV_MAX_ENTRIES];
};
```

`struct Proc` carries a `struct Env *env;` at offset 288 (`_Static_assert`s pin the
field offset and `sizeof(struct Proc) == 296`). Names are **inline** (no per-name
allocation); each value is **individually `kmalloc`'d**, so the `Env` struct stays
modest and a long value never needs a large contiguous allocation.

## Lifecycle (`kernel/env.c` + `kernel/proc.c`)

The `Env` is owned **1:1** by its Proc (RFENVG sharing is deferred, so `ref` is
always 1 at v1.0; the field is the forward hook + the `territory_unref`-shaped
double-free guard). It is:

- **lazily allocated** on the first mutation — `env_lazy`'s CAS closes the
  peer-thread alloc race (two peer threads of one Proc both observe `env == NULL` and
  both allocate; the CAS loser `env_destroy`s its own allocation and adopts the
  winner's). A Proc that never sets a variable carries `env == NULL` (read as empty).
- **deep-copied on spawn** — `env_clone_into(child, parent)` (in `rfork_internal`,
  right after `allowance_clone_into`) takes the parent's lock for the **whole** copy
  (so a parent peer-thread mutation cannot interleave a half-copied snapshot),
  per-value `kmalloc`s, copies `next_id` (so the child cannot mint a colliding id),
  and publishes `child->env` with a RELEASE store while the child is not yet running.
  A NULL `parent->env` leaves `child->env` NULL; an OOM mid-copy rolls back via
  `env_destroy(ne)` (which frees the values copied so far + the struct), leaving
  `child->env` NULL.
- **freed at `proc_free`** — `env_free(p)` (after `allowance_free`) clears `p->env`
  (so a second call no-ops), drops the ref, and `env_destroy`s on the last drop
  (frees every entry's value + the struct). A zero-ref drop is an `extinction` (the
  forward double-free guard).

The two `rfork_internal` rollback shapes are both sound:
- **env OOM** → `child->env` stays NULL → the rollback's `proc_free` → `env_free`
  no-ops.
- **a later failure** (`territory_clone` / `thread_create` fails *after*
  `env_clone_into` succeeded) → `proc_free` → `env_free` frees the just-cloned env
  exactly once. Identical to the allowance discipline.

## Invariants + spec cross-reference

`devenv`/`Env` introduce **no new §28 invariant** — they compose existing ones:

- **I-1** (per-Proc isolation): every op resolves `current_thread()->proc->env`. A
  walked `/env/NAME` Spoor carries only a `qid.path` id (no Proc pointer), so even a
  Spoor leaked to Proc B resolves that id against **B's** env — returning B's
  variable or a clean miss, **never** A's. Per-Proc content behind a global mount.
- **I-32** (per-Proc resource floor / DoS bound): `ENV_MAX_ENTRIES` /
  `ENV_NAME_MAX` / `ENV_VALUE_MAX` cap the environment; on a bound the op fails clean
  (`0` / `-1`), never extincts.
- the multi-thread-shared-state discipline (the #844 / #57a-F2 class): peer threads
  of one Proc share one `p->env` (Go drives many M-threads through one Proc), so
  every access takes `env->lock` and every `p->env` load is ACQUIRE (paired with the
  RELEASE/ACQ_REL publish).

No TLA+ module (per the 2026-05-23 spec-to-code broadening) — the `env->lock` is the
standard per-Proc-group pattern (Territory / allowance); rigor is the prose here +
the focused audit + the unit tests.

## Tests (`kernel/test/test_env.c`, 11 tests)

`env.set_get`, `env.create_idempotent`, `env.overwrite_truncate`,
`env.unset_monotonic` (the net-3d stale-id-fails-clean guarantee), `env.iter_order`
(monotonic readdir, skip-removed), `env.bounds` (the three DoS caps),
`env.clone_deep_independent` (deep-copy + parent/child independence),
`env.free_null_tolerant` (NULL-tolerant + no double-free), `devenv.bestiary`
(vtable shape + `perm_enforced == false`), `devenv.walk_reuse_nc` (the #57a
0-element reuse-`nc` contract), `devenv.walk_read` (the end-to-end walk + read
against the current Proc's env).

The full set→inherit→`goenvs`→`os.Getenv` loop is proven in-VM by `usr/go-env`
(joey sets `/env/GOENVTEST` + `/env/GOENVNUM` on its own env, spawns the probe which
inherits the copy; emits `go-env: STAGE 4a OK`).

## Error paths

- `env_lookup` / `env_create` / `env_unset` on a bad name (empty, `>= ENV_NAME_MAX`,
  contains `/` or NUL) → `0` / `false` (rejected by `name_valid`).
- `env_create` at `ENV_MAX_ENTRIES` → `0` (no free slot).
- `env_write` with `off + n > ENV_VALUE_MAX`, or `off < 0` / `n < 0`, or OOM → `-1`.
- `env_read` / `env_write` on a gone (unset) id → `-1` (the net-3d clean miss).
- `devenv_read`/`write` on the root → `-1`; `devenv_readdir` on a value file → `-1`;
  `devenv_create` with `DMDIR`, or `devenv_unlink` with `REMOVEDIR` → rejected.
- `env_proc()` NULL (off a thread) → every op fails closed.

## Semantics note (the Go side)

`os.Getenv` / `os.Environ` read a **snapshot** the runtime cached at startup
(`goenvs` reads `/env` once); `os.Setenv` / `os.Unsetenv` mutate that in-process
cache **without** writing back to `/env` — exactly as Plan 9 does, so a child does
not observe a parent's post-spawn `Setenv`. To change the on-device environment a
process writes `/env/KEY` (`os.WriteFile`), and the change is inherited by
*subsequently* spawned children (each `rfork` deep-copies the current `/env`).

The split is load-bearing: `goenvs` populates the **runtime** `envs` slice, but
`os.Getenv` routes through the **syscall** package (`os.Getenv → syscall.Getenv`),
which reads `runtime_envs()`. Both halves must be wired — the Stage-3a
`syscall/env_thylacine.go` stub (returning `"", false`) was the bug that made
`os.Getenv` ignore a correctly-populated `/env`.

## Status

Landed at Go Stage 4a (`002764b` kernel, `72d610d` bootenv; go-thylacine fork
`6dbb23d` goenvs + `5ae5437` syscall env). `1004/1004` kernel tests, boot OK, the
G15 loop proven in-VM. `perm_enforced` deferred; RFENVG (shared environment groups)
deferred (`ref` always 1). The post-pivot `/env` re-graft (the probe runs pre-pivot)
is a Stage-4b seam — mirror the `/dev` re-graft (pre-pivot `O_PATH` grab + post-pivot
`mkdir` + `MREPL`); `PGRP_MAX_MOUNTS` is already bumped 16 → 20.

## Known caveats / footguns

- **A value-file fd is not a directory.** `devenv_create` resolves names against the
  calling Proc's env root regardless of the "parent" Spoor's qid — so a `create`
  driven off a *value-file* fd still mints a top-level variable rather than failing
  `ENOTDIR`. Harmless (bounded by `ENV_MAX_ENTRIES`, confined to the caller's own
  env, no UAF/leak/isolation break), but not strictly POSIX. No v1.0 consumer reaches
  it (creates go through the `/env` root dir fd).
- **`.` is treated as a variable name.** `/env` relies on the resolver (`stalk`) to
  clean `.` path components before the walk; a `.` that reached `devenv_walk` would
  miss as an absent variable. No v1.0 consumer walks `/env/.`.
- **Snapshot, not live.** Per the Plan 9 semantics above, `os.Setenv` does not write
  back to `/env`; cross-process propagation is via inheritance at spawn, not a shared
  live view.
