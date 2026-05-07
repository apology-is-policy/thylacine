# 32 — devproc: synthetic /proc Dev (P4-C)

The first **directory-typed** Dev. Walks resolve `/proc/<pid>/{status, cmdline, ctl, ns}`; each leaf is a small synthetic text file. Process control via `/proc/<pid>/ctl` is 9P-mediated per Plan 9 idiom (write commands like `kill`, `stop`, `start`, `notepg`); v1.0 P4-C lands the read side + a stub ctl-write that consumes commands.

Per ARCH §9.4 + ROADMAP §6.1.

---

## Purpose

Two distinct functions, the same Dev:

1. **Process introspection** (read side). Userspace code (Phase 5+) opens `/proc/<pid>/status` and reads pid + state + thread count + (for zombies) exit_status. Same for `cmdline` and `ns`. The format is plain ASCII text — Plan 9 doesn't byte-pack /proc; the text is the contract.

2. **Process control** (write side). Writing commands to `/proc/<pid>/ctl` is the way one process sends signals / stops / starts / kills another. v1.0 P4-C accepts but discards the commands; the verb-set + dispatch lands when `specs/notes.tla` + the kernel notes layer go live (Phase 5+ per the spec catalogue).

The lifecycle primitives `rfork` / `exits` / `wait_pid` remain syscalls (per ARCH §11) — they manipulate the calling thread's own kernel state and can't be 9P-mediated. /proc supplements them with cross-process control.

---

## Public API — `<thylacine/dev.h>` + `<thylacine/proc.h>`

```c
extern struct Dev devproc;        // dc='p', name="proc"

// Lookup primitives added in proc.c (P4-C).
struct Proc *proc_find_by_pid(int pid);
int          proc_for_each(int (*cb)(struct Proc *, void *), void *arg);
```

`proc_find_by_pid` walks the kproc tree (DFS through `parent->children->sibling`) under `g_proc_table_lock`. Returns `NULL` if no matching pid. `proc_for_each` is the same DFS but invokes a callback with each Proc; the lock is held throughout — callbacks must not re-enter `rfork` / `exits` / `wait_pid` / proc_find / proc_for_each.

---

## Namespace + qid encoding

```
/proc                         path = 0                            QTDIR
/proc/<pid>/                  path = (pid << 32) | PQS_PID_DIR    QTDIR
/proc/<pid>/status            path = (pid << 32) | PQS_STATUS     QTFILE
/proc/<pid>/cmdline           path = (pid << 32) | PQS_CMDLINE    QTFILE
/proc/<pid>/ctl               path = (pid << 32) | PQS_CTL        QTFILE
/proc/<pid>/ns                path = (pid << 32) | PQS_NS         QTFILE
```

Subkinds 5..15 reserved for `mem`, `fd/`, `wait`, `note`, `args`, etc. — added as the syscall surface needs them.

`PROC_QID_ROOT_PATH = 0` is the sentinel for the dev's apex directory; pid=0 (kproc) is encoded with subkind `PQS_PID_DIR` (= 1), so kproc's root path is `(0 << 32) | 1 = 1`, distinct from the dev root.

---

## File contents (v1.0)

### `/proc/<pid>/status`

```
pid:     <decimal>
state:   ALIVE|ZOMBIE|INVALID
threads: <decimal>
exit:    <decimal>           ← only present when state == ZOMBIE
```

Reading kproc's status (`/proc/0/status`) typically:
```
pid:     0
state:   ALIVE
threads: 1
```

### `/proc/<pid>/cmdline`

```
<argv[0]>\n
```

v1.0 prints `kproc` for kproc and `<unnamed>` for everyone else (no `Proc.argv0` field yet). Phase 5+ extension to the ELF loader populates `argv0` so this shows actual program names.

### `/proc/<pid>/ctl`

Read returns 0 (write-only). Write returns n (commands consumed; verb-set held to Phase 5+). The future verb-set will include:

- `kill`           — terminate (sends `SIGKILL`-equivalent note)
- `stop`           — pause (note `SIGSTOP`-equivalent)
- `start`          — resume from stop
- `notepg <verb>`  — broadcast to all threads in the proc-group
- `kill-group`     — kill all threads in proc-group

All routed through the (future) kernel `notes` layer pinned by `specs/notes.tla`.

### `/proc/<pid>/ns`

```
binds: <decimal>
```

v1.0 reports the territory's bind count. Future extension dumps each bind as `from -> to` lines once `territory_iter` is added.

---

## Walk semantics

`devproc.walk(c, nc, name[], nname)` supports multi-step walks. Each step calls a private `walk_one(cur_path, name, *out_qid)`:

| `cur_path` | `name` | Result |
|---|---|---|
| any | `".."` | go up (`PQS_PID_DIR` → root; root → root) |
| `0` (root) | decimal `<pid>` | resolve via `proc_find_by_pid`; if found, return `(pid << 32) | PQS_PID_DIR`, QTDIR; else miss |
| `(pid << 32) | PQS_PID_DIR` | `"status"` / `"cmdline"` / `"ctl"` / `"ns"` | return file qid, QTFILE |
| anywhere else | any | miss |

