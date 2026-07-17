# 135 — PTY kernel arc (sessions, process groups, the pts registry, the tty seam, job control)

**Status**: PTY-1 (the kernel half) as-built. Sub-chunks 1a–1f landed on branch
`pty-1` (rebased onto `a58403fb`, the 8c-3 tip). The userspace `ptyfs` byte/cooking
server, the `/dev/ptmx` + `/dev/pts/<n>` tree, per-fd `termios`, and `specs/pty.tla`'s
master/slave data-path realization are **PTY-2+** (Phase 8).

**Binding design**: `docs/PTY-DESIGN.md` (the Fable design pass — four votes: a
userspace 9P `ptyfs`; full POSIX sessions + process groups; Ctrl-Z generalizes the
audited debug stopped-state, I-39; spec-first re-enabled for I-20). **Invariant**: I-20
(the stop leg) + composes I-1/I-9/I-19/I-22/I-24/I-39. **Model**: `specs/pty.tla` (the
data path + signal routing) + `specs/pty_stop.tla` (the stop-ownership algebra, the
PTY-1f pre-commit gate; `docs/DEBUG-FS-DESIGN.md` §5c is the sibling debug-stop
precedent). **ARCH**: §23.5 + §25.4 (the audit-trigger row; the authoritative
prosecution copy).

---

## 1. Purpose

The PTY kernel arc is the **security-sensitive half** of pseudo-terminal support.
Everything a userspace terminal server (`ptyfs`) could do without a trust boundary
stays in userspace; everything that must not be forgeable — signal routing to a
process group, controlling-terminal ownership, the job-control stop — lives here.

The dividing line is a single property (the I-1/I-22 bound): **a terminal server can
never name a process group.** Its sole signalling authority is a pts-scoped
`SYS_TTY_SIGNAL(pts_id, class)`; the kernel resolves `pts_id → controlling session →
that session's foreground group` and delivers there. A compromised or buggy `ptyfs`
can signal only the terminals it minted, and only their foreground groups — never an
arbitrary pgrp, never another server's terminal.

The arc delivers, in six sub-chunks:

- **1a** POSIX sessions + process groups (`sid`/`pgid` on `Proc`; `setsid`/`setpgid`/
  `getpgid`/`getsid`).
- **1b** the `tty:*` note class — kernel-only-POST **and** catchable, a new class
  distinct from both `interrupt` (anyone-post) and `snare:*` (uncatchable-terminate).
- **1c** the pts registry — the kernel-side pts identity, a gen-stamped registry keyed
  by `(SrvConn pointer, qid)`.
- **1d** the tty seam — `SYS_TTY_SIGNAL` + the controlling-terminal syscalls
  (`tcsetpgrp`/`tcgetpgrp`/acquire).
- **1e** the `SYS_WAIT_PID` extension — `WUNTRACED`/`WCONTINUED` + process-group
  selectors + report-is-not-reap.
- **1f** the job-control stop — `job_stop_req` as a second, independent stop owner
  beside the debugger's; `SYS_TTY_CONT`; the POSIX orphan rule.

---

## 2. Public API — syscalls

| # | Syscall | Args | Returns | Notes |
|---|---|---|---|---|
| 89 | `SYS_SETSID` | — | new sid (== caller pid) / `-T_E_ACCES` | POSIX `setsid`; refuses a caller that is already a group leader. |
| 90 | `SYS_SETPGID` | `pid`, `pgid` | 0 / `-T_E_ACCES` / `-T_E_SRCH` | POSIX `setpgid`; `pid`/`pgid` 0 = self; a session leader can't be moved; cross-session moves refused. |
| 91 | `SYS_GETPGID` | `pid` (0 = self) | pgid / `-T_E_SRCH` | Answers for a ZOMBIE too (so `waitpid(-pgid)` matches a zombie's group). |
| 92 | `SYS_GETSID` | `pid` (0 = self) | sid / `-T_E_SRCH` | As above. |
| 93 | `SYS_PTY_REGISTER` | `op`, `a1`, `a2`, `a3` | per-op / `-T_E_*` | Server-only (ptyfs). `op` ∈ {`MINT`, `SLAVE`, `FREE`}. See §5. |
| 94 | `SYS_TTY_SIGNAL` | `pts_id`, `class` | posted/affected count / `-T_E_*` | Server-only signal authority. `class` ∈ `TTY_SIG_{INT,QUIT,TSTP,WINCH,HUP}`. See §6. |
| 95 | `SYS_TTY_ACQUIRE` | `slave_fd` | 0 / `-T_E_*` | POSIX controlling-terminal acquisition (the anti-steal dance, F7). |
| 96 | `SYS_TTY_SET_FG` | `fd`, `pgid` | 0 / `-T_E_*` | POSIX `tcsetpgrp`. |
| 97 | `SYS_TTY_GET_FG` | `fd` | fg_pgid (0 = none) / `-T_E_*` | POSIX `tcgetpgrp`. Master-side reads unconditionally. |
| 98 | `SYS_TTY_CONT` | `fd`, `pgid` | visited count / `-T_E_*` | **PTY-1f**; the shell's `fg`/`bg` resume. Gated exactly like `SET_FG`. See §7.4. |

`SYS_WAIT_PID` (existing number) gained the PTY-1e flags + selectors (§8).

All EPERM-class contours answer `-T_E_ACCES` (the `errno.h` −1-alias rule). Every
value in `[-4095, -1]` is a negated errno the pouch boundary-line passes through.

---

## 3. Sessions + process groups (1a)

`struct Proc` carries `u32 sid` and `u32 pgid` (offsets 328/332). **Unlike** the debug
and resource tail fields (KP_ZERO, never inherited), these are **rfork-inherited** — a
child joins its parent's session and group (POSIX fork). A parentless Proc (kproc,
joey) defaults to its own session and group (`sid = pgid = pid`), stamped beside the
pid at both assignment sites.

