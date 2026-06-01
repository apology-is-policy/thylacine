# 32 — devproc: synthetic /proc Dev (P4-C)

The first **directory-typed** Dev. Walks resolve `/proc/<pid>/{status, cmdline, ctl, ns}`; each leaf is a small synthetic text file. Process control via `/proc/<pid>/ctl` is 9P-mediated per Plan 9 idiom: a write of `kill` / `killgrp` terminates the target Proc (A-4b). P4-C landed the read side + a stub ctl-write; **A-4b** (IDENTITY-DESIGN.md §9.8, invariant I-26) makes ctl real — the two-axis kill authority (owner OR `CAP_HOSTOWNER` OR `CAP_KILL`) + dispatch via `proc_group_terminate`.

Per ARCH §9.4 + ROADMAP §6.1 + IDENTITY-DESIGN.md §9.8 (A-4b).

---

## Purpose

Two distinct functions, the same Dev:

1. **Process introspection** (read side). Userspace code (Phase 5+) opens `/proc/<pid>/status` and reads pid + state + thread count + (for zombies) exit_status. Same for `cmdline` and `ns`. The format is plain ASCII text — Plan 9 doesn't byte-pack /proc; the text is the contract.

2. **Process control** (write side). Writing `kill` / `killgrp` to `/proc/<pid>/ctl` is the way one process terminates another. A-4b makes this real: the two-axis I-26 authority (owner OR `CAP_HOSTOWNER` OR `CAP_KILL`) is enforced at the write site, then `proc_group_terminate` cascades the target's thread-group. `stop` / `start` stay stubbed (scheduler integration, ARCH [OPEN Q 7.6.D]). See the `/proc/<pid>/ctl` section below for the full authority + dispatch contract.

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

Read returns 0 (write-only). Write parses a leading whitespace-delimited verb (A-4b; IDENTITY-DESIGN.md §9.8, invariant I-26):

- `kill`    — terminate the target Proc's thread-group.
- `killgrp` — terminate the target Proc's thread-group. At v1.0 (no cross-Proc process groups — no `setpgrp`) this is identical to `kill`; a distinct cross-Proc `killgrp` is a v1.x seam (pending process groups).
- `stop` / `start` — recognized as future verbs but unimplemented (scheduler integration, ARCH [OPEN Q 7.6.D]); return `-1`.
- any other token — `-1`.

An authorized `kill`/`killgrp` returns `n` (the byte count); a denied / refused / unknown write returns `-1`.

**Authority (two-axis, I-26).** A `kill`/`killgrp` is authorized iff:

- the caller is the target's **owner** — the ctl file is mode `0600` (owner-private), so the owner always holds the w-bit and this reduces to "same `principal_id` as the target" (covers killing your own processes; the parent-of-same-identity-child case is expressible as ownership); OR
- the caller holds **`CAP_HOSTOWNER`** (the unified admin); OR
- the caller holds **`CAP_KILL`** (the cross-identity override for a debugger/supervisor that is neither owner nor hostowner).

The check is computed **directly** in `devproc_write` (NOT via `perm_check`): `CAP_DAC_OVERRIDE` — the generic fs-rwx admin — is deliberately **not** a kill axis. The A-4 capability split keeps fs-admin and process-kill orthogonal (mirrors Linux `CAP_DAC_OVERRIDE` vs `CAP_KILL`); an fs-admin is not implicitly a process-killer. No identity bypasses (I-22); containment is namespace visibility (I-1 — a Proc that cannot walk to `/proc/<pid>` cannot kill it).

**kproc is unkillable.** A `kill` of pid 0 (the kernel proc) is refused *before* the authority check — even a `CAP_KILL` holder cannot terminate the kernel.

**Dispatch.** Both verbs dispatch via `proc_group_terminate` uniformly (single + multi thread), under `g_proc_table_lock` via the `proc_for_each` resolve+authorize+kill idiom (the audited `sys_postnote` pattern — the target is alive under the lock, so no reap-UAF). `proc_group_terminate` is chosen over a bare note post because, post-#811, it is the only termination primitive whose death-wake is **total** (universal death-interruptible sleep): a single-thread target blocked in a non-notes sleep (e.g. `poll(-1)`) would not wake on a bare note post.