Walk is N-step: `walk(root, NULL, ["0", "status"], 2)` produces a `Walkqid` with `nqid=2` and the result Spoor positioned at `/proc/0/status`. On any miss, the walk stops and returns the partial Walkqid (`nqid < nname`); the result Spoor is positioned at the deepest successful step.

`nc` is currently ignored — `devproc_walk` always `spoor_clone(c)` for the result Spoor. Phase 5+ may use `nc` for in-place walk when 9P client integrates fid management.

The Walkqid is `kmalloc`-allocated by `walkqid_alloc(nname)` (added in P4-C; declared in `<thylacine/spoor.h>`). The walk caller owns the memory and must `walkqid_free(wq)` when done. Single-allocation idiom mirrors Plan 9's port/sysfile.c walk handling.

---

## Implementation

`kernel/devproc.c` (~360 LOC). Structure:

- **Qid encoding helpers** (`proc_qid_make`, `proc_qid_pid`, `proc_qid_kind`).
- **Per-pid file table** (g_proc_pid_files[] — name + subkind for status/cmdline/ctl/ns).
- **Tiny formatters** (`fmt_udec`, `fmt_sdec`, `fmt_str`) — no libc; ~30 LOC of byte-level integer/string formatting into a stack buffer.
- **Content generators** (`format_status`, `format_cmdline`, `format_ns`) — produce up to `cap` bytes; return total bytes that would be produced, allowing offset-aware reads.
- **`parse_decimal`** — strict pid parser (rejects empty, leading `-`, non-digits, overflow past 31 bits).
- **`walk_one`** — qid-by-qid step dispatch.
- **Vtable functions** — `attach`, `walk`, `open`, `close`, `read`, `write`, plus the standard stubs.

`kernel/proc.c` adds:

- `proc_find_by_pid_walk` (recursive helper).
- `proc_find_by_pid` (public; locks + dispatches the walker).
- `proc_for_each_walk` (recursive helper).
- `proc_for_each` (public; locks + dispatches).

`kernel/spoor.c` adds:

- `walkqid_alloc(max_qids)` — kmalloc-backed allocation sized for the flexible array.
- `walkqid_free(w)` — kfree.

`<thylacine/spoor.h>` declares the helpers; `<thylacine/dev.h>` declares `extern struct Dev devproc`.

`kernel/dev.c::dev_init` adds `dev_register(&devproc)` after the trivial Devs.

### Read offset semantics

Each file's `format_*` writes the FULL content into a 256-byte stack buffer (sufficient for v1.0's small fields), then `devproc_read` copies the requested `[off, off+n)` slice. If `off >= total`, returns 0 (EOF). If `off + n > total`, returns `total - off` (short read, which Plan 9 callers handle via re-read at `off + got`). NULL buf or n < 0 returns -1.

### Read on a directory qid

Returns -1. The 9P readdir machinery (which would synthesize a stat-record stream for each child) lands when the syscall surface or the in-kernel readdir helper land — both Phase 4+ chunks. Tests in `devproc.read_dir_returns_neg1` lock in this v1.0 behavior so a future readdir landing is an explicit replacement, not a quiet contract change.

### Write semantics

- `ctl` writes return `n` (commands consumed). The future verb parser is the natural extension point; the API contract (write returns n) does NOT change.
- Non-`ctl` writes return `-1` (status / cmdline / ns / dirs are read-only).

---

## Spec cross-reference

P4-C is impl-only — no new TLA+ module. Two cross-references for future audit:

- **Notes / signal delivery** (`specs/notes.tla`) — pin the cross-process delivery semantics that ctl-write will dispatch through. Phase 5+ when notes lands; the ctl write parser then routes verbs through `note_post(target_proc, note)`.
- **Concurrent proc lookup** — at v1.0 P4-C, `proc_find_by_pid` returns a stable pointer under "no concurrent reap" assumption; SMP-safe lookup with refcount lands at Phase 5+ (alongside the syscall surface where userspace-facing /proc ops can race wait_pid).

---

## Tests

`kernel/test/test_devproc.c` — 13 tests:

| Test | Covers |
|---|---|
| `devproc.bestiary_smoke` | Registration: dc='p' + name="proc" + lookup. |
| `devproc.attach_returns_dir` | Root attach: qid.path=0, qid.type=QTDIR. |
| `devproc.walk_root_to_kproc_dir` | walk("0") yields `/proc/0/` QTDIR. |
| `devproc.walk_unknown_pid_misses` | walk("99999") returns Walkqid with nqid=0. |
| `devproc.walk_to_status_file` | Multi-step walk: `["0", "status"]` → nqid=2, QTFILE. |
| `devproc.walk_dotdot_to_root` | ".." from `/proc/0/` → root. |
| `devproc.read_status_format` | Read /proc/0/status; verify text contains "pid:" / "state:" / "ALIVE" / "threads:". |
| `devproc.read_cmdline_kproc` | Read /proc/0/cmdline; contains "kproc". |
| `devproc.read_ns_format` | Read /proc/0/ns; contains "binds:". |
| `devproc.read_ctl_returns_zero` | ctl read returns 0 (write-only). |
| `devproc.write_ctl_consumes` | ctl write returns n; non-ctl writes return -1. |
| `devproc.read_dir_returns_neg1` | Read on root QTDIR returns -1 (readdir deferred). |
| `devproc.read_partial_offset` | Offset-aware slice; off >= total returns 0. |