Mutated **only** by `proc_setsid` / `proc_setpgid`, under `g_proc_table_lock`. Read by
the tty seam (fg-pgid routing), `notes_post_pgrp` (the fan-out), the `wait_pid_for`
pgrp selectors, and the orphan rule.

```c
int proc_setsid(struct Proc *self);              // -> new sid or -T_E_ACCES
int proc_setpgid(struct Proc *self, int pid, int pgid);   // 0 / -T_E_ACCES / -T_E_SRCH
int proc_getpgid(struct Proc *self, int pid);    // pgid / -T_E_SRCH  (ZOMBIE answers)
int proc_getsid(struct Proc *self, int pid);     // sid / -T_E_SRCH
bool proc_pgrp_in_session(u32 pgid, u32 sid);    // an ALIVE member of pgid is in sid?
```

`proc_setsid` refuses a caller that already leads a group (POSIX EPERM — a group leader
cannot create a session, else it would orphan its group); on success `sid = pgid = pid`
and the caller drops any controlling terminal (I-2: setsid severs the tty). `setpgid`
refuses moving a session leader and refuses a cross-session target.

---

## 4. The `tty:*` note class (1b)

A **new note class** — kernel-only-POST **and** catchable — between `interrupt`
(anyone-post + catchable) and `snare:*` (kernel-post + uncatchable-terminate). The
five names share one mask bit `NOTE_BIT_TTY` (5); per-kind masking is a v1.x lift (the
`NOTE_BIT_SNARE` precedent). `NOTE_MASK_SUPPORTED = 0x3f`.

| Name | POSIX | Uncaught default |
|---|---|---|
| `tty:winch` | SIGWINCH | ignored (informational; queue for the fd-read path) |
| `tty:susp` | SIGTSTP | **STOP** (the job-control machinery — applied at POST time, §7.2) |
| `tty:cont` | SIGCONT | resume (a kernel side effect, not a note disposition) |
| `tty:quit` | SIGQUIT | **terminate** (the LS-5 pattern) |
| `tty:hup` | SIGHUP | **terminate** |

**The POST gate is the only userspace barrier** and it is load-bearing: `notes_post`
rejects a `tty:`-prefixed name from userspace `SYS_POSTNOTE` (`synthetic == false`),
exactly as it rejects `snare:*`. Without it, `tty:cont` would be postable via the
parent-only `SYS_POSTNOTE` gate, letting an unprivileged parent resume a
**debugger-stopped** child — an I-39 leak (F4). The kernel-synthetic posters (the tty
seam, the orphan/teardown fans) pass `synthetic = true`.

The terminate class `{interrupt, tty:quit, tty:hup}` arms a **second** latch,
`PROC_FLAG_TTY_TERMINATE_PENDING` (bit 8), distinct from LS-5c's
`PROC_FLAG_INTR_TERMINATE_PENDING` (bit 7). Two latches, not one, because
`thread_die_pending` reads each **lock-free** and pairs it with **its own** family mask
bit (`interrupt ↔ NOTE_BIT_INTERRUPT`, the tty terminate pair ↔ `NOTE_BIT_TTY`) — a
single shared latch would make a thread that masked only `interrupt` spuriously
`*_INTR`-unwind for a pending `tty:hup` it hasn't masked, breaking "a latch-woken
thread never unwinds into a tail that refuses to act."

