# Implementing Linux-phenotype threads (`clone(CLONE_THREAD)`) — a build guide

**Audience:** you, implementing it yourself, for fun. **Goal:** teach the
VIVARIUM Linux phenotype to accept `clone(CLONE_THREAD)` so that stock
multithreaded Linux binaries (npxf, curl's threaded resolver, git's parallel
index-pack, basically *most* real programs) run under `viv`.

Everything here is anchored to real code at `aux-2` HEAD. Every file:line was
read off the tree; if a line has drifted a little by the time you read it, grep
the named function — the names are stable.

> **The one-sentence version.** Thylacine already has the primitive a Linux
> thread needs — a `Thread` inside a `Proc` that shares the address space, fds,
> namespace, and signal table with its peers. You are not building a threading
> engine; you are writing a *translator* from the Linux `clone` ABI onto
> machinery that already works. The single genuinely-new kernel function is
> "make a thread in the *caller's own* Proc"; almost everything else already
> exists and just needs wiring.

---

## 1. The mental model: two ways to make a thread

**Linux.** Everything is a "task." A thread is `clone(2)` with a pile of
sharing flags — `CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |
CLONE_CHILD_CLEARTID | CLONE_DETACHED`. The child is a new schedulable task
that *shares* the caller's address space, file table, and signal handlers, sits
in the same thread group (same TGID = same `getpid()`), and gets its own TID.
Crucially, the child **resumes execution at the instruction right after the
`clone` syscall, with `x0 == 0`**, on a caller-provided stack — the kernel is
never handed an "entry function."

**Thylacine.** A `Proc` owns an `AddrSpace` (page tables), a `HandleTable`
(fds), a `Territory` (namespace), and a `viv_sigtab` (signal disposition). A
`Thread` is a schedulable register-context that lives *inside* a Proc. Every
Thread of a Proc **already shares** all of those. So a Thylacine `Thread` *is* a
Linux `CLONE_THREAD` task — the sharing is definitional, not something you
arrange.

That means most of the `CLONE_*` sharing flags map to **nothing to do**:

| Linux flag | Thylacine reality | Work |
|---|---|---|
| `CLONE_VM` | Threads share the AddrSpace | **free** |
| `CLONE_FILES` | Threads share the HandleTable | **free** |
| `CLONE_FS` | Threads share the Territory | **free** |
| `CLONE_SIGHAND` | `sigtab` is Proc-scoped (`proc.h:851`) | **free** |
| `CLONE_THREAD` | a Thread in the same Proc | **free** (this is the point) |
| `CLONE_SYSVSEM` | no SysV sems at v1.0 | **free** (ignore the bit) |
| child `stack` (arg) | the thread's `SP_EL0` | pass it through |
| `CLONE_SETTLS` + `tls` | `TPIDR_EL0` | set the new thread's TLS register |
| child resumes at clone-return, `x0=0` | a copied trap frame | **the crux** — see §2 |
| `CLONE_PARENT_SETTID` + `ptid` | write the tid to `*ptid` | already have the helper |
| `CLONE_CHILD_CLEARTID` + `ctid` | write 0 + futex-wake at exit | already implemented |
| thread calls `SYS_exit`(93) | `thread_exit_self` (this thread) | route 93 correctly |
| the return value | the new thread's tid | return `tid`, not a new `pid` |

Two "free" columns are why this is a translator and not a rewrite. The two rows
that carry real work are **the crux** (§2) and a couple of **missing syscall
rows** (`futex`, `gettid`, §3.7–3.8).