Tests cover the v1.0 contract end-to-end. Future Phase 5+ chunks add tests for cross-process ctl-write routing through notes + readdir-on-directory + multi-pid walk under concurrent rfork.

---

## Status

| Component | State |
|---|---|
| `kernel/devproc.c` + devproc Dev (dc='p') | Landed (P4-C) |
| Qid encoding + walk dispatch | Landed (P4-C) |
| status / cmdline / ns / ctl read | Landed (P4-C) |
| ctl write (stub: consume commands) | Landed (P4-C) |
| Multi-step walk | Landed (P4-C) |
| `proc_find_by_pid` + `proc_for_each` | Landed (P4-C; in `kernel/proc.c`) |
| `walkqid_alloc` + `walkqid_free` | Landed (P4-C; in `kernel/spoor.c`) |
| In-kernel tests | 13 covering the full v1.0 contract |
| Bestiary count | 6 (devnone + cons + null + zero + random + proc) |
| /proc readdir (9P stat-stream synthesis) | Held to a Phase 4+ readdir chunk |
| /proc/<pid>/mem | Held to Phase 5+ (mem read needs handle-table-mediated VA→PA path) |
| /proc/<pid>/fd/ | Held to Phase 5+ (handle table iteration via 9P readdir) |
| ctl verb parser + note dispatch | Held to Phase 5+ (after `specs/notes.tla` lands) |
| Refcount-protected `proc_find_by_pid` (SMP) | Held to Phase 5+ (proc_get/put + wait_pid integration) |
| `argv[0]` in cmdline | Held to a Phase 5+ ELF loader extension |

---

## Known caveats / footguns

### `proc_find_by_pid` returns a stable pointer only under the "no concurrent reap" assumption

At v1.0 the returned `struct Proc *` is valid only while the caller hasn't released the kernel context to a path that could `wait_pid`-reap the target. v1.0 callers (devproc reads under a single boot CPU; future userspace `read /proc/<pid>/status` calls under the calling thread's context where the target Proc cannot be self-reaped) satisfy this. SMP-safe lookup with refcount lands at Phase 5+; the API will remain `proc_find_by_pid(pid)` but the return value will be `proc_get`'d, requiring a balancing `proc_put`.

### Walk allocates a fresh Spoor per step

`devproc_walk` always `spoor_clone(c)` the result. For deep multi-step walks the allocation traffic adds up. Phase 5+ may extend by accepting `nc` (the candidate Spoor) and mutating it in place, eliminating per-step alloc — the API contract supports this; v1.0 simplifies for clarity.

### ctl writes are silent at v1.0

Write to `/proc/<pid>/ctl` returns n but does not act. A test that expects "after `kill` the proc is ZOMBIE" will fail. v1.0 P4-C is the substrate; the verb-set + dispatch is Phase 5+ work after `specs/notes.tla` pins delivery semantics.

### Reads on root or pid_dir return -1 at v1.0

The Plan 9 idiom is "read on a directory returns synthesized 9P stat records (one per child entry)". v1.0 P4-C returns -1 — readdir lands when the 9P interpreter or in-kernel readdir helper lands. Tests pin -1; replacing it requires explicit code change.

### `cmdline` is `<unnamed>` for non-kproc procs at v1.0

No `Proc.argv0` field yet. The exec path (`exec_setup`) doesn't capture argv0. Phase 5+ extension: pass argv array into exec_setup, store argv0 string in struct Proc (caching the first argv element), expose via cmdline.

### `pid` lookup is O(N) DFS

Acceptable while live-Proc count is bounded. Phase 5+ adds a hash table or RB-tree keyed by pid for O(log N) lookup when the proc count grows past ~100.

### Walkqid is heap-allocated per call

`walkqid_alloc` does `kmalloc`. For deep walk loops this churns the kmalloc-32 / kmalloc-64 cache. v1.0 acceptable; if walk becomes a hot path, a per-CPU walkqid bump arena is the natural optimization.

---

## References

- `docs/ARCHITECTURE.md` §9.4 — /proc layout (directly the contract this implements).
- `docs/ROADMAP.md` §6.1 — Phase 4 deliverables (dev/proc among them).
- `docs/reference/30-dev-spoor.md` — Dev vtable + Spoor lifecycle (the substrate).
- `docs/reference/31-trivial-devs.md` — leaf-file Devs (cons / null / zero / random) for context.
- `docs/reference/14-process-model.md` — Proc + Thread lifecycle (the source-of-truth for what /proc/<pid>/status reports).
- `specs/notes.tla` (planned, Phase 5+) — note delivery; the future ctl-write dispatch routes here.