```c
int notes_post_pgrp(u32 pgid, const char *name, u32 arg);  // -> members posted
int notes_post_pid(int pid, const char *name, u32 arg);    // -> 1 if posted (F13's 2nd hup target)
```

Both run the whole fan-out under **one** `g_proc_table_lock` hold: membership (`p->pgid`)
is read under the same lock that serializes `setpgid`/`rfork`/`exit`, so a concurrent
membership mutation orders entirely before or after the fan — never a half-delivered
group (F14). `pgid`/`pid` 0 is refused (the boot group is never a target). Each member's
post takes the established `g_proc_table_lock → q->lock` edge, and the per-member
terminate-wake (`proc_interrupt_terminate_wake`) runs under the same hold.

---

## 5. The pts registry (1c)

The kernel-side pts identity is a **registry**, not a handle-table kind — the Weft
`share_id` shape. There is no userspace-holdable pts handle; a slave/master fd resolves
to a pts **server-side**, by the connection it arrived on plus the file's qid.

`struct pts_entry` (64 slots in `g_pts[PTS_MAX]`, one leaf `g_pts_lock`):

```c
struct pts_binding { bool used; bool master; struct SrvConn *conn; u64 qid; };
struct pts_entry {
    bool live;
    u32  gen;          // 0 = virgin; >= 1 at mint; bumped at free (F11 gen guard)
    u32  server_pid;   // the minting server (pids never reused -> the authority anchor)
    struct pts_binding bindings[PTS_BINDINGS_MAX];   // master + up to N slaves
    u32  ct_sid;       // controlling session (0 = none)  -- kernel-owned (the F1 seam)
    u32  fg_pgid;      // foreground group   (0 = none)
};
```

**The correlation key is SrvConn pointer identity.** Both endpoints of a connection
share one `struct SrvConn`; each binding **holds a `srvconn_ref`** (so no ABA — the
pointer can't be freed-and-reused under a live binding); a resolve does a
**pointer-compare only, never a deref** → fail-closed. The `(SrvConn, qid)` pair is
global-unique across the registry.

```c
s64 pts_mint(struct Proc *server, struct SrvConn *cn, u64 master_qid);   // -> pts_id / -errno
int pts_bind_slave(struct Proc *server, struct SrvConn *cn, u64 slave_qid, u64 pts_id);
int pts_free(struct Proc *server, u64 pts_id);
s64 pts_resolve_conn_qid(struct SrvConn *cn, u64 qid, bool *is_master_out);
s64 pts_resolve_spoor(struct Spoor *sp, bool *is_master_out);   // fd -> pts (see below)
int pts_spoor_conn_qid(struct Spoor *sp, struct SrvConn **cn_out, u64 *qid_out);
```

`pts_spoor_conn_qid` resolves an fd's Spoor to `(SrvConn, qid)`: `dev9p_client_fid(sp)`
→ the underlying `p9_client` → `p9_srvconn_transport_conn(cl)` — a **magic-checked ctx
downcast** on the transport (each backend's ctx leads with a distinct `u32` magic;
`P9_SRVCONN_TRANSPORT_MAGIC` yields the `SrvConn`, else NULL). A loopback / spoor-backed
client carries no `SrvConn` → NULL → the resolve fails closed (no pts can be registered
on such a session).

The `pts_id` encodes `(gen << PTS_IDX_BITS) | idx` (idx bits = 16); `gen` is never 0 and
bumps at every free. So a `pts_id` held across a free + re-mint of the same slot fails
every later op (F11 / the netd slot-gen / net-3d F1 mis-route class).

**Authority anchors**: MINT is gated (at the syscall front) on a **server-endpoint conn
fd** (`devsrv_conn_of(sp)` + `!(sp->flag & CSRVCLIENT)`) plus `PROC_FLAG_MAY_POST_SERVICE`
(the Weft-7 F1 registry-squat lesson); SLAVE/FREE on the **minting server's pid**
(`server_pid`, monotonic, never reused). A `ct_sid`/`fg_pgid` mutation is the
controlling-terminal cores' job (§6), not the registry's.

**Lifetime**: `srvconn_unref` is **never** called under `g_pts_lock` (the last unref
tears down + frees — chan/slab locks). `pts_clear_locked` **stages** the binding conns
into a caller array; the caller unrefs them after the lock drops. A dead server's conns
are TORN by its handle-close teardown; `pts_gc_one_locked` reclaims (at mint-full) an
entry whose every binding conn is torn — so a crashed-then-restarted ptyfs regains
registry capacity.

---

## 6. The tty seam (1d)

The kernel resolves `pts → controlling session → foreground group`; the server names
only the pts.

```c
s64 pts_tty_signal(struct Proc *server, u64 pts_id, u32 sig_class);   // server-scoped
s64 pts_tty_acquire(struct Proc *p, struct SrvConn *cn, u64 qid);     // controlling-terminal acquire
s64 pts_tty_set_fg(struct Proc *p, struct SrvConn *cn, u64 qid, u32 pgid);   // tcsetpgrp
s64 pts_tty_get_fg(struct Proc *p, struct SrvConn *cn, u64 qid);      // tcgetpgrp
s64 pts_tty_cont(struct Proc *p, struct SrvConn *cn, u64 qid, u32 pgid);     // PTY-1f, §7.4
```

**`pts_tty_signal`** (the SIGNAL syscall): validates `server_pid` + gen under
`g_pts_lock`, **snapshots** `(ct_sid, fg_pgid)`, releases, then posts — **no
`g_pts_lock → g_proc_table_lock` nesting anywhere**. The snapshot is the F11 no-redirect
property: a concurrent free/re-mint can't redirect an in-flight signal (the fg value was
captured while the id was valid); a post to a group that emptied in the window is the
benign POSIX race. Class routing:

