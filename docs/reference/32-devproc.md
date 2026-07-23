# 32 — devproc: synthetic /proc Dev (P4-C)

The first **directory-typed** Dev. Walks resolve `/proc/<pid>/{status, cmdline, ctl, ns}`; each leaf is a small synthetic text file. Process control via `/proc/<pid>/ctl` is 9P-mediated per Plan 9 idiom: a write of `kill` / `killgrp` terminates the target Proc (A-4b). P4-C landed the read side + a stub ctl-write; **A-4b** (IDENTITY-DESIGN.md §9.8, invariant I-26) makes ctl real — the two-axis kill authority (owner OR `CAP_HOSTOWNER` OR `CAP_KILL`) + dispatch via `proc_group_terminate`.

Per ARCH §9.4 + ROADMAP §6.1 + IDENTITY-DESIGN.md §9.8 (A-4b).

---

## Purpose

Two distinct functions, the same Dev:

1. **Process introspection** (read side). Userspace code (Phase 5+) opens `/proc/<pid>/status` and reads pid + state + thread count + (for zombies) exit_status. Same for `cmdline` and `ns`. The format is plain ASCII text — Plan 9 doesn't byte-pack /proc; the text is the contract.

2. **Process control** (write side). Writing `kill` / `killgrp` to `/proc/<pid>/ctl` is the way one process terminates another. A-4b makes this real: the two-axis I-26 authority (owner OR `CAP_HOSTOWNER` OR `CAP_KILL`) is enforced at the write site, then `proc_group_terminate` cascades the target's thread-group. **prowl-4** adds the job-control pair `suspend` / `resume` (the unconditional Plan-9 /proc stop/cont, the SAME I-26 gate — a monitor pausing a process). The debug run-control verbs `attach`/`detach`/`stop`/`start`/`step`/`hw*` are the separate Go IDE Stage 8a surface (I-39; `134-debug-fs.md`). See the `/proc/<pid>/ctl` section below for the full authority + dispatch contract.

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
/proc/<pid>/sched             path = (pid << 32) | PQS_SCHED      QTFILE  (prowl-3b; OQ-4 owner-or-CAP_HOSTOWNER)
```

Subkinds 5..15 reserved for `mem`, `fd/`, `wait`, `note`, `args`, etc. — added as the syscall surface needs them.

`PROC_QID_ROOT_PATH = 0` is the sentinel for the dev's apex directory; pid=0 (kproc) is encoded with subkind `PQS_PID_DIR` (= 1), so kproc's root path is `(0 << 32) | 1 = 1`, distinct from the dev root.

### Namespace residence (#57a)

devproc is **mounted at `/proc`** in the boot namespace (`kernel/joey.c::joey_mount_static_dev`), grafted onto the synthetic devramfs `/proc` mount-point dir in the kproc Territory (inherited by every Proc via `territory_clone`) and re-grafted onto the pivoted disk root by the long-running init. Before #57a devproc was kernel-internal — defined and unit-tested but unreachable by a path.

Becoming reachable through `stalk` required fixing `devproc_walk` to honor the **reuse-`nc` contract**: a non-NULL `nc` (the caller's `spoor_clone(parent)`) must be returned as `wq->spoor`, and a 0-element walk must yield `nqid == 0` — the shape `stalk`'s `clone_walk_zero` needs to cross the mount. devproc had kept the pre-16b-gamma self-cloning shape (returning its own `spoor_clone`, which `stalk` rejects as `wq->spoor != nc`) precisely because it had never been mounted; the dual mode (`nc == NULL` → self-clone, for the kernel-internal direct-call tests; `nc != NULL` → reuse) matches `devramfs_walk`.

`devproc.perm_enforced == false`, so `/proc` is world-walkable (Plan 9 all-pids-visible introspection); the per-pid `ctl` **kill** authority stays I-26 two-axis-gated at the write site, independent of namespace reachability — the mount widens *visibility*, never *authority*. Per-namespace `/proc` filtering (a container sees only its own pids) is the container runner (#70).

**Read-path lifetime (#57a focused-audit F2).** `devproc_read` and `devproc_stat_native` find the target Proc and read its fields (`format_status`/`format_ns` / the uid+gid) **inside a `proc_for_each` callback — under `g_proc_table_lock`** (the kill-path shape), early-stopping at the matched pid. Before #57a devproc was unmounted, so these ran single-threaded only; they used `proc_find_by_pid` (which locks, walks, **unlocks**, returns the bare pointer) and then dereferenced it *outside* the lock. Once the mount makes `/proc` cross-Proc reachable (Proc A reading `/proc/<B>` by path or `SYS_FSTAT`), a concurrent reap (`wait_pid` on another CPU) could `kmem_cache_free(B)` in the lookup→deref window — a **UAF** (info leak or a wild `p->territory` deref → extinction). Holding `g_proc_table_lock` across the find+read closes it: a reaped Proc is unlinked-under-the-lock (`proc_unlink_child`'s precondition) *before* it is freed, so under the lock a Proc is either linked-and-alive (safe to deref) or already-unlinked-and-not-found. `format_ns` (#66b) now calls `territory_format_ns`, which takes the target territory's `ns_lock` to read the mount LIST (a pointer walk a concurrent unmount could free, unlike the pre-#66 single-aligned-int `binds: N` read). That is a **nested** `g_proc_table_lock → ns_lock` acquire, and it is acyclic: nothing held under `ns_lock` (in `territory.c`: mount/unmount/clone/chroot/pivot/bind/unbind/mount_lookup) ever takes `g_proc_table_lock` — they only `spoor_ref`/`path_ref` (atomic) and defer the sleeping `spoor_clunk` to outside the lock. Under `ns_lock` the entries + their ref-held immutable `Path` strings are stable; no sleep, no allocation runs under either lock.

---

## File contents (v1.0)

### `/proc/<pid>/status`

**prowl-1 (PROWL-DESIGN.md §3) brought this to Plan 9 parity** — it now carries the process name, cumulative on-CPU time, the parent pid, and the owning principal/gid, not just pid/state/threads.

```
name:    <basename>          ← the execed binary's basename; "?" if unstamped
pid:     <decimal>
state:   ALIVE|ZOMBIE|INVALID
threads: <decimal>
cpu_ns:  <decimal>           ← cumulative on-CPU time in ns (Σ run_ns over threads)
ppid:    <decimal>           ← parent pid; 0 if none
principal:<decimal> gid:<decimal>
pages:   <decimal>           ← live SYS_BURROW_ATTACH anon pages (#65)
children:<decimal>           ← live direct children (#65)
exit:    <decimal>           ← only present when state == ZOMBIE
```

`cpu_ns` is **cumulative + monotonic** — a monitor derives %CPU by diffing it across two polls (`Δcpu_ns / Δwall`, the htop method); the kernel keeps no instantaneous-rate state. It is the sum of per-thread `run_ns` (`proc_cpu_ns`, walked under `g_proc_table_lock` via the `proc_for_each` read path — the #57a walk-safety); it does **not** include the currently-running thread's in-flight slice since its last switch-in (< 1 slice, negligible over a poll interval). `name` is the basename of the **resolved binary path** (`exec_setup_from_spoor` from the Spoor's #66 namespace name — unforgeable, not caller-controlled argv[0]); the boot chain (kproc/joey) is stamped its literal.

Reading kproc's status (`/proc/0/status`) typically:
```
name:    kproc
pid:     0
state:   ALIVE
threads: 1
cpu_ns:  12345678
ppid:    0
principal:4294967294 gid:4294967294
pages:   0
children:3
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
- `suspend` / `resume` — **the job-control stop/cont (prowl-4; PROWL-DESIGN.md §4.3).** `suspend` parks the target's threads (sets `job_stop_req`); `resume` un-parks them (clears it). This is the **unconditional Plan-9 `/proc` job stop** — a monitor pausing an arbitrary process by pid, the SIGTSTP/SIGCONT shape reached through the `/proc/<pid>/ctl` authority (not a controlling terminal). It is **uncatchable** exactly as `kill` is (no `tty:susp` note, no catchability gate — those are the pts SIGTSTP path's, `proc_job_stop_pgrp`), and gated by the SAME two-axis I-26 authority as kill (stopping is strictly weaker than the killing that authority already permits). It is **distinct** from the debug `stop` / `start` verbs below: those set `debug_stop_req` and require a debugger `attach` slot; `suspend`/`resume` set `job_stop_req` (I-20's second stop owner) and need no attach — the two compose per StopCompatI39 (a target both debug- and job-stopped stays parked until BOTH clear).
- `attach` / `detach` / `stop` / `start` / `waitstop` / `step` / `hwbreak` / `hwrmbreak` / `hwwatch` / `hwrmwatch` / `hwverify` — the **debug** run-control + hardware-breakpoint verbs (Go IDE Stage 8a; I-39). These require the caller's ctl fd to own the debugger attach slot and set `debug_stop_req`; see `docs/reference/134-debug-fs.md` for the full contract. (The debug `stop`/`start` are NOT the job-control `suspend`/`resume` above.)
- any other token — `-1`.

An authorized `kill` / `killgrp` / `suspend` / `resume` returns `n` (the byte count); a denied / refused / unknown write returns `-1`.

**Authority (two-axis, I-26).** A `kill` / `killgrp` / `suspend` / `resume` is authorized iff:

- the caller is the target's **owner** — the ctl file is mode `0600` (owner-private), so the owner always holds the w-bit and this reduces to "same `principal_id` as the target" (covers killing your own processes; the parent-of-same-identity-child case is expressible as ownership); OR
- the caller holds **`CAP_HOSTOWNER`** (the unified admin); OR
- the caller holds **`CAP_KILL`** (the cross-identity override for a debugger/supervisor that is neither owner nor hostowner).

The check is computed **directly** in `devproc_write` (NOT via `perm_check`): `CAP_DAC_OVERRIDE` — the generic fs-rwx admin — is deliberately **not** a kill axis. The A-4 capability split keeps fs-admin and process-kill orthogonal (mirrors Linux `CAP_DAC_OVERRIDE` vs `CAP_KILL`); an fs-admin is not implicitly a process-killer. No identity bypasses (I-22); containment is namespace visibility (I-1 — a Proc that cannot walk to `/proc/<pid>` cannot kill it).

**kproc is unkillable.** A `kill` of pid 0 (the kernel proc) is refused *before* the authority check — even a `CAP_KILL` holder cannot terminate the kernel. Likewise `suspend`/`resume` refuse kproc (and any non-ALIVE target) before the authority check.

**Job-control dispatch (prowl-4).** `suspend` / `resume` resolve the target under `g_proc_table_lock` (the `proc_for_each` resolve+authorize idiom, `devproc_job_walk_cb`), refuse kproc + non-ALIVE, apply the I-26 gate above, then call `proc_job_stop_proc` / `proc_job_cont_proc` (`kernel/proc.c`). Those reuse the PTY-1f per-member job-control helpers: `suspend` = `proc_job_stop_one_locked` (set `job_stop_req` + latch the `WAIT_UNTRACED` stop report + wake the target's own sleepers) then the one hoisted `smp_resched_others()` reschedule IPI so a peer RUNNING at EL0 traps to its stop checkpoint; `resume` = `proc_job_resume_one_locked` (clear `job_stop_req` + latch the `WAIT_CONTINUED` cont report + wake the parked threads). The park is at the **same** EL0-return-tail / `sleep`-`tsleep` detour checkpoint the debug stop uses, so a suspended thread holds **no kernel lock** while parked. Both are non-blocking (unlike the debug `stop`/`waitstop`) + idempotent (a second `suspend` / a `resume` of a running Proc is a no-op). Death wins: a `suspend` racing a group-terminate sets a flag the die-check-before-stop-check tail ignores (the target dies); a suspended process that is later orphaned is hup+cont'd by the POSIX orphan rule. **Caveats** (job-control-inherent, not kernel-soundness): suspending a process that holds a userspace lock others wait on can wedge that *workload* (resumable via `resume`); suspending your own controlling shell is the same footgun class as killing it (already reachable via `kill`).

**Dispatch.** Both verbs dispatch via `proc_group_terminate` uniformly (single + multi thread), under `g_proc_table_lock` via the `proc_for_each` resolve+authorize+kill idiom (the audited `sys_postnote` pattern — the target is alive under the lock, so no reap-UAF). `proc_group_terminate` is chosen over a bare note post because, post-#811, it is the only termination primitive whose death-wake is **total** (universal death-interruptible sleep): a single-thread target blocked in a non-notes sleep (e.g. `poll(-1)`) would not wake on a bare note post.

**User-reachability** of `/proc/<pid>/ctl` is a Utopia namespace seam: devproc is kernel-internal at v1.0 (joey's territory is a single root, no synthetic Dev grafted in), and reaching a path that crosses the devramfs->devproc mount boundary needs a boot-path `SYS_MOUNT` of devproc + the production multi-component, mount-crossing path resolver (Plan 9 `namec`). `SYS_POSTNOTE` already gives userspace the parent-kill path. A-4b lands + kernel-unit-tests the mechanism + the authority.

### `/proc/<pid>/ns`

```
mount <mountpoint-name> <source-name-or-#dc>
mount <mountpoint-name> <source-name-or-#dc>
...
binds: <decimal>
```

The Plan 9 `ns` substrate (#66b). One `mount` line per entry in the Proc's territory mount table, then the bind count. The **mountpoint** column is the namespace name the directory was mounted onto — the entry's refcounted `PgrpMount.mp_path` (a `Spoor.path`, #66a; `?` when the mountpoint had no retained name, e.g. a kernel-internal direct walk). The **source** column is the mounted tree's namespace name (`source->path`) when it has one, else `#<dc>` — the Plan 9 device spec (`#9`=9P, `#s`=srv, `#p`=proc, `#-`=devnone). Rendered by `territory.c::territory_format_ns` (so the `ns_lock` discipline stays in `territory.c`); `devproc.c::format_ns` is now a thin call into it. Per **I-33** the mount table keys on the `(dc, devno, qid.path)` identity, NEVER on `mp_path`, so the names are introspection-only — a wrong/absent name can only misreport this file, never change a resolution. A list longer than the read buffer (`DEVPROC_READ_BUF` = 512) truncates cleanly (best-effort introspection). Read it with the native `ns [pid]` tool (`usr/coreutils/src/bin/ns.rs`).

The bind column stays a count: `binds[]` are abstract `path_id_t` pairs (no string names) at v1.0.

### `/proc/<pid>/sched` (prowl-3b)

The per-thread scheduler-introspection block — the deep half of the `prowl`
scheduler view (`docs/PROWL-DESIGN.md` §3.3):

```
name:    stratumd
pid:     47
threads: 6
tid band cpu run_ns nsched parks nmig state
0 NORM 2 4821773110 45219 8801 214 SLP
1 NORM 0 1099284551 12034 3310 41 RUN
...
```

The header lines (`name`/`pid`/`threads`) then one space-separated row per thread:
`tid`, `band` (INTR/NORM/IDLE, `Thread.band`), `cpu` (the last CPU it ran on —
`-` until dispatched), `run_ns` (cumulative on-CPU ns), `nsched` (times it got the
CPU — a busy-yield storm reads an astronomical rate), `parks` (`nsleeps`, voluntary
sleeps), `nmig` (cross-CPU moves), `state` (RUN/RDY/SLP/EXIT). The counters are the
prowl-3a per-thread accumulators (`docs/reference/15-scheduler.md`), read
`__atomic` RELAXED; the reader diffs them across polls for per-thread rates.

`format_sched` walks `p->threads` **under `g_proc_table_lock`** (via
`devproc_read_cb`, the `proc_cpu_ns` #95 walk-safety) and bounds the walk: a row
is written to a scratch offset and committed only once it fully fits, so a
heavily-threaded Proc's tail truncates cleanly at a whole-row boundary (never a
partial row, never past `DEVPROC_READ_BUF`=2048 — bumped from 512 for the
per-thread rows).

**Visibility (OQ-4).** This is the **deep** per-process view, so the read is gated
`owner-or-CAP_HOSTOWNER` (`devproc_sched_authorized`, checked in-callback against
the target found under the lock; a denial returns `-1`). The **summary**
(name/%cpu/state/mem) stays all-visible via `/ctl/procs` + `/proc/<pid>/status`
(the Plan 9 all-pids posture) — but a process's per-thread scheduler internals are
its owner's own or the host operator's. The gate is **strictly narrower** than the
kill (`+CAP_KILL`) and debug (`+CAP_DEBUG`) gates: reading scheduler telemetry is
neither killing nor debugging, so those caps are deliberately not axes (I-22, the
capability split stays orthogonal). Mode `0400` (owner); the `CAP_HOSTOWNER`
override at the read site is the `perm_enforced=false` devproc idiom (as with the
kill/debug gates). Composes I-1 + the `/ctl` visibility-not-authority posture; no
new §28 invariant.

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

### Owner-axis breadth at v1.0: SYSTEM-owns-SYSTEM (A-4b audit F1)

The owner axis is `caller->principal_id == target->principal_id`. At v1.0 the *entire* boot chain (kproc → joey → corvus → stratumd → every spawned Proc) runs as `PRINCIPAL_SYSTEM` (no `/sbin/login` stamps distinct identities until A-5), so every SYSTEM-principal Proc is "owner" of every other. This is the correct consequence of the identity model (mirrors Linux: all root-owned procs can signal each other) and narrows naturally once A-5 stamps distinct login identities. It is dormant at v1.0 (devproc is unmounted; the surface is EL0-unreachable). **Forward-looking** (the future `/proc`-mount chunk): reconsider whether a *persistent* PID-1/init Proc should join `kproc` in the unconditional-unkillable guard — at v1.0 joey exits after bringup (no persistent init), and only `kproc` is structurally critical, so only `kproc` is guarded.

### `devproc_stat_native` uses the lockless `proc_find_by_pid` window (A-4b audit F2)

`stat_native` dereferences the bare pointer from `proc_find_by_pid` with no lock held — the documented v1.0 "no concurrent reap" window, the *same class* as `devproc_read` above. It is NOT on any authorization path: every `perm_check` site that consults `stat_native` is gated on `dev->perm_enforced`, and `devproc.perm_enforced == false`; stat_native serves only `SYS_FSTAT` introspection + `SEEK_END`. The KILL path does NOT use it (it resolves under `g_proc_table_lock` via `proc_for_each`). The general fix (deref inside a `proc_for_each` cb, as the kill path does) is the right shape if devproc ever gains concurrent reaping or perm-enforcement; tracks with the pre-existing `devproc_read` window.

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