**The aarch64 `clone` argument order** (you'll need this constantly). musl's
`clone.s` issues `syscall(SYS_clone=220, flags, stack, ptid, tls, ctid)`, so in
the kernel dispatch the registers land as:

```
args[0] = flags        args[3] = tls    (CLONE_SETTLS)
args[1] = child_sp     args[4] = ctid   (CLONE_CHILD_CLEARTID)
args[2] = ptid  (CLONE_PARENT_SETTID)
```

(Note `tls` comes *before* `ctid` on aarch64 — `CONFIG_CLONE_BACKWARDS`,
documented at `kernel/include/thylacine/vivarium.h:1968-1975`.)

---

## 2. The one genuinely new thing: a same-Proc thread

Here's the trap. Thylacine has *two* thread-creation cores today, and **neither
one fits `CLONE_THREAD` as-is**:

- **`thread_create_user`** (`kernel/thread.c:450-501`) takes an explicit
  `user_entry_va` — a function pointer the kernel jumps to. But a raw Linux
  `clone()` never gives the kernel an entry function; the child resumes at the
  parent's saved PC and *musl's own userspace stub* (`clone.s`, the `1:` label)
  pops the real `func`/`arg` and calls it. So the entry-va shape is wrong.

- **`rfork_internal`** (`kernel/proc.c:1245-1374`) *does* copy the parent's trap
  frame so the child resumes where the parent was (`fork_frame_init`,
  `kernel/thread.c:505-523`, sets `regs[0]=0` and `sp=child_sp`) — exactly the
  right register shape — **but it always allocates a brand-new `struct Proc`**
  (`proc_alloc_in`, `kernel/proc.c:1358`), even for the address-space-sharing
  `RFMEM` case. A new Proc means a new `pid`, which is *wrong* for a Linux
  thread: threads share one pid.

So the missing piece is the intersection: **the fork frame-copy, but into the
caller's already-existing Proc.** You will write one new function,
approximately:

```c
// kernel/thread.c — NEW. Make a Thread in `proc` (the CALLER's own Proc, NOT a
// fresh one), resuming at the caller's trap frame with x0=0, on child_sp, with
// child_tls in TPIDR_EL0. This is fork_frame_init's register shape (thread.c:505)
// applied to the SYS_THREAD_SPAWN structural path (thread_link_into_proc +
// ready) instead of rfork's proc_alloc_in path.
struct Thread *thread_create_forked_in_proc(struct Proc *proc,
                                            const struct exception_context *frame,
                                            u64 child_sp,
                                            u64 child_tls);
```

Study these three functions side by side before you write it — your new
function is a graft of the first two:

- `thread_create_forked` (`kernel/thread.c:525-599`) — the frame-copy path, but
  it's called *after* a new Proc + AddrSpace already exist. Copy its body from
  the point where it has a `struct Thread *t`; it does `fork_frame_init(&t->ctx…,
  frame, child_sp)` and `t->ctx.tpidr_el0 = child_tls` (`thread.c:590`). **Keep
  that.**
- `thread_create_user` (`kernel/thread.c:450-501`) — for the tail: how a Thread
  is allocated + `thread_link_into_proc(t, proc)` (`thread.c:408`). Take the
  `proc` from the *caller* (`current_thread()->proc`), not a freshly-alloc'd one.
- `sys_thread_spawn_handler` (`kernel/syscall.c:4551-4587`) — for the very end:
  it does `ready(nt)` and returns `nt->tid`. **That `tid` is your clone return
  value** (see §3.6).

The reason this is safe and clean: a Thread linked into an existing Proc
inherits that Proc's AddrSpace / HandleTable / Territory / sigtab **by
construction** — which is precisely every `CLONE_VM/FILES/FS/SIGHAND` guarantee,
for free. You are not cloning any of that; you're refcounting into it, exactly
as `SYS_THREAD_SPAWN` peers already do.

**One lifetime subtlety to get right (and to have the auditor check):** the
per-AddrSpace page budget (I-32) and the thread cap (`PROC_THREAD_MAX`). A new
thread charges the same `AddrSpace.page_budget` as its peers (that's correct —
sharing the address space shares the cap), and it must respect the per-Proc
thread cap the way `sys_thread_spawn_handler` does (`kernel/syscall.c:4520-ish`
— grep for the `PROC_THREAD_MAX` check and mirror it). Don't skip that check;
an unbounded `pthread_create` loop is a DoS otherwise.

---

## 3. The build, piece by piece

Each piece: **what** · **where** · **a template to copy** · **how to test it**.
Do them in this order; §4 gives the milestones so you get a *running* thread
before you make it *joinable*.

### 3.1 — The clone-flag vocabulary

**What.** musl emits `flags == 0x007D0F00`. Four of those bits have no
`VIV_CLONE_*` constant yet: `CLONE_FS`(0x200), `CLONE_SIGHAND`(0x800),
`CLONE_SYSVSEM`(0x40000), `CLONE_DETACHED`(0x400000). Add them, then define the
exact admitted word.

**Where.** `kernel/include/thylacine/vivarium.h:2004-2019` (the enum) and
`:2032-2044` (the `#define`s).

**Template.** Follow the existing style verbatim:

```c
// add to the enum at vivarium.h:2004
VIV_CLONE_FS      = 0x00000200,
VIV_CLONE_SIGHAND = 0x00000800,
VIV_CLONE_SYSVSEM = 0x00040000,
VIV_CLONE_DETACHED = 0x00400000,

// add beside VIV_CLONE_FLAGS_ADMITTED at vivarium.h:2032
#define VIV_CLONE_FLAGS_THREAD                                                 \
    ((u32)(VIV_CLONE_VM | VIV_CLONE_FS | VIV_CLONE_FILES | VIV_CLONE_SIGHAND | \
           VIV_CLONE_THREAD | VIV_CLONE_SYSVSEM | VIV_CLONE_SETTLS |          \
           VIV_CLONE_PARENT_SETTID | VIV_CLONE_CHILD_CLEARTID |              \
           VIV_CLONE_DETACHED))
```

Sanity-check: `VIV_CLONE_FLAGS_THREAD` must equal `0x007D0F00`. Put that as a
`_Static_assert` right below it — it's the cheapest possible guard against a
typo, and it documents the exact ABI you're matching.

**Test.** The static assert *is* the test at this stage (it fails the build if
the OR is wrong).

### 3.2 — The decide (make it 3-way)

**What.** `vivarium_clone_decide` currently answers a boolean `share_mem_out`
(fork vs vfork-ish). You need a third outcome: THREAD. Widen the out-param to an
enum.

**Where.** `kernel/vivarium.c:1327-1409` (body), `kernel/include/thylacine/vivarium.h:2046-2093`
(decl + doc).

**Template.** The existing function is your skeleton — keep its fail-closed
prologue and exact-equality style:

```c
enum viv_clone_mode { VIV_CLONE_MODE_FORK, VIV_CLONE_MODE_VFORK,
                      VIV_CLONE_MODE_THREAD };

enum viv_verdict vivarium_clone_decide(u64 flags, u64 stack,
                                       enum viv_clone_mode *mode_out) {
    if (!mode_out) return VIV_FORWARD;                 // fail closed

    if (flags == (u64)VIV_CLONE_FLAGS_FORK) {          // existing
        *mode_out = VIV_CLONE_MODE_FORK;
        return VIV_TRANSLATED;
    }
    if (flags == (u64)VIV_CLONE_FLAGS_ADMITTED) {      // existing vfork-ish
        if (stack == 0) return VIV_FORWARD;
        *mode_out = VIV_CLONE_MODE_VFORK;
        return VIV_TRANSLATED;
    }
    if (flags == (u64)VIV_CLONE_FLAGS_THREAD) {        // NEW
        if (stack == 0) return VIV_FORWARD;            // a thread MUST have a stack
        *mode_out = VIV_CLONE_MODE_THREAD;
        return VIV_TRANSLATED;
    }
    return VIV_FORWARD;                                // anything else -> ENOSYS
}
```

Keep the **exact-equality** discipline — do not switch to a bitmask test. It is
the phenotype's whole safety story: you translate *exactly* the flag words real
libcs emit, and anything else forwards to a clean ENOSYS instead of being
half-honored. (If a future musl adds a bit, you'll get a clean ENOSYS and a
one-line fix, not a subtle miscompile.)

**Test.** This is a pure function → a kernel unit test, no boot machinery. Copy
the shape of `test_vivarium_ioctl_decide` (`kernel/test/test_vivarium.c`, the
C2-k1a decode test — it's the freshest example of a decide unit test) or the
existing clone-decide test if there is one (grep `test_vivarium.*clone`).
Assert: the fork word → FORK; the admitted word → VFORK; `0x007D0F00` → THREAD;
`0x007D0F00` with `stack==0` → FORWARD; a garbage word → FORWARD + `mode_out`
untouched; `NULL mode_out` → FORWARD. Register it in `kernel/test/test.c` (the
forward-decl block + the table). Build + `tools/test.sh`; it runs at boot.

### 3.3 — The same-Proc thread core (the crux)

**What.** Write `thread_create_forked_in_proc` (§2).

**Where.** `kernel/thread.c`, next to `thread_create_forked`.

**Template.** Graft `thread_create_forked` (`thread.c:525-599`) — take its Thread
alloc + `fork_frame_init(&t->ctx…, frame, child_sp)` + `t->ctx.tpidr_el0 =
child_tls` — but pass in the caller's `proc` and end like
`sys_thread_spawn_handler`:

```c
struct Thread *thread_create_forked_in_proc(struct Proc *proc,
                                            const struct exception_context *frame,
                                            u64 child_sp, u64 child_tls) {
    struct Thread *t = thread_alloc();          // as thread_create_user does
    if (!t) return NULL;
    // kstack, ctx.sp (kernel SP), ctx.lr = trap-return path -- copy from
    // thread_create_forked; it already sets these up for a forked frame.
    fork_frame_init(&t->trapframe, frame, child_sp);   // regs[0]=0, sp=child_sp
    t->ctx.tpidr_el0 = child_tls;                      // SETTLS
    thread_link_into_proc(t, proc);                    // SHARE the caller's Proc
    return t;                                           // caller does ready(t)
}
```

The exact field names (`->trapframe` vs `->ctx`, the kstack setup) — read
`thread_create_forked` and mirror them; I've sketched the shape, not the field
spelling. The important invariants: (a) `thread_link_into_proc(t, proc)` with
`proc = current_thread()->proc`, never a new Proc; (b) `regs[0]=0` via
`fork_frame_init` so the child's clone returns 0; (c) `tpidr_el0 = child_tls`.

**Test.** You can't easily unit-test thread creation in isolation (it needs a
Proc + the scheduler), so this piece is proven by the probe leg in §3.6/§4 —
the first time a thread actually *runs*. Build it now; verify it *compiles* and
that the suite still boots (you haven't wired it to anything yet, so nothing
should change at runtime).

### 3.4 — The clone shell (wire the THREAD mode)

**What.** In the clone dispatch, when the decide says THREAD, call your new core
instead of `sys_rfork_core`, and return the *tid*.

**Where.** `kernel/syscall.c:12120-12145` (`case VIV_LINUX_CLONE:`).

**Template.** The current shell:

```c
case VIV_LINUX_CLONE: {
    enum viv_clone_mode mode;
    if (vivarium_clone_decide(args[0], args[1], &mode) != VIV_TRANSLATED)
        return -(s64)T_E_NOSYS;

    if (mode == VIV_CLONE_MODE_THREAD) {
        struct Thread *cur = current_thread();
        struct Thread *nt = thread_create_forked_in_proc(
            cur->proc, ctx /* the trap frame */, args[1] /* child_sp */,
            args[3] /* tls */);
        if (!nt) return -(s64)T_E_AGAIN;

        if (args[0] & VIV_CLONE_PARENT_SETTID)          // publish ptid (x2)
            (void)uaccess_store_u32(args[2], (u32)nt->tid);
        if (args[0] & VIV_CLONE_CHILD_CLEARTID)         // arm the CLEARTID slot
            nt->clear_child_tid = args[4];              // ctid (x4)

        ready(nt);
        return (s64)nt->tid;                            // parent gets the tid
    }

    // existing fork/vfork path, unchanged:
    return sys_rfork_core(ctx,
                          mode == VIV_CLONE_MODE_VFORK ? (RFPROC | RFMEM)
                                                       : RFPROC,
                          args[1], 0);
}
```

Two details cross-checked against how the natives already do it:
`uaccess_store_u32(ptid, tid)` is exactly `sys_thread_spawn_handler`'s
CLONE_PARENT_SETTID publish (`kernel/syscall.c:4581-4582`); setting
`nt->clear_child_tid` is what `SYS_SET_TID_ADDRESS` does for native threads
(`kernel/syscall.c:4472`), and the exit-time consumer (§3.6) already reads it.

**Test.** After §3.6, a probe leg. For now: build + boot (no behavior change for
existing binaries, since none emit the THREAD flag word except through a real
`pthread_create`).

### 3.5 — SETTLS is already handled — just stop discarding the arg

**What.** The child's TLS. Good news: `fork_frame_init`/the ctx already carry
`tpidr_el0` (`kernel/thread.c:590`). The only bug waiting for you is that the
*fork* path deliberately ignores the tls register (it's "garbage" for a bare
fork, `vivarium.h:1977-1999`). For the THREAD mode you pass `args[3]` through
(you already did, in §3.4). Just make sure you don't route the thread case
through `sys_rfork_core`'s `child_tls==0 → inherit` fallback
(`kernel/syscall.c:9219-9303`) — the THREAD branch bypasses `sys_rfork_core`
entirely, so you're fine.

**Test.** Covered by the probe leg reading `errno`/a TLS-dependent value (§4).
A thread whose TLS is wrong crashes *fast* (musl dereferences `TPIDR_EL0` for
`errno` almost immediately), so "the thread ran and reported a sane errno" is a
strong TLS test on its own.

### 3.6 — PARENT_SETTID + CHILD_CLEARTID (mostly already built)

**What.** `ptid` you already publish (§3.4). `ctid` you already store (§3.4).
The *exit-time* half — write 0 to `*ctid` and futex-wake it — **already exists**
and fires automatically once `clear_child_tid` is set:

```c
// kernel/proc.c:2527-2547 -- thread_clear_child_tid_handoff, called from
// thread_exit_self (proc.c:3142). You write NOTHING here; you just had to set
// t->clear_child_tid in §3.4 and route the exit correctly in §3.7.
static void thread_clear_child_tid_handoff(struct Thread *t, struct Proc *p) {
    u64 tidptr = t->clear_child_tid;
    if (tidptr == 0) return;
    if (tidptr & 0x3u) return;
    if (uaccess_store_u32(tidptr, 0u) != 0) return;
    (void)sys_torpor_wake_for_proc(p, tidptr, (u32)~0u);   // wake the joiner's barrier
}
```

**Subtle but important — get the mental model right so you test the right
thing.** `pthread_join` does **not** wait on this `ctid` address. It waits on a
*userspace* word `t->detach_state` (`musl/src/thread/pthread_join.c:16-22`),
which the exiting thread wakes from userspace *before* it ever calls `SYS_exit`.
The `ctid` (`CLONE_CHILD_CLEARTID`) address musl passes is `&__thread_list_lock`
— a libc-internal global — and its job is `__tl_sync`'s **barrier**: it stops
`pthread_join` from `munmap`ing a thread's stack while the thread is still
executing its final instructions, because the lock only clears "after `SYS_exit`
has been called, via the exit futex address" (`musl/src/thread/pthread_create.c:142-144`).
So both mechanisms must work for join to be race-free, but they wake on
*different* addresses. Your job is just to make the kernel fire the CLEARTID
handoff at true thread retirement — which it already does.

**Test.** A probe that `pthread_create`s a thread that sets a shared flag, then
`pthread_join`s it and asserts the flag is set and join returned. If join hangs,
your CLEARTID handoff isn't firing (thread didn't reach `thread_exit_self`, or
`clear_child_tid` wasn't set). If join returns but you later crash, it's the
`__tl_sync` barrier / stack-reuse race.

### 3.7 — The exit path: `SYS_exit`(93) ≠ `SYS_exit_group`

**What.** A musl thread exits with `SYS_exit` (93), **never** `exit_group`
(`musl/src/thread/pthread_create.c:170-172`: `for(;;) __syscall(SYS_exit, 0);`).
Under the phenotype, `SYS_exit` must route to `thread_exit_self` (this thread
only; the Proc lives on if peers remain), and must stay distinct from
`exit_group` (which terminates the whole Proc, and which the phenotype already
maps).

**Where.** `kernel/vivarium.c` (add a `VIV_LINUX_EXIT` decode row, nr 93) +
`kernel/syscall.c` (shell it onto `thread_exit_self`,
`kernel/proc.c:3019-3162`). Check first whether `SYS_exit`(93) already has a row
— grep `VIV_LINUX_EXIT\b` (word boundary, to exclude `EXIT_GROUP`). If it maps
to the Proc-exit today, that's a *latent bug for threaded programs*: a
single-threaded phenotype process calling `exit(0)` via `SYS_exit` happens to be
fine because last-thread-out becomes the zombie anyway, but you want it
explicitly on `thread_exit_self` so a threaded program's worker exit doesn't
kill the Proc.

**Template.** `thread_exit_self` already does the right thing
(`kernel/proc.c:3019`): "if last live Thread → become_zombie; else just this
Thread → THREAD_EXITING + sched()." You only wire nr 93 → it. There's a native
`sys_thread_exit_handler` (`kernel/syscall.c:4592-4595`) to mirror.

**Test.** The join probe (§3.6) exercises exactly this — the joined thread must
reach `thread_exit_self` for the CLEARTID handoff to fire. If join hangs, this
routing is the first suspect.

### 3.8 — `gettid` and `futex` — the two real gaps

**What.** These don't exist at all yet, and musl's threading hits both
immediately (any mutex/cond-var, and `__tl_sync`'s raw `SYS_futex`).

- **`gettid` (nr 178)** → `current_thread()->tid`. Note `getpid()` stays
  correct unchanged, because CLONE_THREAD peers share one `Proc` and `getpid`
  reads `Proc.pid` (`kernel/include/thylacine/proc.h:174`), while `tid` is the
  separate per-Thread counter (`kernel/include/thylacine/thread.h:52`,
  `alloc_next_tid`).
- **`futex` (nr 98)** → shell `FUTEX_WAIT`/`FUTEX_WAKE` onto
  `sys_torpor_wait_for_proc` / `sys_torpor_wake_for_proc`
  (`kernel/torpor.c`, `kernel/include/thylacine/torpor.h:156,170`). Start with
  just `FUTEX_WAIT` (val, timeout) and `FUTEX_WAKE` (count) — the private,
  non-PI, no-`FUTEX_WAKE_OP` subset musl's basic locks use. The Linux
  `futex(uaddr, op, val, timeout, uaddr2, val3)` maps: `FUTEX_WAIT` →
  `torpor_wait(uaddr, val, timeout_us)`; `FUTEX_WAKE` → `torpor_wake(uaddr,
  count)`. Mind the `op` masking (`FUTEX_PRIVATE_FLAG` 0x80, `FUTEX_CLOCK_REALTIME`
  0x100 — strip them; you only need the low bits) and the timeout unit
  (Linux `struct timespec` vs torpor's microseconds — convert).

**Where.** Both are new `VIV_LINUX_*` enum rows + decode rows in
`kernel/vivarium.c` + shells in `kernel/syscall.c`. `gettid` is a trivial T2
(read a field, return it). `futex` is a T2 shell (validate `uaddr`, decode the
op, call torpor). Use the `ioctl`/`fcntl` T2 shells you've seen as the structural
template (decide → shell).

**Test.** `gettid`: a probe asserting `gettid() != 0` and (in a thread)
`gettid() != getpid()`. `futex`: the pthread mutex/cond path exercises it; a
focused probe can `FUTEX_WAIT` on a word another thread `FUTEX_WAKE`s. This is
I-9 (no-lost-wakeup) territory — see §6.

---

## 4. Build order + milestones

Do it in layers so you always have something that *works*, and each red result
points at exactly the layer you just added:

1. **Vocabulary + decide (§3.1–3.2).** Milestone: `vivarium.clone_decide` unit
   test goes green; `0x007D0F00 → THREAD`. Pure, no boot risk. *You've proven the
   translator recognizes a pthread.*
2. **The core + shell, thread that RUNS but can't join (§3.3–3.5, 3.7).**
   Wire the THREAD branch, route `SYS_exit`→`thread_exit_self`. Milestone: a
   probe that `pthread_create`s a thread which writes a shared global and
   `sleep`s/spins (no join yet), and the main thread polls the global. *You've
   proven a thread spawns, shares memory, and has working TLS* (it read `errno`
   without crashing). This is the big one — if the shared global flips, the crux
   (§2) is correct.
3. **Join (§3.6 + `futex` §3.8).** Add `gettid` + `futex`. Milestone: the probe
   `pthread_join`s and returns; no hang, no post-join crash. *You've proven the
   CLEARTID barrier + the futex path.*
4. **A real multithreaded binary.** Bake a tiny musl-static C program that spawns
   N threads, each incrementing a mutex-guarded counter, joins them all, and
   prints the sum. Run it under `viv`. *You've proven mutexes/cond-vars over
   futex end-to-end.* Then try npxf.

Each milestone is independently committable. Milestone 2 alone is a genuinely
useful landing (threads that run), even before join.

---

## 5. Testing — the ladder, with templates

- **Kernel unit tests** (pure functions): `vivarium.clone_decide`,
  and later `vivarium.futex_decide`/`vivarium.gettid`. Template:
  `test_vivarium_ioctl_decide` in `kernel/test/test_vivarium.c` (the freshest
  pure-decode test) + register in `kernel/test/test.c`. These run at boot via
  `tools/test.sh`.
- **A phenotype probe leg**: extend `usr/viv-pheno-probe` (the native probe that
  runs under `pheno=1`). Grep it for its existing legs (e.g. the `linux_exit(N)`
  / `brk` legs) — add a `pthread_create`+`pthread_join` leg that reports a marker
  string on success. This is the discrimination-provable witness: sabotage the
  thread core and the marker must vanish. Wire the assertion into `usr/joey`
  (the boot gate that reaps the probe), the way the other viv legs are asserted.
- **An E2E bundle**: bake a small multithreaded musl-static binary into a `viv`
  bundle (template: how `tools/build.sh` stages the `pheno` / `alpine` bundles,
  and the anchors you learned about the hard way — `proc/ sys/ dev/*`). Assert
  its stdout under a boot gate.
- **The SMP gate**: `tools/ci-smp-gate.sh`. Thread creation is *concurrency* —
  this is the load-bearing rigor. It multi-boots default + UBSan × smp4/smp8 and
  classifies corruption. Your thread path shares an AddrSpace across CPUs; the
  gate is where a refcount race or a TLS mixup surfaces. **Do not skip it.**
- **The holotype review**: this is an audit-bearing surface (thread lifecycle,
  I-24; the futex is I-9). When you're done, spawn the `holotype-reviewer` agent
  scoped to your diff. (If you want, I can run that round for you.)

---

## 6. Hazards + the audit surface

Thread creation lands on real invariants — know them before the auditor tells
you:

- **I-24 (group termination, exactly-once).** When the Proc is terminated
  (`exit_group`, a fatal note, a kill), *all* its threads must go down atomically
  and exactly once, with no thread left running EL0 after the Proc is a zombie.
  Your new threads are in the same Proc, so `proc_group_terminate` already covers
  them — but verify: a thread mid-`clone` racing a `proc_group_terminate` must
  not leave a half-linked Thread. The install (`thread_link_into_proc` + `ready`)
  ordering matters; mirror `sys_thread_spawn_handler`'s.
- **I-9 (no lost wakeup).** The `futex` shell is a wait/wake protocol. Register
  the waiter *before* re-checking the condition, exactly as torpor's native path
  does — `sys_torpor_wait_for_proc` already gets this right, so as long as you
  shell straight onto it and don't add your own pre-check, you inherit the
  correctness. Don't reimplement the wait loop in the shell.
- **I-32 (resource floor).** Per-AddrSpace page budget + `PROC_THREAD_MAX`
  (§2). A `pthread_create` storm must fail clean (`-EAGAIN`), never OOM the box.
- **TLS / SP banking.** `SP_EL0` is the user stack (I-21); the kernel runs on
  `SP_ELx`. `thread_user_trampoline` installs `sp_el0` and `tpidr_el0` *at the
  eret* (`arch/arm64/context.S:316-320`), and the context-switch save/restore
  (`context.S:91,141`) preserves `tpidr_el0` across preemption. Your forked-frame
  thread rides the normal trap-return, not the trampoline, so confirm the trap
  frame carries `sp_el0 = child_sp` and the ctx carries `tpidr_el0 = child_tls`
  — the two registers a thread cannot live without.
- **The `AddrSpace.page_budget` is shared, and that's correct** — two threads
  sharing an address space share the cap. `addrspace_charge_pages` reads the cap
  off the AddrSpace it charges; don't pass it as a parameter (I-32's rejected
  shape).

Read the audit-trigger rows for `thread_spawn / thread_exit`
(`docs/AUDIT-TRIGGERS.md`, grep "thread_spawn") and the VIVARIUM clone row before
you start — they enumerate the prosecution categories a reviewer will use.

---

## 7. After threads: the two socket gaps npxf also needs

npxf uses two socket shapes the phenotype refuses today. They're smaller than
threads and independent, so tackle them after milestone 3:

- **`SOCK_NONBLOCK`** (`net.cpp:118`) — currently refused at `socket()`
  (`kernel/vivarium.c:1628-1630`, the "asked for SOCK_NONBLOCK → refuse"
  branch). The socket layer would need a non-blocking mode (analogous to the
  `CNONBLOCK` work just done for pipes in the git-stash chunk — same idea, a
  different Dev). This is netd/weft territory, more involved than the pipe case.
- **`AF_UNIX`** (`net.cpp:171,194`) — the phenotype's socket domain check admits
  `AF_INET` only (`kernel/vivarium.c:1622`). npxf uses AF_UNIX for a local
  transport; if you can run it over AF_INET (loopback) instead, you sidestep this
  for now.
- **`getaddrinfo` by name** (`net.cpp:64`) — DNS-by-name is unimplemented
  (net-4d). Workaround: connect by numeric IP (or seed `/etc/hosts`), which
  `getaddrinfo` resolves locally without a DNS query.

Threads are the blocker that stops npxf cold; these three are what it hits
*next*. But get a thread running first — it's the fun part, and everything else
is downstream of it.

---

## Appendix: the musl thread ABI, in one place

```
pthread_create -> __clone(func, stack, flags, arg, &new->tid, TP_ADJ(new), &__thread_list_lock)
                  musl/src/thread/pthread_create.c:243-245,355

flags = 0x007D0F00 = CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|SETTLS
                     |PARENT_SETTID|CHILD_CLEARTID|DETACHED
                  musl/include/sched.h:53-67

aarch64 syscall(SYS_clone=220, x0=flags, x1=stack, x2=ptid, x3=tls, x4=ctid)
                  musl/src/thread/aarch64/clone.s
   child: resumes after svc with x0=0, on `stack`; pops func/arg; blr func
   thread exit: SYS_exit (93), NOT exit_group   (pthread_create.c:170-172)

pthread_join waits on t->detach_state (userspace word, SYS_futex FUTEX_WAIT)
             then __tl_sync waits __thread_list_lock -> 0, fired by the kernel's
             CLONE_CHILD_CLEARTID at true SYS_exit retirement
                  musl/src/thread/pthread_join.c:16-22, pthread_create.c:142-144

TLS read: mrs x, tpidr_el0   (musl/arch/aarch64/pthread_arch.h:4)
```

Kernel-side landmarks:
```
thread_create_user            kernel/thread.c:450   (entry-va shape; NOT the match)
thread_create_forked          kernel/thread.c:525   (frame-copy; but mints a new Proc)
fork_frame_init               kernel/thread.c:505   (regs[0]=0, sp=child_sp)
thread_link_into_proc         kernel/thread.c:102/408
thread_exit_self              kernel/proc.c:3019    (last-out zombie / else this thread)
thread_clear_child_tid_handoff kernel/proc.c:2527   (CLEARTID: write 0 + torpor_wake)
sys_thread_spawn_handler      kernel/syscall.c:4488 (arg validation + tid return template)
vivarium_clone_decide         kernel/vivarium.c:1327
case VIV_LINUX_CLONE          kernel/syscall.c:12120
sys_rfork_core                kernel/syscall.c:9219 (the fork wiring template)
sys_torpor_wait/wake_for_proc kernel/torpor.c ; torpor.h:156,170
```

Have fun. The moment that shared global flips in milestone 2, you'll have a
Linux thread running on Thylacine — everything after that is polish.