- `INT` → `interrupt`, `QUIT` → `tty:quit`, `WINCH` → `tty:winch`, `HUP` → `tty:hup`,
  each via `notes_post_pgrp(fg, …)`.
- `HUP` **additionally** reaches the controlling process — the session leader
  (`pid == ct_sid`) — when it is not in the foreground group (F13's two POSIX
  carrier-loss targets), deduped by pgid.
- `TSTP` → `proc_job_stop_pgrp(fg)` — the job-control stop fan (§7.2), live since PTY-1f.
- No controlling session / no fg seated → 0 (a terminal nobody controls routes nowhere).

**Acquisition** (`pts_tty_acquire`): the caller must be a session leader (`sid == pid`)
whose session has no controlling terminal yet; the `(cn, qid)` must be a **slave**
binding (a master-side fd → `-T_E_INVAL`). A pts already controlled by **another**
session is never stolen (`-T_E_ACCES`, F7); re-acquiring one's own → 0 (second-open
inherits). On success it binds `ct_sid = sid` + seats `fg_pgid = the leader's pgid`. One
controlling terminal per session (a second distinct pts → `-T_E_ACCES`).

**`tcsetpgrp`/`tcgetpgrp`**: `set_fg` gates on the caller being in the pts's controlling
session (`ct_sid == p->sid`) and `pgid` naming a group with an ALIVE member in that
session (`proc_pgrp_in_session`, checked **unlocked** before the seat — the group-empties
race is the benign POSIX one). `get_fg`: a **master-side** fd reads unconditionally (the
terminal emulator owns the terminal — the Linux `TIOCGPGRP`-on-master shape); a slave-side
fd requires the caller in the controlling session.

---

## 7. The job-control stop (1f)

### 7.1 Two stop owners on one park

`struct Proc` carries `u32 job_stop_req` at offset **316** — the pad slot immediately
after `debug_stop_req` (312), on the same cache line, **no struct growth** (`Proc` stays
344 bytes). It is the **second, independent stop owner** beside the debugger's:

```c
static inline bool proc_stop_requested(const struct Proc *p) {
    return (__atomic_load_n(&p->debug_stop_req, __ATOMIC_ACQUIRE) |
            __atomic_load_n(&p->job_stop_req,  __ATOMIC_ACQUIRE)) != 0;
}
```

A thread **parks** at its EL0-return checkpoint (or the `sleep`/`tsleep` stop detour)
iff `debug_stop_req | job_stop_req`, and each resume clears **only its own owner**:

- `proc_debug_resume` clears `debug_stop_req` — never `job_stop_req`.
- `proc_job_resume_one_locked` clears `job_stop_req` — never `debug_stop_req`.