**User-reachability** of `/proc/<pid>/ctl` is a Utopia namespace seam: devproc is kernel-internal at v1.0 (joey's territory is a single root, no synthetic Dev grafted in), and reaching a path that crosses the devramfs->devproc mount boundary needs a boot-path `SYS_MOUNT` of devproc + the production multi-component, mount-crossing path resolver (Plan 9 `namec`). `SYS_POSTNOTE` already gives userspace the parent-kill path. A-4b lands + kernel-unit-tests the mechanism + the authority.

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

- `ctl` writes parse the verb (A-4b): an authorized `kill`/`killgrp` terminates the target Proc's thread-group and returns `n`; a denied / refused (kproc) / unimplemented (`stop`/`start`) / unknown verb returns `-1`. See the `/proc/<pid>/ctl` section for the two-axis authority + dispatch.
- Non-`ctl` writes return `-1` (status / cmdline / ns / dirs are read-only).

---

## Spec cross-reference

P4-C + A-4b are impl-only — no new TLA+ module (per the 2026-05-23 spec-to-code suspension). Cross-references for audit:

- **Group termination** (ARCH §7.9.1, invariant I-24) — the ctl `kill`/`killgrp` dispatch routes through `proc_group_terminate` (the audited #809/#811 cascade), NOT a bare note post: post-#811 it is the only termination primitive whose death-wake is total. The A-4b authority (I-26) is validated by prose (IDENTITY-DESIGN.md §9.8) + this doc + the audit + the kernel tests.
- **The `sys_postnote` idiom** (`kernel/syscall.c`) — devproc's resolve+authorize+kill walk mirrors `postnote_walk_cb` (target resolved + acted-on under `g_proc_table_lock` via `proc_for_each`; no reap-UAF). The authority model differs (postnote is parent-only; ctl is the I-26 two-axis set).
- **Concurrent proc lookup** — at v1.0, `proc_find_by_pid` (used by `devproc_stat_native` + `devproc_read`) returns a stable pointer only under the "no concurrent reap" assumption; the KILL path does NOT use it — it resolves under `g_proc_table_lock` via `proc_for_each`. SMP-safe refcounted lookup lands at Phase 5+.

---

## Tests

`kernel/test/test_devproc.c` — 16 tests (P4-C: 13; A-4b: the renamed `write_ctl_rejects` + 3 new kill tests):

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
| `devproc.write_ctl_rejects` | A-4b: kill of kproc (pid 0) refused; unknown verb -1; non-ctl write -1. |
| `devproc.read_dir_returns_neg1` | Read on root QTDIR returns -1 (readdir deferred). |
| `devproc.read_partial_offset` | Offset-aware slice; off >= total returns 0. |
| `devproc.kill_authorized_predicate` | A-4b: the two-axis predicate — owner / CAP_KILL / CAP_HOSTOWNER allow; non-owner-no-cap denies; **CAP_DAC_OVERRIDE denies** (not a kill axis). |
| `devproc.stat_native_ctl_owner` | A-4b: stat_native reports target uid/gid; ctl mode 0600, status 0444; dev apex -1. |
| `devproc.write_ctl_kill_dispatch` | A-4b: owner kill + killgrp set `group_exit_msg`; non-owner denied (NULL); non-ALIVE refused. |

The A-4b kill tests construct synthetic targets (no running thread, so `group_exit_msg` is the dispatch observable — the death step is the audited #809/#811 EL0-die-check path) and exercise the authority predicate + dispatch directly. Userspace E2E of `/proc/<pid>/ctl` is deferred with the namespace seam (see the ctl section). Future chunks add readdir-on-directory + multi-pid walk under concurrent rfork.

---

## Status

| Component | State |
|---|---|
| `kernel/devproc.c` + devproc Dev (dc='p') | Landed (P4-C) |
| Qid encoding + walk dispatch | Landed (P4-C) |
| status / cmdline / ns / ctl read | Landed (P4-C) |
| ctl write: `kill` / `killgrp` + two-axis authority (I-26) | Landed (A-4b) |
| `devproc_stat_native` (per-pid owner + mode) | Landed (A-4b) |
| Multi-step walk | Landed (P4-C) |
| `proc_find_by_pid` + `proc_for_each` | Landed (P4-C; in `kernel/proc.c`) |
| `walkqid_alloc` + `walkqid_free` | Landed (P4-C; in `kernel/spoor.c`) |
| In-kernel tests | 16 (P4-C contract + A-4b kill mechanism/authority) |
| Bestiary count | 6 (devnone + cons + null + zero + random + proc) |
| /proc readdir (9P stat-stream synthesis) | Held to a Phase 4+ readdir chunk |
| /proc/<pid>/mem | Held to Phase 5+ (mem read needs handle-table-mediated VA→PA path) |
| /proc/<pid>/fd/ | Held to Phase 5+ (handle table iteration via 9P readdir) |
| ctl `stop` / `start` verbs | Held (scheduler integration, ARCH [OPEN Q 7.6.D]) |
| `/proc` user-mount (reachability) | Held to Utopia (namespace seam: boot `SYS_MOUNT` + the `namec` multi-component mount-crossing resolver) |
| cross-Proc `killgrp` (distinct from thread-group) | Held to v1.x (no process groups at v1.0) |
| Refcount-protected `proc_find_by_pid` (SMP) | Held to Phase 5+ (proc_get/put + wait_pid integration) |
| `argv[0]` in cmdline | Held to a Phase 5+ ELF loader extension |

---

## Known caveats / footguns

### `proc_find_by_pid` returns a stable pointer only under the "no concurrent reap" assumption

At v1.0 the returned `struct Proc *` is valid only while the caller hasn't released the kernel context to a path that could `wait_pid`-reap the target. v1.0 callers (devproc reads under a single boot CPU; future userspace `read /proc/<pid>/status` calls under the calling thread's context where the target Proc cannot be self-reaped) satisfy this. SMP-safe lookup with refcount lands at Phase 5+; the API will remain `proc_find_by_pid(pid)` but the return value will be `proc_get`'d, requiring a balancing `proc_put`.

### Walk allocates a fresh Spoor per step

`devproc_walk` always `spoor_clone(c)` the result. For deep multi-step walks the allocation traffic adds up. Phase 5+ may extend by accepting `nc` (the candidate Spoor) and mutating it in place, eliminating per-step alloc — the API contract supports this; v1.0 simplifies for clarity.

### `/proc` is kernel-internal at v1.0 — ctl kill is not yet userspace-reachable

A-4b lands the ctl `kill`/`killgrp` mechanism + the two-axis authority, but `/proc` is not mounted into any user territory at v1.0 (joey's namespace is a single root: `chroot` to devramfs, `pivot_root` to Stratum FS). Reaching `/proc/<pid>/ctl` from userspace needs (a) a boot-path `SYS_MOUNT` of devproc and (b) the production multi-component, mount-crossing path resolver (Plan 9 `namec`) — the v1.0 `SYS_WALK_OPEN` is single-component + dev-internal (rejects `/`). Both are Utopia-lane namespace work (the resolver serves `/proc` + `/ctl` + `/dev` + `/net` uniformly). `SYS_POSTNOTE` already provides userspace a parent-kill path. A-4b's mechanism + authority are kernel-unit-tested.

### kproc (pid 0) is unkillable; `kill` and `killgrp` are identical at v1.0

A `kill` of pid 0 is refused before the authority check — even a `CAP_KILL` holder cannot terminate the kernel proc. And with no cross-Proc process groups at v1.0 (no `setpgrp`), `kill` and `killgrp` both terminate the *target Proc's thread-group*; a distinct cross-Proc `killgrp` (the Plan 9 note-group sense) is a v1.x seam.

### `CAP_DAC_OVERRIDE` is NOT a kill axis

The ctl kill authority is computed directly (owner OR `CAP_HOSTOWNER` OR `CAP_KILL`), NOT via `perm_check`. Routing through `perm_check` would fold in `CAP_DAC_OVERRIDE` (its A-4a DAC-override) and silently make every fs-admin a process-killer — defeating the A-4 capability split, which deliberately keeps fs-rwx admin orthogonal to process-kill (mirrors Linux `CAP_DAC_OVERRIDE` vs `CAP_KILL`). A future maintainer must not "simplify" the check to a `perm_check` call.

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