A woken thread re-checks `stop_park_wake_cond` (`!proc_stop_requested`) and re-parks
while the **other** owner still holds. So a `tty:cont` can never run a debugger-stopped
thread — precisely the `pty_stop.tla` `BUGGY_DOUBLE_STOP` counterexample (a resume that
clears both owners). Death overrides both: every `group_exit_msg` check precedes the
stop checks (`pty_stop.tla` `DeathWinsOverJobStop`), so a group-terminate reaps even a
job-stopped group (the #811 death-interruptible sleep reaches the parked thread).

### 7.2 The TSTP fan (`proc_job_stop_pgrp`)

Under one `g_proc_table_lock` hold, per ALIVE member of `pgid`:

1. **The catchability gate** (`proc_tty_susp_would_stop_locked`, the LS-5
   `notes_interrupt_should_terminate_locked` analog): the default STOP fires only when
   the target has no async handler, is not self-managing (no notes fd), **and** at least
   one thread leaves `NOTE_BIT_TTY` unmasked. Otherwise the susp is **CAUGHT** — the
   `tty:susp` note is posted (delivered/deferred on the target's own terms) and **no
   stop** happens. tmux/bash/vim catch SIGTSTP to save terminal state; an uncatchable
   susp would fail PTY-4's own gate.
2. **The orphan check**: an uncaught susp on an **orphaned** group (no ALIVE
   same-session out-of-group parent of any member) is **discarded entirely** — the POSIX
   orphan rule's stop-suppression half (nobody could resume it).
3. Otherwise the **default STOP** (`proc_job_stop_one_locked`): set `job_stop_req` +
   `stop_report_pending` + run the shared wake cascade + wake the parent's
   `child_waiters` (the PTY-1e `WAIT_UNTRACED` edge). The signal is **consumed by the
   default action** — no `tty:susp` note is queued, so nothing stays pending across the
   stop (a stale susp delivered after a later cont would re-suspend a resumed program).

An already-job-stopped member is skipped (a second Ctrl-Z on a stopped process is a
POSIX no-op — no re-latch, no note).

### 7.3 The shared wake cascade

`proc_stop_wake_cascade_locked(p)` is factored out and shared **verbatim** by both
owners' delivers (`proc_debug_stop_deliver` and `proc_job_stop_one_locked`): it is
`proc_group_terminate`'s death-wake cascade, but the woken sleeper arms the sleep-detour
park instead of the die. `torpor_wake_all_for_proc(p)` for the futex/torpor waiters
(Go's idle Ms), then the per-peer `wait_lock → rendez_blocked_on → wakeup` walk, then
`smp_resched_others()` to kick an EL0-running peer to its tail. The RELEASE store of the
stop flag happens-before each wake and each peer's `wait_lock` acquire, so the woken
sleeper's ACQUIRE-read observes the stop — no stop-wake lost between the wake and the
detour re-park (I-9 register-then-observe; `debug_stop.tla` `StopWakesSleeper`).

The park itself is the 8c-2 machinery, unchanged: a stopped thread parks on its own
`debug_rendez` (shared stop-park rendez, one park, two owners), at the EL0-return tail
(`el0_return_stop_check`) or via the `sleep`/`tsleep` detour (`proc_stop_sleeper_park` —
renamed from `proc_debug_stop_sleeper_park`; the cond `stop_park_wake_cond` proceeds when
**both** owners clear).

### 7.4 `SYS_TTY_CONT = 98` — the shell's `fg`/`bg` resume

The **one named path** by which a session member resumes a job-stopped group in its
session. It is the number beyond the 89–97 pin (user-signed-off 2026-07-17) — necessary
because F4 keeps `tty:cont` kernel-synthetic-only on the POST axis (no `SYS_POSTNOTE`
reaches it — the I-39 gate against conting a debug-stopped child), F8 covers only the
pts-teardown cont, and ordinary `kill` covers only one's **own** group. A shell doing
`fg` must resume **another** pgrp of its session.

`pts_tty_cont(p, cn, qid, pgid)` is gated exactly like `SET_FG`: membership
(`proc_pgrp_in_session(pgid, p->sid)`) checked unlocked, then the binding +
controlling-session gates under `g_pts_lock`, then — with the leaf **released** — the
`proc_job_cont_pgrp(pgid)` fan. Per member: the `tty:cont` note (catchable-informational)
+ the job resume (`proc_job_resume_one_locked`: clear `job_stop_req` before the wake
walk, `cont_report_pending` latched, the `debug_rendez` peers woken, the parent's
`child_waiters` woken). The resume is **unconditional on catchability** (POSIX: SIGCONT
resumes whether caught or not); the target group need not be the foreground group (`bg`)
nor stopped (POSIX SIGCONT semantics). Works on a **torn** conn (the resolve is pure
pointer identity), so a shell whose ptyfs died can still resume its jobs while the
registry entry lives.

### 7.5 The teardown fan (F8) + the orphan rule

**Teardown** (`pts_teardown_fan`, staged off `pts_clear_locked` at both `pts_free` and
the mint-GC arm, run after `g_pts_lock` drops): freeing a controlled pts is POSIX modem
hangup — `tty:hup` to the fg group **and** the out-of-fg session leader (F13 dual
target), then `tty:cont` + the job resume to the fg group, per-member hup-before-cont
(POSIX 2.4.3's order — the resume lets an uncaught hup's terminate actually run and a
hup-catching survivor actually handle it). No stopped job strands with a dead SIGCONT
source.

**The orphan rule** (`proc_orphan_rule_locked`, hooked into `proc_become_zombie_locked`
**before** the reparent): a death that newly-orphans a process group it **anchored** —
its children's groups where it was the same-session out-of-group parent, and its own
group via its own parent-edge — with **job-stopped members** gets `tty:hup` then
`tty:cont` per member (resuming them + arming the uncaught-hup terminate latch). The
dying Proc is excluded from every membership/anchor walk as already-dead, so evaluating
pre-flip is exactly "the world once the Proc is gone." An **already-orphaned** group is
never re-signalled (a spurious hup could kill); a group the dying Proc never anchored
(different session) is untouched.

### 7.6 The park-predicate fan-out (round-2 R2-F2 — the load-bearing realization)

`job_stop_req` is threaded through **every** site that reads `debug_stop_req` where a
job stop must also park:

| Site | File | Change |
|---|---|---|
| `el0_return_stop_check` fast-path + proceed check | `kernel/proc.c` | `proc_stop_requested` (both owners must clear to eret). |
| `stop_park_wake_cond` (the park cond) | `kernel/proc.c` | proceed when both clear. |
| `sleep` / `tsleep` stop detours | `kernel/sched.c` | detour gate reads `proc_stop_requested`. |
| `client_stop_pending` | `kernel/9p_client.c` | reads the disjunction. |
| the elected-reader **handoff skip** | `kernel/9p_client.c` | `!(r->owner && proc_stop_requested(r->owner))`. |

The sharpest is the **8c-3 elected-9P-reader release**. A Ctrl-Z'd foreground child that
happens to hold the elected-reader role on the **shared SYSTEM Stratum** dev9p client
(`/bin`, `/lib`) must release + re-hand the role — exactly as a debug-stopped reader
does — or it freezes those trees for **every survivor Proc** until `fg`: the exact #89
bug re-opened via a flag the audited machinery does not read. Because both
`client_stop_pending` and the handoff skip now read the disjunction, a job stop drives
the same frame-atomic release (`reader_recv_frame`'s `stop_no_park`/`stop_unwinds`/
`stop_unwound` machinery classifies a job stop-unwind identically — the stable
`stop_unwound` latch is owner-only, both resumes clear their flags asynchronously).

**Deliberately NOT generalized**: `devproc_target_fully_stopped` (the `/proc/<pid>`
mem/regs/kregs/wait gate) stays **debug-only**. A job-stopped Proc is **not**
debugger-readable: its threads park on the same `debug_rendez`, but the debug surface
keys on the **debug owner** axis (I-39's attach + stop protocol; a Ctrl-Z'd process must
not become readable by whoever holds a ctl fd without issuing the debug stop). A debugger
stopping an already-job-parked target sets `debug_stop_req` and finds the threads already
parked → fully-stopped immediately. This is a documented do-not-generalize.

---

## 8. The wait extension (1e)

`SYS_WAIT_PID` went `(status_out)` → `(want_pid, flags, status_out)` at U-7-pre;
PTY-1e adds the job-control flags + selectors on top, all in `wait_pid_for`:

- **Selectors** (`want_pid`): `> 0` a specific child; `-1` any; `0` any child in the
  **caller's** group (`p->pgid` read at each authoritative re-scan under
  `g_proc_table_lock` — the mid-wait `setpgid`-leave, R2-F6); `< -1` any child in group
  `-want_pid` (negated via `s64`, `INT_MIN`-safe).
- **`WAIT_UNTRACED` / `WAIT_CONTINUED`** (flags 2 / 4): report a stopped / continued
  child. **Report-is-not-reap** (R2-F6): the report returns the child's pid + a packed
  status **without** running the reap teardown (`proc_unlink_child` / on_cpu-spin /
  `thread_free` / `proc_free`) and consumes the latch (`stop_report_pending` /
  `cont_report_pending`) **exactly once** under the lock; the child stays linked + ALIVE,
  reapable by a later exit wait. Precedence: **exit > continue > stop**. A plain wait
  (neither flag) neither sees nor consumes a latch.
- **The packed status** is Linux `wait(2)` layout, **opt-in via the flags**: exited
  `(code & 0xff) << 8`, stopped `0x7f | (20 << 8)`, continued `0xffff`. Every pre-PTY
  caller (no flags) keeps the **raw** exit status — zero flag-day. Decoders:
  `WAIT_IF_EXITED` / `WAIT_EXITSTATUS` / `WAIT_IF_STOPPED` / `WAIT_IF_CONTINUED`.

A child STOPPING (setting `stop_report_pending`) is a **new** wake trigger on the
parent's `wait_pid_for` rendez — a fresh I-9 register-then-observe edge (the latch is set
under the same `g_proc_table_lock` the re-scan takes, and `child_waiters` is woken under
it, so register-then-observe holds exactly as for the zombie wake). The 1f setters
(`proc_job_stop_one_locked` / `proc_job_resume_one_locked`) drive this edge; a stop
supersedes an unreported cont and vice versa (latest-state reporting).

---

## 9. State machine — the stop ownership (pty_stop.tla)

```
        StopJob                    StopDebug
   {} ---------> {job}       {} ------------> {debug}
        ResumeJob                  ResumeDebug
  {job} --------> {}      {debug} ----------> {}

  {job,debug} --ResumeJob--> {debug}     (per-owner clear: StopCompatI39)
  {job,debug} --ResumeDebug--> {job}

  any state --GroupDie--> {} + grpDead    (DeathWinsOverJobStop)
```

`stopOwners ⊆ {job, debug}`; park iff non-empty. Each resume subtracts only its owner.
`GroupDie` (a group-terminate) clears everything and reaps — death wins from any state,
even a stop. The buggy cfgs: `BUGGY_DOUBLE_STOP` (a job resume subtracts **all** owners →
`StopCompatI39` violated — a debugger-stopped thread runs); `BUGGY_DEATH_BLOCKED`
(`GroupDie` gated on `stopOwners = {}` → a stopped group's death never completes →
`DeathWinsOverJobStop` violated). See `specs/SPEC-TO-CODE.md::pty_stop.tla` for the
action↔code map.

---

## 10. Spec cross-reference

- **`specs/pty_stop.tla`** — the stop-ownership algebra; the PTY-1f pre-commit gate.
  Clean + `pty_stop_liveness` + `pty_stop_buggy_double_stop` (StopCompatI39) +
  `pty_stop_buggy_death_blocked` (DeathWinsOverJobStop). Action↔code map in SPEC-TO-CODE.
- **`specs/pty.tla`** — the master/slave data path + signal routing (the design model);
  the byte/cooking action↔code map is a reservation until `ptyfs` (PTY-2+); the
  signal-routing half is realized by the §6 seam.
- **`specs/debug_stop.tla`** — re-verified GREEN. The elected-reader release + the job
  owner are **below** its abstraction (it models the debug park + death composition), so
  it stays clean while the job owner rides the same park machinery — the 8c-3 precedent.

---

## 11. Tests

Kernel unit tests (default build; `tools/test.sh`, 1161/1161 at the PTY-1f landing):

| Test | Covers |
|---|---|
| `pts.mint_bind_resolve_free` | the registry lifecycle; ref balance proven by the conn refcount returning to pre-mint. |
| `pts.gen_guard_stale_id` | a `pts_id` across a free + re-mint of the same slot fails closed (F11). |
| `pts.authority_minting_server_only` | a second server can't SLAVE/FREE another's pts (R2-F4). |
| `pts.binding_dedup_bounds_uniqueness` | idempotent re-bind, cross-pts reject, the per-entry row bound, duplicate master. |
| `pts.full_registry_torn_conn_gc` | fill 64 slots → `-T_E_AGAIN`; tear a dead server's conn → the next mint reclaims it. |
| `pts.syscall_gates` | the MINT MAY_POST_SERVICE gate, the CSRVCLIENT client-endpoint reject, bad-fd/op, a full round trip. |
| `pts.tty_acquire_matrix` | the F7 anti-steal + one-ctty-per-session + the second-open inherit + the master-side reject. |
| `pts.tty_set_get_fg_matrix` | `tcsetpgrp`/`tcgetpgrp` session-membership + the master-read carve-out. |
| `pts.tty_signal_routing` | INT/WINCH to exactly fg; HUP's dual target + the in-fg dedup; the gen guard; the note-only TSTP on a thread-less member. |
| `pts.tty_tstp_stop_cont_seam` | **1f** — the uncaught default STOP (no note) + idempotent second TSTP + the caught (self-managing) and all-masked note-only arms + `pts_tty_cont`'s gates + the resume/report transitions. |
| `pts.teardown_hup_cont` | **1f** — the F8 fan: resume + dual-target hup + cont + the terminate latch. |
| `proc.wait_pid_for_{no_match,wnohang_alive_then_reap,selects_target,pgrp_selectors,report_not_reap}` | the 1e selectors + report-is-not-reap + the packed-vs-raw discriminator. |
| `proc.job_stop_owner_algebra` | **1f** — the stopOwners algebra both orders; each resume clears only its own owner (the `BUGGY_DOUBLE_STOP` regression). |
| `proc.job_stop_park_report_cont_live` | **1f** — a REAL child blocked in `tsleep` is job-stopped (the deliver cascade → the sleep-detour park on its own `debug_rendez`, observed), `WAIT_UNTRACED` reports without reaping, `SYS_TTY_CONT` resumes (`WAIT_CONTINUED` reports), the released child runs to a clean exit (resume liveness). |
| `proc.job_stop_orphan_rule` | **1f** — both POSIX halves: the TSTP discard on an orphaned group vs the stop once anchored; the anchor-death hup+cont fan (a different-session child group untouched). |
| `9p_client.handoff_skips_debug_stopped_owner` | **1f leg** — the JOB revert-probe: a debug-only handoff hands the role to a Ctrl-Z'd owner; + both-axes drop. |
| `pgrp.*` (1a) | `setsid`/`setpgid`/`getpgid`/`getsid` contours; ZOMBIE answers get/reject set. |
| `tty.*` (1b) | the tty:* class post gate + the terminate-latch pairing. |

New test hooks: `proc_test_link_child` (fabricated parent-edges the orphan-rule anchor
qualification reads); `proc_test_unlink` now unlinks from the actual parent
(backward-compatible — a test-linked Proc's parent is kproc).

**What the tests deliberately don't cover**: the full multi-Proc-survivor-during-stop
E2E (the shared owed cross-Proc multi-in-flight SMP harness, carried across
#841/#845/#349/Loom) — the job-reader release is exercised via the sleep detour in the
live test + the SMP gate; the byte/cooking + the `ptmx`/`pts` tree + per-fd termios
(PTY-2+).

---

## 12. Error paths

Every EPERM-class contour → `-T_E_ACCES`. Registry misses → `-T_E_NOENT` (an unbound
`(cn, qid)`) or `-T_E_INVAL` (a stale/bad `pts_id`, a non-dev9p Spoor). MINT without the
service flag → `-T_E_ACCES`; a client-endpoint conn fd → `-T_E_INVAL`. `SET_FG`/`CONT`
`pgid` 0 or with no ALIVE session member → `-T_E_INVAL`/`-T_E_ACCES`. `setpgid` on a
nonexistent target → `-T_E_SRCH`. A full registry with every conn live → `-T_E_AGAIN`. A
per-entry binding overflow → `-T_E_NOMEM`.

---

## 13. Known caveats / footguns

- **The all-masked stop-on-unmask deviation** (documented v1.0): if every thread masks
  `NOTE_BIT_TTY`, an uncaught susp is delivered **note-only** — the POSIX "stop on the
  first unmask of a pending stop signal" is not modeled (the disposition is evaluated
  once, at post). Masking a stop signal is rare.
- **The hup-surviving-shell-with-stopped-bg-jobs corner** (v1.x seam): once a pts entry
  is **freed**, `SYS_TTY_CONT`'s pointer-identity resolve dies with it, so a shell that
  catches the teardown hup and survives with stopped background jobs on a freed pts can
  no longer resume them via the tty path (a kill-authority SIGCONT is the fallback).
- **Lazy GC teardown timing**: a dead server's registry entry runs its carrier-loss fan
  at the next **mint-full** GC, not immediately. The interim is covered by the orphan
  rule (the shell's death) + the torn-conn `SYS_TTY_CONT` resolve (the entry still
  resolves by pointer while it lives).
- **`devproc_target_fully_stopped` stays debug-only** (deliberate, §7.6) — do NOT
  generalize it to `proc_stop_requested`.
- **`job_stop_req` occupies `debug_stop_req`'s pad slot** (offset 316). Do not add a
  field there without re-checking the `_Static_assert` on `Proc` size (344) and the
  offset asserts.
- The byte/cooking engine, `/dev/ptmx` + `/dev/pts/<n>`, per-fd `termios`, and the
  master/slave data path are **PTY-2+** — `specs/pty.tla`'s data-path realization lands
  with the userspace `ptyfs` server.

---

## 14. Status

| Sub-chunk | Commit | What |
|---|---|---|
| PTY-1a | `a418dba0` | sessions + process groups (89–92). |
| PTY-1b | `131ea092` | the `tty:*` note class + `notes_post_pgrp`/`notes_post_pid`. |
| PTY-1c | `2a006c3e` | the pts registry + `SYS_PTY_REGISTER` (93). |
| PTY-1d | `96900a36` | the tty seam + controlling-terminal syscalls (94–97). |
| PTY-1e | `f2ee7f66` | the `SYS_WAIT_PID` extension. |
| PTY-1f | `bce3fe33` | `job_stop_req` + the park fan-out + `SYS_TTY_CONT` (98) + the orphan rule. |
| PTY-1g | — | this doc + the ARCH §25.4 row + the focused holotype + the SMP gate + merge. |
