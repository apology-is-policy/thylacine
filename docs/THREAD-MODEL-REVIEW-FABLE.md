> **Kept verbatim.** Below this note is the unedited report of the Fable 5.1 reviewer that read Thylacine's thread
> model on 2026-10-20, at the operator's request. It read `main` at 107c5c1bd; the snapshot root it names was a scratch
> copy of that commit, so its paths are repository paths at 107c5c1bd, and its line numbers may have moved since.
> Its section 7 judges the in-flight fix as it was described to the reviewer, a design that also swept at thread exit.
> The fix that landed on aux-3 (e7d6f2f47) frees exited threads only before a thread-budget charge (thread create and
> the Loom SQPOLL setup), never at exit; it claims each thread by unlinking it under `g_proc_table_lock`, skips kproc,
> and clears the debug focus under the lock. The joint overhaul plan built on this report, with aux's checks and the
> decisions it needs from the operator, is `docs/thread-model-review.html`.

MODEL(start): Fable 5.1 (claude-fable-5-1)

# Thylacine multi-threading model -- independent review (Fable round)

Snapshot: `main` at 107c5c1bd (2026-10-19), read-only. Reviewer family: Fable.
Implementer of the in-flight fix: Opus (aux). Cross-family round. Every claim
below was re-derived from the snapshot's code; comments, dossiers and specs
are quoted only as claims to check, and each disagreement with the code is
itself listed (section 4). Nothing was built, booted or model-checked: every
figure marked "reasoned" is reasoning from code, and the few numbers quoted
from in-tree comments are the tree's own measurements, not mine.

Paths below are relative to the snapshot root
`/private/tmp/claude-501/-Users-northkillpd-projects-thylacine-aux/27e7472f-d6a3-43e2-b1f6-c6fb20e34a39/scratchpad/mtreview/main-107c5c1bd/`.

---

## 1. The model as built

### 1.1 Objects and ownership

- **Thread** (`kernel/include/thylacine/thread.h:59-616`): 1760 bytes, pinned by
  `_Static_assert` (`thread.h:618`). Owns: register context `ctx` (656 B incl.
  512 B of V registers), a 32 KiB order-3 kstack whose low 16 KiB is a
  no-access guard (`thread.h:674-678`, `thread.c:396-413`), run state
  (`RUNNING/RUNNABLE/SLEEPING/EXITING`, `thread.h:31-37`), the EEVDF-lite key
  `vd_t`/`band`, the SMP claim `on_cpu` (`thread.h:158`), `cpu_pinned`, the
  wait registration (`rendez_blocked_on` + `wait_lock`, `thread.h:124,304`),
  the per-thread note state (`note_mask`, `in_handler`, 1 KiB+ of inline
  saved context incl. a 520 B FP area, `thread.h:256-290,615`), the
  `clear_child_tid` join word (`thread.h:227`), eleven per-subsystem bools
  (`exit_close_active`, `cons_frozen_unwound`, `kthread_join_active`,
  `closes_never_wait`, `debug_ss_armed`, `stop_unwinds`, `stop_no_park`,
  `stop_unwound`, `note_unwound`, `recv_caught_ok`, `note_interruptible`,
  `thread.h:372-507`), the debug park rendez + trapframe pointer
  (`thread.h:406,423`), telemetry (`run_ns`, `nsched`, ...,
  `thread.h:541-591`), `preempt_count` and `in_syscall` (`thread.h:321,348`).
- **Proc** is the thread group and the shared-resource container: `threads`
  (head-inserted doubly linked list) + `thread_count` (`proc.h:178,181`),
  one AddrSpace, one handle table, one Territory, one note queue
  (`proc.h:413`), `group_exit_msg` as the set-once die flag and status
  (`proc.h:447`), `group_exit_code`/`group_term_sig` (`proc.h:357,945`),
  `debug_stop_req`/`job_stop_req`, `debug_focus_thread` (`proc.h:583-643`),
  `loom_sqpoll_count` (`proc.h:928`).
- **tid** is a global monotonic `u32` counter (`thread.c:53,78-82`), never
  recycled, independent of the pid space; `kthread` is tid 0, each per-CPU
  idle takes the next tids (`thread.c:284,336`), so a Proc's main thread's
  tid is never its pid (`syscall.c:5218-5224` says so).

### 1.2 Creation

Three constructors, three trampolines, one link:

- `thread_create_user` (`thread.c:502-554`) for `SYS_THREAD_SPAWN`
  (`syscall.c:5254-5354`): argument gates (alignment, bounds), `proc_thread_cap_ok`
  (`proc.c:1268-1277`, under `g_proc_table_lock`), create, optional ptid
  publish before `ready()` (`syscall.c:5347-5352`). First switch-in lands in
  `thread_user_trampoline` (`arch/arm64/context.S:281-359`): releases the
  run-tree lock, runs `el0_return_die_check`, masks DAIF, erets.
- `thread_create_forked` (`thread.c:578-663`) for the phenotype's
  `clone(CLONE_THREAD)` (`syscall.c:5388-5468`): copies the parent's trap
  frame onto the child's kstack top, resumes through the shared exception
  return (`vectors.S:396-420`).
- `thread_create_with_arg` (`thread.c:384-473`) for kernel entries (rfork
  children's exec thunks, kthreads, the SQPOLL poller).
- Each constructor: `kmem_cache_alloc(KP_ZERO)` + `alloc_pages(order 3, KP_ZERO)`
  + `mmu_set_no_access_range` (demote + `tlbi vmalle1is`, `arch/arm64/mmu.c:1255-1276`)
  + `thread_kstack_poison` (16 KiB fill, `thread.c:144-151`) +
  `thread_link_into_proc` (`thread.c:102-115`, under `g_proc_table_lock`,
  `thread_count++`).
- `ready()` (`sched.c:910-925`) places on an idle peer in production
  (`select_idle_target`, `sched.c:444-458`) else locally; `ready_on`
  (`sched.c:823-908`) inserts into the band's sorted list under that CPU's
  `cs->lock`, sets wake-preempt or the target's `need_resched` + IPI.

### 1.3 Scheduling

- Per-CPU `struct CpuSched` (`sched.c:61-143`): three sorted lists, `vd_counter`,
  the two switch handoffs (`pending_release_lock`, `prev_to_clear_on_cpu`),
  `idle_in_wfi`.
- `sched()` (`sched.c:1264-1607`): mask IRQs, raw-acquire `cs->lock`, assert
  `preempt_count == 0`, dispatch on `prev->state` (RUNNING requeues at
  `vd_counter++`; SLEEPING/EXITING are not requeued, `sched.c:1426-1446`),
  `pick_next` else `try_steal` (claims the victim's `on_cpu` under the peer's
  lock, `sched.c:1233-1236`), set `next->on_cpu`, stash `prev` for the clear,
  `sched_install_asid_ttbr0`, `hwdebug_switch_in`, telemetry,
  `cpu_switch_context` (`context.S:68-186`: callee-saved + SP + TPIDR_EL0 +
  TTBR0 (+`isb`) + all 32 V registers + FPSR/FPCR), then on resume re-read
  `this_cpu_sched()` and clear `prev->on_cpu` (RELEASE) before releasing the lock
  (`sched.c:1586-1606`; the fresh-thread twin is `sched_finish_task_switch`,
  `sched.c:1112-1126`).
- Preemption: `sched_tick` (`sched.c:2738-2784`) decrements the 6-tick slice
  and sets `need_resched`; `preempt_check_irq` (`sched.c:2786-2846`) refuses
  while `preempt_count != 0` or `in_syscall` and otherwise calls `sched()`.
  Syscall bodies run IRQs-on and non-preemptible (`syscall.c:15615-15641`).
- `on_cpu` is the only inter-CPU edge guarding ctx/kstack reuse: set RELAXED
  before the switch, cleared RELEASE after it; `thread_free`, the reaper and
  `wake_rendez_waiter` spin on it (`thread.c:730-735`, `proc.c:6449-6451`,
  `sched.c:1931-1933`).

### 1.4 Synchronization

- Rendez: single waiter; `sleep_common`/`tsleep_common` (`sched.c:1990-2187,
  2219-2392`) take `t->wait_lock` outermost, publish `caught_wait`, register
  (`r->waiter`, `rendez_blocked_on`, SLEEPING), then re-check death
  (`thread_die_pending`, `notes.c:1343-1403`) and the caught-note predicate
  BEFORE dropping the locks and calling `sched()` -- the register-then-observe
  that `specs/death_wake.tla` models. Each sleep also detours into the stop
  park when a stop is pending (`sched.c:2041-2079`).
- torpor (the futex): one global `torpor_lock`, 64 buckets keyed by
  `(Proc*, VA)`, stack-allocated waiters, head-inserted (`torpor.c:70-83,
  244-246`); wait = lock-free prefault compare, under-lock compare, register,
  post-register die check, `tsleep_noteintr` (`torpor.c:145-336`); wake walks
  the bucket from the head under the global lock and calls `wakeup()`
  (`torpor.c:338-401`). `wakeup()` takes `g_timerwait.lock` then `r->lock`
  and may spin on the waiter's `on_cpu` under both (`sched.c:2850-2906`,
  `1931-1933`).
- Lock order as built: `g_proc_table_lock -> wait_lock -> g_timerwait.lock ->
  r->lock -> cs->lock`; `g_proc_table_lock -> torpor_lock -> g_timerwait ->
  r->lock`. I found no inversion in the files read (torpor drops its lock
  before `tsleep`; `wait_pid_for` drops the table lock before sleeping,
  `proc.c:6501-6516`).

### 1.5 Notes (signals)

- Notes are per-Proc (one queue, `proc.h:413`). Delivery happens at the
  EL0-return tail of whichever thread arrives there with the family unmasked
  (`notes_deliver_tail`, `notes.c:1808-1842`, via `current_thread()`), or at
  the thread the posting walk designated (`proc_caught_note_walk`/`caught_pick`,
  `proc.c:3329-3345, 3433-3477`: the poster, else the oldest live thread, else
  any awake thread, else an unwinding sleeper). `kill` is a group terminate
  (`postnote_kill_cascade_locked`, `syscall.c:5722-5726`).
- `tkill`/`tgkill` locate the Proc holding the tid (`proc_holds_tid_locked`,
  `syscall.c:5700-5704`) and then post a **Proc-wide** note
  (`viv_kill`, `syscall.c:13743-13795`).
- Group termination: `group_terminate` (`proc.c:5101-5190`) under
  `g_proc_table_lock`: revoke allowance, CAS `group_exit_msg`, torpor wake-all,
  per-peer `wait_lock` wake via `rendez_blocked_on`, broadcast IPI. Every
  thread dies at `el0_return_die_check` (`proc.c:5200-5259`) which calls
  `thread_exit_self`.

### 1.6 Exit and free

- `thread_exit_self` (`proc.c:4520-4685`): under `g_proc_table_lock`, count live
  peers (`proc_count_live_peers_locked` = peers not EXITING, `proc.c:4182-4189`);
  if last: read the group claim, mark `PROC_FLAG_EXIT_CLOSING`, drop the lock,
  close handles (`proc_close_handles_at_exit`, may sleep, `proc.c:4056-4094`),
  release the territory, `proc_become_zombie_locked` (`proc.c:3903-4004`);
  then `t->state = THREAD_EXITING`, `notes_exit_drop_locked`, unlock,
  `thread_clear_child_tid_handoff` (a `uaccess_store_u32` + torpor wake,
  `proc.c:3840-3860`), the srv/cap/weft notifies if last, `sched()`.
  **A non-last thread is never freed here** (`proc.c:4651-4684`): it stays in
  `p->threads`, counted in `thread_count`, with its kstack, until the Proc is
  reaped.
- `exits_term` (`proc.c:4344-4517`) is the whole-program exit: with live peers
  it runs `group_terminate` and falls into `thread_exit_self`; alone it does
  the same close/territory/zombie sequence inline (duplicated, see 4.1).
- Reap: `wait_pid_for` (`proc.c:6302-6536`) unlinks the zombie under the lock,
  then walks `zombie->threads` lock-free, spinning each thread's `on_cpu`
  and `thread_free`ing it (`proc.c:6447-6455`), then `proc_free`.
- `thread_free` (`thread.c:665-756`): refuse RUNNING/current/kthread/corrupt,
  `sched_remove_if_runnable`, re-check RUNNING, spin `on_cpu` (ACQUIRE),
  unlink under the table lock, restore guard pages (+ `tlbi vmalle1is`),
  free pages, free the slot.
- Kernel threads: `kproc`'s threads (idles, kthreads, the Loom SQPOLL poller)
  are never group-terminated; the SQPOLL kthread marks itself EXITING under an
  IRQ mask and is `thread_free`d by its joiner (`loom.c:386-396, 2746-2757`).
  SQPOLL kthreads are charged to their creating Proc's thread budget
  (`proc_sqpoll_charge`, `proc.c:1284-1302`).

### 1.7 Introspection and debug

- `/proc/<pid>/status` and `/ctl/procs` print `thread_count` lock-free
  (`devproc.c:252`, `devctl.c:254`); `/proc/<pid>/sched` walks `p->threads`
  under the table lock (`devproc.c:390-414`); `proc_cpu_ns` sums `run_ns`
  under the lock (`proc.c:1333-1339`).
- Debug reads go to `debug_focus_thread` if it is still in the list, else the
  list head (`devproc_focus_thread`, `devproc.c:1942-1949`); the head is the
  **newest** thread (head insertion, `thread.c:105-107`).
  `devproc_build_regs` refuses an EXITING head (`devproc.c:1966`).
  There is no per-tid `/proc` layer (`docs/DEBUG-FS-DESIGN.md:1077`).

### 1.8 Userspace

- Pouch (musl): `pthread_create` -> `SYS_THREAD_SPAWN(entry, sp, args, tls,
  &new->tid)`; `start()` -> `SYS_SET_TID_ADDRESS(&__thread_list_lock)`;
  `pthread_exit` -> `SYS_THREAD_EXIT` loop; `__unmapself` -> `SYS_BURROW_DETACH`
  + `SYS_THREAD_EXIT`; futex -> torpor (`usr/lib/pouch/patches/0004-pouch-pthread.patch`).
  `pthread_kill` is NOT retargeted (only `raise`/`kill` are, patch 0007 files
  list at `0007:172-883`); musl's `pthread_kill.c` still issues `SYS_tkill`,
  whose number is the `0xFFFF` ENOSYS sentinel (`0001-pouch-syscall-seam.patch:502`).
- libthyla-rs: raw `spawn_raw`/`exit_self`/`set_tid_address`/`join_tid`
  (`usr/lib/libthyla-rs/src/thread.rs`), and -- the one place the known
  defect is written down as accepted -- `thread.rs:38-41`: "The kernel keeps
  the Thread descriptor + kstack live until the Proc dies; v1.0 accepts that
  bound. Per-Thread reaping is a v1.x extension."
- Go: `newosproc` -> `thread_spawn` and `throw("newosproc")` on any error
  (`go-runtime/os_thylacine.go:402-409`); `exitThread` stores 0 to `*wait`
  then `SYS_thread_exit` (`sys_thylacine_arm64.s:54-60`); futex -> torpor;
  `osyield` -> `SYS_YIELD`.

---

## 2. Soundness findings

Severity: P0 actively broken; P1 latent broken (correct under exact coverage,
wrong under a realistic deviation); P2 hazard to land before merge; P3 nice.

### F-1 [P1] execve destroys the old address space while an EXITING peer may still be on-CPU writing through it

Invariant at stake: I-44 (address-space integrity), I-24's "exiting tails are
harmless" assumption, the #788 class (no ctx/stack/AS reuse while a CPU is
still using it).

State: Proc P with threads T1 (will exec) and T2 (exiting). T2 on CPU B runs
`thread_exit_self`; T1 on CPU A runs `sys_execve`.

1. T2 commits `t->state = THREAD_EXITING` under `g_proc_table_lock`
   (`proc.c:4651`) and unlocks (`proc.c:4656`). T2 is still `on_cpu == true`
   and still executing: its next instructions are
   `thread_clear_child_tid_handoff(t, p)` (`proc.c:4665`), i.e.
   `uaccess_store_u32(tidptr, 0)` (`proc.c:3858`) -- a store through the
   CPU's live TTBR0, which names P's OLD address space -- then
   `sys_torpor_wake_for_proc`, then `sched()`.
2. T1's exec admission `proc_exec_alone(p)` counts peers that are not EXITING
   (`proc.c:4740-4748`) -> 0 -> admitted (or T1 was already past admission:
   the re-check in `proc_exec_replace` at `proc.c:4930` also counts only
   non-EXITING peers).
3. `proc_exec_replace` swaps `p->as = nas` (`proc.c:4936`), installs the new
   root on CPU A only (`sched_activate_addrspace`, `proc.c:4999`), then
   `addrspace_release(old)` -- and if P was the only holder (the ordinary
   case), `addrspace_destroy(old)` (`proc.c:5031-5040`) drains the VMAs and
   frees the pages and page tables.
4. Nothing between steps 1 and 3 waits for T2 to be off-CPU. There is no
   `on_cpu` spin on the exec path (contrast `wait_pid_for`, `proc.c:6449-6451`,
   which spins before `thread_free`/`proc_free`). So T2's store in step 1 can
   land (a) through a still-valid TLB entry into a page `addrspace_destroy`
   is freeing -- a write into freed, possibly reallocated memory; or (b) via
   a demand-page fault: the kernel-mode uaccess fixup calls
   `userland_demand_page(current->proc, ...)` (`arch/arm64/exception.c:283-285`,
   `fault.c:1276`), which reads `p->as` -- now `nas` -- and installs a page in
   the NEW image at the OLD image's tid VA, writing a 4-byte zero into the
   new program's memory if a VMA covers it.

Violation: the new image's memory (or freed kernel memory) is written by a
thread of the old image. Window: T2's tail is microseconds; it is widened by
a slow demand page (the pool-reclaim pass `image_cache_reclaim` runs under
memory pressure before refusing an anon charge) or by a host vCPU stall of
CPU B -- the same amplifier the tree accepted as real for #788
(`thread.h:79-85`). Realistic deviation: a multi-threaded Linux program that
joins its last worker and immediately execs, under a build-load memory
squeeze. The L-2b exit criterion already names the missing property:
"a multi-thread Proc's peers are all EXITING **and off-CPU** before the swap"
(`docs/LINEAGE.md:1481`); the as-built L-2a admission checks only the first
half. The LINEAGE prose at `LINEAGE.md:537-540` claims the multi-thread case
is refused -- but an EXITING peer is not "multi-threaded" by
`proc_exec_alone`'s definition, so the refusal does not cover this case.

Fix: in `proc_exec_replace`, after the live-peer re-check under the lock and
before the swap, spin `__atomic_load_n(&peer->on_cpu, ACQUIRE)` to false for
every EXITING peer (the `wait_pid_for` discipline), and keep doing so in
L-2b. Add a `KERNEL_TESTS` window hook on the exiting tail (the
`exit_close_test_window` shape, `proc.c:4109-4126`) so a test can hold T2
between the EXITING commit and its `sched()` while T1 execs.

### F-2 [P2] An unprivileged thread-churning Proc can extinct the kernel (tid / pid exhaustion)

Invariant: I-32 ("fail clean, never extinct").

1. `alloc_next_tid` (`thread.c:78-82`): `u32 next = __atomic_fetch_add(&g_next_tid, 1u, ...); if (next >= 0x7fffffffu) extinction("alloc_next_tid: g_next_tid would overflow INT_MAX");`
   tids are never recycled anywhere (no free path writes `g_next_tid`).
2. The spawn path has no budget on cumulative creations; the per-Proc cap
   bounds only concurrent (unreaped) threads.
3. A loop of `pthread_create`/`pthread_join` by an unprivileged Proc advances
   the counter by one per iteration. Creation cost today (SLUB alloc, order-3
   buddy alloc, 48 KiB of writes, two system-wide TLBIs -- see E-1/E-2) is
   plausibly 10-50 us, so 2^31 spawns is on the order of 6 hours to a day of
   churn on one CPU -- hours, not years. The in-flight fix makes churn
   cheaper (no cap refusal), bringing the extinction closer.
4. The same shape exists for pids (`proc_alloc`'s INT_MAX guard,
   `proc.c:462-470` region) at a lower rate, and pid non-recycling is
   load-bearing for the session hangup (`sub-kernel-death.md:116`).

The comment at `thread.c:74-77` reasons about live counts ("< 10^5 in any
imaginable workload"), not cumulative allocations -- a comment true about the
wrong quantity.

Fix (immediate): make exhaustion a clean `-EAGAIN` from `SYS_THREAD_SPAWN`
and `-1` from rfork, never an extinction. Fix (overhaul): 64-bit internal
thread identity with a user-visible 31-bit tid that is recycled per-Proc with
a generation (or kept monotonic 64-bit internally and truncated only at the
Linux ABI boundary). Decide pid recycling separately (ABI; operator vote).

### F-3 [P2] Thread-directed signals are Proc-directed; native `pthread_kill` is unserved

Invariant: I-19 (delivery semantics), POUCH-DESIGN P-3 ("no silently-wrong
POSIX").

1. Phenotype `tkill(tid, sig)` / `tgkill(tgid, tid, sig)` -> `viv_kill`
   (`syscall.c:13743-13795`). The tid is used only to find the Proc
   (`proc_holds_tid_locked`, `syscall.c:5700-5704, 13753-13755`); the post is
   `postnote_self(p, name)` or `notes_post(target, ...)` -- a Proc-wide note.
2. Delivery: the note is taken by whichever thread reaches an EL0-return tail
   with the family unmasked (`notes_deliver_tail`, `notes.c:1808-1842`), or
   the designee of `caught_pick` (`proc.c:3329-3345`: poster first, then the
   oldest live thread). A `pthread_kill(worker, SIGUSR1)` meant to interrupt
   one blocked worker therefore wakes/handles on the caller or main instead.
   Linux `tkill` is thread-directed (`do_send_specific` -> the task's private
   `pending`).
3. Native pouch: `pthread_kill.c` is not in patch 0007's file list
   (`0007-pouch-signals.patch:172-883`), so musl's `__syscall(SYS_tkill, ...)`
   hits the `0xFFFF` sentinel (`0001:502`) -> ENOSYS; the patch header's
   "raise() / pthread_kill -> SYS_postnote" (`0007:184`) describes code that
   does not exist. `pthread_cancel` (SIGCANCEL via tkill) is likewise
   unserved -- documented as deferred in `POUCH-DESIGN.md:214`, but the
   Linux-phenotype half is silently wrong rather than refused.
4. Minor sibling: `proc_holds_tid_locked` matches EXITING-unreaped tids, so a
   `tgkill` to an exited thread succeeds where Linux returns ESRCH.

Fix: a per-thread pending set beside the Proc queue (Linux's `pending` vs
`signal->shared_pending`), thread-directed post for tkill/tgkill/pthread_kill,
the shared-pending pick kept for process-directed notes; serve `pthread_kill`
in pouch; `caught_target.tla` extended with a thread-directed family.

### F-4 [P2] The debug surface reads the newest thread and refuses when it has exited

Invariant: I-39 (stopped-only inspection of a multi-thread target).

1. `p->threads` is head-inserted (`thread.c:104-107`): the head is the most
   recently created thread, not main.
2. `devproc_focus_thread` falls back to `target->threads` when no bp/step
   focus is set (`devproc.c:1942-1949`) -- the manual `stop`/attach case.
3. `devproc_build_regs` returns 0 when that head is EXITING (`devproc.c:1966`)
   -> `r->result = -1` (`devproc.c:2098`).
4. A Go target whose most recently created M exited (an exited
   `LockOSThread` goroutine; any `mexit`) keeps that M as an EXITING,
   unreaped head forever (today's model). A debugger that attaches and
   `stop`s reads `regs`/`fpregs`/`kregs` and gets -1 although every live
   thread is parked. `docs/DEBUG-FS-DESIGN.md:1402` describes the head as
   "TID==PID" -- false twice (tid != pid; head = newest).

Fix: the in-flight fix removes the exited head; independently, make the
fallback `main` (the oldest live thread, as `caught_pick` already computes),
and land the per-tid layer (`/proc/<pid>/thread/<tid>/`) so the debugger
chooses.

### F-5 [P2] `PROC_THREAD_MAX = 256` is the wrong bound for the Go M model, and Go treats it as fatal

Invariant: I-32 is a floor, but its value must not refuse a correct program.

1. `proc_thread_cap_ok` refuses at 256 counting every unreaped thread plus
   SQPOLL kthreads (`proc.c:1268-1277`).
2. Go spawns an M per blocked syscall-holding goroutine (its own ceiling is
   10000); on `-EAGAIN` `newosproc` throws (`os_thylacine.go:404-408`) -- the
   process dies. Even after the in-flight fix, 256 concurrently blocked Ms is
   reachable by a server with a few hundred blocking readers.
3. The resource actually being protected is unswappable kstack memory, and
   that memory is `alloc_pages` (`thread.c:396`), not `alloc_user_pages`: it
   is charged neither to the Proc's page budget nor to the I-32 user pool.
   Two Procs at the cap pin 16 MiB the pool never sees.

Fix: charge kstacks to the pool (physical, like every other user page since
B-1a'), raise the count default (4096) and make it a spawn-perm-raisable
budget like `page_budget`; keep a count only as the DoS floor.

### F-6 [P3] The exit tail is preemptible on the IRQs-on, unmarked paths

Invariant: the implicit "a thread that committed EXITING runs its tail to
`sched()`" (`seam-exiting-tails-never-sleep` records the sleep half only).

1. `preempt_check_irq` (`sched.c:2786-2846`) gates on `preempt_count` and
   `in_syscall` only; `sched()` accepts `prev->state == THREAD_EXITING`
   (`sched.c:1350-1354`) and simply does not requeue it.
2. `userland_enter` calls `el0_return_die_check` with IRQs ENABLED from a
   kthread-shaped thunk (`arch/arm64/userland.S:50-63`; `in_syscall == 0`,
   `preempt_count == 0`); the exec-failure `exits("fail-exec")` sites
   (`syscall.c:8592, 8908, 9711`) run in the same context.
3. If a group kill lands on a child still in its exec thunk, the die-check
   runs `thread_exit_self` IRQs-on: between `t->state = THREAD_EXITING` +
   unlock (`proc.c:4651-4656`) and `sched()` (`proc.c:4683`), a cross-CPU
   `ready_on` can set this CPU's `need_resched` (`sched.c:905`) and IPI it;
   the IRQ return runs `preempt_check_irq` -> `sched()` -> the EXITING thread
   is switched out for good from inside the IRQ path.
4. Skipped work: `srv_proc_exit_notify`, `cap_proc_exit_notify`,
   `weft_share_release_owner` (`proc.c:4671-4675`). A pending `/cap` grant
   targeting the dying child is never cleared (a table-slot leak;
   `devcap.c:473-484`). The outcome is otherwise identical to a voluntary
   switch, which is why this is P3.

Fix (structural): `preempt_check_irq` returns without consuming the flag when
`t->state != THREAD_RUNNING` -- a thread that has committed EXITING (or
SLEEPING) is instructions away from its own `sched()`. Also move the three
notifies before the EXITING commit (as `exits_term` already does at
`proc.c:4374-4389`).

### F-7 [P3] A thread spawned into a stopped Proc runs EL0 code until its first trap

`thread_user_trampoline` runs the die-check but no stop check
(`context.S:286-312`); `userland_enter` likewise (`userland.S:63`; the held
spawn `userland_enter_held` does run the stop leg, `vectors.S:455-471`). A
`SYS_THREAD_SPAWN` racing a debug or job stop produces a thread that runs for
up to one tick before `el0_return_stop_check` parks it. `devproc_all_threads_parked`
correctly reports "not settled" meanwhile (`devproc.c:1800-1818`), so no
stopped-only read is served early; the cost is a Ctrl-Z'd process executing a
little. Fix: route every first entry through the one tail (the
`userland_enter_held` shape: build a frame, take `.Lel0_return`).

### F-8 [P3] Thread spawn is admitted in a dying Proc

`sys_thread_spawn_handler` and `viv_clone_thread` never read
`group_exit_msg` (`syscall.c:5254-5354, 5388-5468`). A thread created after
the group claim allocates a kstack, takes a cap slot, is placed, dispatched,
and dies at its trampoline die-check -- wasted work and, under a kill of a
spawn-looping Proc, a steady stream of 32 KiB allocations until the spawner
itself reaches a tail. Linux `copy_process` refuses with EAGAIN under
`signal_group_exit`. Fix: refuse with `-EAGAIN` when `group_exit_msg != NULL`
(read under the lock the cap check already takes).

### F-9 [P3] torpor wakes LIFO

Waiters are head-inserted (`torpor.c:245-246`) and the wake walks from the
head (`torpor.c:389-397`): `wake(1)` wakes the newest waiter. musl's mutex
slow path is `__wait`/`__wake(l, 1)`; under sustained contention the oldest
waiter can be starved indefinitely. Linux futex wakes FIFO within priority.
Not an I-8 violation (I-8 is about runnable threads) -- a fairness defect.

### F-10 [P3] torpor keys on `(Proc*, VA)`, not the address space

`torpor_hash` (`torpor.c:80-83`). Two Procs sharing an AddrSpace
(`rfork(RFPROC|RFMEM)`, `proc.c:418-420`) cannot wake each other. Dormant
because the vfork parent is parked (`proc.c:1990-1991`), but it is the wrong
key for the sharing model L-3 built; key on `(AddrSpace*, VA)`.

### Withdrawn after checking

- "The clear-child-tid store can sleep after EXITING and trip `sleep: current
  is not RUNNING`": withdrawn. `demand_page_locked` refuses a write to a
  non-writable VMA before the FILE arm (`fault.c:430-431` precedes `:547`),
  writable file mappings do not exist, and the anon/COW arms do not sleep.
- "Two threads can both count zero live peers": withdrawn; the EXITING
  commit and the count are under one lock (`proc.c:4552-4555, 4651`), the
  inv-i24 argument holds in the code.
- "A peer can appear during the final close": withdrawn; every spawner is a
  RUNNING thread of the Proc and the closer is the only one.

---

## 3. Efficiency findings (all reasoned from code; nothing measured by me)

### E-1 [P2-efficiency] Every thread create and every thread free flushes every CPU's entire TLB

`mmu_set_no_access_range` and `mmu_restore_normal_range` end in
`tlbi_vmalle1is` (`arch/arm64/mmu.c:1273, 1296`) -- "invalidate all stage-1
EL1&0 entries, all ASIDs, inner shareable". Thread create (`thread.c:409`)
and `thread_free` (`thread.c:748`) each issue one. A `pthread_create`/`join`
storm (libxml2's thread test, OpenRCT2's job pool) therefore flushes every
other Proc's user TLB on every CPU twice per thread lifetime. Linux avoids
this with a per-CPU cache of vmapped stacks (`cached_stacks`) and never
toggles direct-map permissions on the hot path. Fix: a kstack cache (free
list of guarded 32 KiB blocks, guard left no-access, bounded size); the
demote and both TLBIs leave the hot path.

### E-2 48 KiB of stores per thread create

`alloc_pages(..., KP_ZERO)` zero-fills all 32 KiB including the 16 KiB
guard that is then made no-access (`thread.c:396-413`), and
`thread_kstack_poison` overwrites the usable 16 KiB (`thread.c:427`). Drop
KP_ZERO (the poison is the only content that matters; `struct Thread` is
KP_ZERO separately).

### E-3 Half of per-thread kernel memory is guard

16 KiB guard per 16 KiB stack because the guard must be whole pages of an
order-3 direct-map block (`thread.h:674-678`). 256 threads = 4 MiB of guard.
The ARCH 8.12 watermark says typical depth is ~6 KiB. A kstack VA arena
(vmalloc-like, 1 guard page, non-contiguous backing) would halve it; the
direct-map model makes that a real chunk, not a tweak.

### E-4 Eager FP/SIMD save+restore on every switch

`cpu_switch_context` stores and loads all 32 Q registers + FPSR/FPCR with
two `isb`s on every switch (`context.S:98-183`), including kthread<->kthread
and idle switches where no V register is live (the kernel is
`-mgeneral-regs-only`, `context.S:40-47`). Linux arm64 defers the restore to
return-to-user (`TIF_FOREIGN_FPSTATE`) and skips kernel threads. ~1 KiB of
memory traffic per switch; at the tick-driven and futex-driven switch rates
of a Go build this is not free.

### E-5 Sibling threads pay a full address-space switch

`sched_install_asid_ttbr0` + `msr ttbr0_el1; isb` run on every switch
(`sched.c:1496`, `context.S:148-150`) even when `prev->proc->as ==
next->proc->as` (two threads of one Proc). Linux skips `switch_mm` when the
mm is unchanged. The ASID fast path is a CAS (`asid.c:240-245`); the `isb`
and the `dsb ish` in `sched_record_space` (`sched.c:1001`) are the cost.
`hwdebug_switch_in` is cheap when nothing is loaded (`hwdebug.c:279-297`).

### E-6 One global IRQ-off lock on the thread hot paths

`g_proc_table_lock` is taken twice per spawn (`proc.c:1271`, `thread.c:103`),
once per exit (`proc.c:4552`), once per `tkill` (`syscall.c:13753`), per
`thread_free` (`thread.c:118`), and across every `/proc` and `/ctl/procs`
formatting walk (`proc.c:919-924`). All Procs' thread creation and exit
serialize on one lock held with IRQs masked. A per-Proc `threads_lock`
(leaf under the table lock) would scope it to the tree; the death cascade's
"caller holds `g_proc_table_lock`" contract would become "holds
`p->threads_lock`".

### E-7 torpor: one global lock held across cross-CPU spins

`sys_torpor_wake_for_proc` holds `torpor_lock` across `wakeup()`
(`torpor.c:351-399`), which may spin on the waiter's `on_cpu`
(`sched.c:1931-1933`) -- every futex op on every CPU then waits on one
cross-CPU switch completion (the documented F2 hazard, `torpor.c:379-387`);
`torpor_wake_all_for_proc` walks all 64 buckets (`torpor.c:413-426`). Per-bucket
locks and collect-under-lock/wake-after are the standard shape.

### E-8 The timer-wait list is global and scanned every tick on every CPU

`timerwait_tick` runs from every CPU's 1 kHz tick under `g_timerwait.lock`
(`sched.c:2459-2498, 2747`), O(timed sleepers) with a from-head rescan per
wake. Every `pthread_cond_timedwait`, every Go `usleep`, every torpor
timeout is on this list. Linux keeps per-CPU hrtimer bases.

### E-9 The run tree is an O(N) sorted list

`insert_sorted` (`sched.c:618-642`). Fine at tens of threads; at the 4096
the cap redesign (F-5) would allow, a wake costs O(N) under `cs->lock`.
Recorded seam (`seam-runq-rbtree`).

### E-10 The in-flight fix's sweep is a latency spike on the spawn path

Freeing up to 255 exited peers inside one `SYS_THREAD_SPAWN` (non-preemptible
syscall body) costs 255 x (lock + guard restore + system-wide TLBI + buddy
free) -- milliseconds, and 255 global TLB flushes -- on the creator's thread.
E-1's cache removes most of it; otherwise the sweep must be bounded per call.

### E-11 `struct Thread` 1760 B, two per 4 KiB slab

~1 KiB of it is inline note-delivery save state (`thread.h:272-290, 615`),
justified as alloc-free delivery. The kstack is also alloc-free: the save
block could live at the kstack top (Linux keeps the signal frame on the user
stack and the pt_regs on the kernel stack). Minor.

---

## 4. Elegance findings

### 4.1 Two exit functions, one algorithm

`exits_term` (`proc.c:4344-4517`) and `thread_exit_self` (`proc.c:4520-4685`)
both contain: lock, count live peers, mark `EXIT_CLOSING`, drop lock, close
handles, re-check, release territory, re-check, `proc_become_zombie_locked`,
EXITING, unlock, handoff, `sched()`. `exits_term` already delegates to
`thread_exit_self` when peers exist or a claim exists (`proc.c:4424-4448`).
They are one function parameterized by (code, sig, msg, claim-now?).

### 4.2 The Thread is a bag of per-subsystem latches

Eleven bools owned by 9p_client, pipe, srvconn, cons, loom, dev9p, notes and
exec (`thread.h:372-507`; owners per `grep`: `stop_unwinds` 9p_client,
`stop_no_park` 9p_client+pipe, `note_unwound` 9p_client+srvconn,
`recv_caught_ok` 9p_client+pipe+srvconn, `cons_frozen_unwound` cons,
`kthread_join_active` loom, `closes_never_wait` loom+dev9p,
`exit_close_active` loom+notes+proc). Each is owner-only and individually
sound; together they make the sleep primitives a five-way dispatcher
(`enum sleep_unwind` x stop detour x `caught_wait` kind x `stop_unwinds` x
the held flags, `sched.c:1950-2187`). Linux expresses the same space as a
three-point lattice (INTERRUPTIBLE / KILLABLE / UNINTERRUPTIBLE) plus
`signal_pending`/`fatal_signal_pending`. A per-wait descriptor on the
sleeper's stack (`struct Wait { rendez, class, flags }`, reached via
`rendez_blocked_on`) would move these off the Thread.

### 4.3 Two "thread counts", documented three ways

`thread_count` counts unreaped threads (`thread.c:113,134`); live = 
`proc_count_live_peers_locked`. `proc.h:112` says "`PROC_THREAD_MAX` -- live
threads"; `vault/invariants/inv-i32.md:39` says "live threads | `thread_count`";
`vault/system/kernel/execution/sub-kernel-proc.md:646-649` says "`thread_count`
is not 'live threads'. It counts unreaped threads". The proc.h/inv-i32 reading
is the one the code does not implement, and it is the root of the operator's
freeze.

### 4.4 The scripture's thread model is not the as-built one

- `ARCHITECTURE.md:713-741` (7.3) lists `STOPPED`, `struct EventQueue
  events`, `struct ErrStr errstr`, `virtual_eligible`, `int cpu` -- none
  exist; the real struct has none of them and 40 fields the section does not
  mention.
- `ARCHITECTURE.md:747-761` (7.4): "RFMEM: Share address space (creates a
  thread within current Proc, not a new Proc)" and "pthread_create():
  rfork(RFPROC|RFMEM|...)" -- as built, `rfork(RFPROC|RFMEM)` creates a NEW
  Proc sharing the space and parks the parent (vfork, `proc.c:1961-1991`);
  threads come from `SYS_THREAD_SPAWN`/`clone(CLONE_THREAD)`.
- `ARCHITECTURE.md:4577-4586` (23.4): pthread "maps onto rfork(...)";
  `FUTEX_WAIT_BITSET`/`CMP_REQUEUE` "supported at Phase 5" -- `viv_futex`
  returns ENOSYS for them (`syscall.c:5483-5484`).
- `ARCHITECTURE.md:995-996` (7.9): "1. All threads stopped" -- threads
  self-terminate at their tails; nothing stops them.
- `IDENTITY-DESIGN.md:794` still lists `PROC_PAGE_MAX` 65536 pages, removed
  at B-1a'.
- `DEBUG-FS-DESIGN.md:1402`: head thread "TID==PID" (see F-4).
- `0007-pouch-signals.patch:184`: `pthread_kill -> SYS_postnote` (see F-3).
- `libthyla-rs/src/thread.rs:38-41` is the only honest statement of the
  known defect ("v1.0 accepts that bound").

### 4.5 tid and pid are unrelated spaces

The main thread's tid is "whatever `g_next_tid` was" (`syscall.c:5218-5224`);
`gettid() == getpid()` -- which every Linux program may assume for the main
thread -- is false. `caught_pick` reconstructs "main" by list order
(`proc.c:3331-3333`) because the Proc has no `main` pointer.

### 4.6 Reclamation is nobody's step

A non-last thread's exit changes `state` and nothing else; its memory is the
reaper's problem, and the reaper is the parent's `wait` -- an event unrelated
to the thread. The in-flight fix makes it a sibling's problem instead. Plan 9
and Linux both free the dead context from the scheduler's next switch
(section 5).

### 4.7 Three trampolines, two of which skip a tail leg

`thread_trampoline`, `thread_user_trampoline`, `thread_fork_trampoline`
(`context.S:220-244, 281-364`, `vectors.S:398-420`) plus `userland_enter` and
`userland_enter_held` (`userland.S`, `vectors.S:457-472`). Only the last runs
the full EL0-return tail. One "first entry" that builds a frame and takes
`.Lel0_return` would delete two eret sequences and F-7.

### 4.8 `thread_switch` is a second scheduler

A test-only direct switch (`thread.c:758-873`) re-implements the on_cpu /
IRQ-mask / ASID / finish-switch protocol with its own `#101` regression hook
global. It should become a test that calls `sched()`.

---

## 5. Heritage and state of the art

**Plan 9.** Only procs; `rfork(RFMEM)` makes a new proc sharing memory (the
thread library is user-level). Exit: `pexit` sets `up->state = Moribund` and
calls `sched()`; the *next* `schedinit()` on that CPU sees the previous proc
Moribund, releases its MMU state and returns the Proc to the free list -- the
dead context is reclaimed by the first switch after it, by the thread that
inherits the CPU, never by the parent. The parent receives a `Waitmsg` queued
at exit; no zombie struct lingers. `postnote` takes the sleeper's `p->rlock`
to read `p->r` and wake it -- the shape Thylacine's `wait_lock` copies
(`ARCHITECTURE.md:1393-1394`). Notes are per-proc. Thylacine follows Plan 9
on the sleep/wake and death cascade; it departs on reclamation (Plan 9 frees
at the next switch; Thylacine at reap) and on the proc/thread split (Plan 9
has none; Thylacine's Thread-in-Proc is the 9front/Linux shape).

**Linux.** Thread groups: `tgid`, `signal->live` (the live count),
`thread_group` list, `CLONE_THREAD`. Exit: `do_exit` -> `exit_notify` ->
`release_task` for every non-leader thread immediately (autoreap; the
leader stays until `wait`); the kernel stack is freed after the final switch,
in `finish_task_switch` when `prev->state == TASK_DEAD` (`put_task_stack`,
deferred through RCU), and stacks come from a per-CPU vmapped-stack cache.
Signals: per-task `pending` + per-group `shared_pending`; `complete_signal`
chooses a thread that `wants_signal`; `tkill` is thread-directed; `kill` is
group-directed. Exec: `de_thread` SIGKILLs the other threads
(`zap_other_threads`) and waits on `notify_count` until they are gone before
the swap. `pid_max` with bitmap recycling; `RLIMIT_NPROC` per user, no
per-process thread cap. Lazy FPSIMD restore; `switch_mm` skipped when the mm
is unchanged; futex buckets with per-bucket locks and FIFO plist. Thylacine
should follow Linux on: autoreap at the switch, thread-directed signals,
`de_thread`'s wait-for-dead, lazy FP, same-mm switch, futex bucket locking
and FIFO. It should not follow Linux on tid recycling without a generation
(Thylacine's non-reuse is a real safety property its `stripes`/pid
reasoning already relies on), nor on `ptrace`'s per-thread attach model (the
Proc-level attach with a focus thread is cleaner for a debugger that owns the
whole target).

**Zircon.** Threads and processes are kernel objects reached by handles;
`zx_task_kill` sets `THREAD_SIGNAL_KILL` checked at kernel exit (the
flag-and-self-terminate model ARCH 7.9.1 cites); a process exits when its
last thread exits; a dead thread's kernel stack is freed by the reaper
(`dead_threads` drained by a kernel thread), never by the exiting thread;
futexes are keyed by VA within the process; there is no per-process thread
count, only memory. Thylacine's capability conviction argues for Zircon's
*object* shape: a thread handle (`KObj_Thread`) in the handle table would
carry join/kill/debug authority explicitly instead of tid-plus-parent gates
(`postnote_walk_cb`'s `target->parent != w->caller`, `syscall.c:5746`), and
would give `pthread_join` a real primitive (wait on the handle) instead of
the `clear_child_tid` futex idiom.

**seL4.** TCBs are capability objects; the kernel has one per-core stack
because it never blocks. That shape is unavailable to a blocking 9P kernel,
which is why Thylacine (like Plan 9 and Linux) needs a kernel stack per
thread -- and why that stack's lifetime, not the Thread struct's, is the
load-bearing resource question.

**Where Thylacine's convictions argue for its own answer.** (a) Verified
complexity: the death-wake cascade is modelled (`death_wake.tla`) and should
stay the one termination primitive; thread-directed signals and autoreap must
land as extensions of that model, not beside it. (b) Capability authority:
thread control (kill, debug, join) should flow through handles or the existing
two-axis gates, never through tid guessing. (c) Plan 9 heritage: the
Proc remains the resource container and the note queue is per-Proc by
default; thread-directed notes are an addition for the phenotype and
`pthread_kill`, not a replacement.

---

## 6. Overhaul proposal

### 6.1 Target model

**Object.** `struct Thread` stays the schedulable unit; `struct Proc` stays the
group and the shared-resource container. Add `Proc.main` (the group leader).
Give threads a 64-bit kernel identity (`u64 tid64`, never reused) and a
user-visible `tid` whose policy is an ABI decision (6.4). The per-subsystem
latches move into a per-wait descriptor on the sleeper's stack.

**Lifecycle.** States `RUNNING -> {RUNNABLE, SLEEPING} -> ... -> EXITING ->
DEAD`. EXITING = committed to exit, still on a CPU; DEAD = final switch-out
complete (`on_cpu` cleared) and reclaimable. The transition EXITING->DEAD and
the hand-off to the reaper happen at the one point in the kernel that KNOWS
the context is off-CPU: the resume path after `cpu_switch_context`
(`sched.c:1586-1606`, `sched_finish_task_switch`), when `prev->state ==
THREAD_EXITING`. There, after the RELEASE clear of `prev->on_cpu` and the
`cs->lock` release, push `prev` onto a per-CPU dead list (intrusive, via
`runnable_next`, which is unused for a dead thread). A per-CPU reaper drains
it with IRQs on and no run-queue lock held: unlink from `p->threads` under
the Proc's thread lock, fold `run_ns` into `p->cpu_ns_dead`, clear
`p->debug_focus_thread` if it pointed here, return the kstack to the cache,
free the slot. Reaper = the idle thread before it parks (`sched_idle_park`)
plus one kthread per CPU for busy CPUs (or the next `sched()` entry with IRQs
on; the lock order `g_proc_table_lock -> ... -> cs->lock` forbids freeing
inside the switch's lock hold, so the drain must run outside it). This is
Plan 9's `schedinit` and Linux's `finish_task_switch` shape, and it makes
reclamation prompt (one switch), sibling-independent and lock-order-clean.
Every thread autoreaps, including the last: the ZOMBIE transition stays in
`thread_exit_self`, `wait_pid_for` frees only the Proc and waits (spins or
sleeps on a per-Proc "threads_reclaimed" condition) for `thread_count == 0`.
Kernel threads with a joiner (the SQPOLL poller) are marked `joined` and
skipped by the reaper; `loom_free` keeps its join.

**Ownership.** A Thread is freed exactly once, by its reaper (or its joiner),
only when DEAD. Every holder of a `Thread *` other than the owner reaches it
through `p->threads` under `p->threads_lock` and finishes its use under that
hold; a pointer compared by identity after a free is forbidden (hence the
focus clear above, or compare `tid64`).

**Locking.** `g_proc_table_lock` guards the tree, parentage, ZOMBIE
transitions. New `Proc.threads_lock` (leaf under it) guards `threads`,
`thread_count`, the live count, the EXITING commit and the dead-list hand-off.
The death cascade and the caught-note walk take `g_proc_table_lock ->
threads_lock -> wait_lock -> ...`. torpor: per-bucket locks, collect then
wake, keyed on `(AddrSpace*, VA)`, FIFO.

**Accounting.** `thread_count` = threads not yet reclaimed (transiently
> live by at most one per CPU in flight). The cap counts exactly that and is
honest within one switch. kstack bytes are charged to the physical user pool
(`alloc_user_pages`), the per-Proc count default rises (4096) and is a
spawn-perm-raisable budget like `page_budget`; SQPOLL kthreads keep sharing
it. tid exhaustion is `-EAGAIN`.

**Signals and notes.** Keep the per-Proc queue for process-directed notes and
the designation walk (`caught_target.tla`). Add a per-thread pending word for
thread-directed notes (tkill/tgkill/pthread_kill, SIGCANCEL): posted under the
target's `wait_lock`, woken by the existing rendez wake, delivered only at
that thread's tail. `kill` stays the group terminate.

**Debug.** `/proc/<pid>/thread/<tid>/{regs,fpregs,kregs,kstack,ctl}`; the
Proc-level files report `main` (not the newest); the focus thread remains
for bp/step fires. All three first-entry paths take `.Lel0_return`, so a
thread born into a stop parks before its first instruction.

**Exec (L-2b).** The execer posts the group-wide die (a new per-thread
`die` bit in the pending word, not `group_exit_msg`, which must stay
set-once for a real kill), wakes and kicks peers exactly as
`group_terminate` does, then waits until every peer is DEAD and reclaimed
(`thread_count == 1`) before `proc_exec_replace`. That also fixes F-1 by
construction.

**Limits.** No extinction reachable from a creation path. Tid/pid exhaustion
-> errno. The watermark and the guard stay.

### 6.2 What to delete

- The non-last branch's "leave it in the list" (`proc.c:4651-4684`) and the
  `wait_pid_for` thread walk (`proc.c:6447-6455`).
- `exits_term`'s duplicated last-out block (`proc.c:4470-4491`) -- fold into
  `thread_exit_self(code, sig, msg)`.
- `thread_switch` + `g_thread_switch_test_window_ns` (`thread.c:758-873`).
- `thread_user_trampoline`'s and `userland_enter`'s private eret sequences
  (replace with the frame-building first entry).
- `mmu_set_no_access_range`/`mmu_restore_normal_range` on the thread hot
  path (the cache keeps guards established).
- The eleven per-subsystem Thread bools, once the wait descriptor exists.
- `proc_count_live_peers_locked`'s role as the only "live" definition --
  replace with `p->live_threads`, maintained at the EXITING commit.

### 6.3 Invariants and specs

- New §28 row (needs operator signoff; proposed I-48 or an I-24 clause):
  "**Thread reclamation**: a Thread's context and kstack are freed exactly
  once, only after its final switch-out has completed (`on_cpu` false) and
  only by its designated reaper or joiner; no address space is destroyed or
  swapped while a thread of its Proc is on a CPU; `thread_count` exceeds the
  live count by at most the number of threads in flight between their final
  switch-out and their reclamation."
- `specs/thread_reap.tla` (new, spec-first): threads, CPUs, states incl. DEAD,
  the dead list, the reaper, `wait_pid`, `exec`; invariants FreedOnce,
  NeverFreeOnCpu, NoHolderAfterFree (the debug reader as a holder), and the
  liveness EventuallyReclaimed; buggy cfgs: free-while-on-cpu (the #788
  shape), double-reaper (the in-flight fix's wait_pid race), exec-before-dead
  (F-1).
- Extend `death_wake.tla` with the reap step so `ZombieImpliesAllDead` becomes
  "ZOMBIE implies all dead AND reclaimed before `proc_free`".
- Extend `caught_target.tla` with thread-directed families (a designation
  that cannot be handed off).
- `torpor.tla` (new): per-bucket locks + collect-then-wake; the 2026-05-23
  suspension allowed prose for the original, but the overhaul changes the
  lock structure of an I-9 surface -- it should carry its own model.
- Re-run buggy cfgs: `death_wake_buggy`, `sched_alpha`, `sched_oncpu_*`,
  `debug_stop_buggy_*` (the focus-thread and stop-at-birth changes),
  `caught_target_buggy_*`.

### 6.4 ABI / format changes (each needs the operator's signoff)

1. `gettid()` of the main thread == `getpid()` (tid space change).
2. tid policy: (a) keep monotonic non-reuse with a 64-bit internal id and
   `-EAGAIN` at 2^31 (no reuse ever, exhaustion becomes a clean failure); or
   (b) per-Proc recycling with a generation. (a) preserves today's "a stale
   tid never aliases" property; (b) is Linux's.
3. `PROC_THREAD_MAX` default and a `SPAWN_PERM_MAY_RAISE_THREAD_BUDGET`.
4. Thread-directed `tkill`/`tgkill` semantics in the phenotype; `pthread_kill`
   served in pouch (a pouch patch and a VIVARIUM row).
5. `/proc/<pid>/thread/<tid>/` layout (operator-visible; the Operator's
   Manual).
6. `SYS_THREAD_SPAWN`/`SYS_THREAD_EXIT`/`SYS_SET_TID_ADDRESS` are unchanged.

### 6.5 Migration (landable chunks, each with its gate)

- **C0 -- stop the bleeding** (no ABI): F-1 (exec spins `on_cpu` on EXITING
  peers), F-6 (`preempt_check_irq` gates on `state == RUNNING`), F-8 (refuse
  spawn in a dying Proc), F-2 mitigation (`-EAGAIN` at tid/pid exhaustion).
  Gate: audit round; `tools/ci-smp-gate.sh`; a `KERNEL_TESTS` window test for
  F-1 (hold the exiting peer's tail, exec concurrently, assert the old space
  survives until the peer is off-CPU); the full suite on ci + production
  images.
- **C1 -- autoreap at the switch**: dead list + reaper; `wait_pid` frees the
  Proc only; `joined` kthreads; focus clear; `proc_cpu_ns` fold; dossier
  updates (`sub-kernel-thread`, `sub-kernel-death`, `sub-kernel-proc`,
  `inv-i32`). Gate: `thread_reap.tla` green + three buggy cfgs red; the
  churn test (10^5 create/join at -smp 4 with `thread_total_destroyed`
  tracking and kstack pages flat); libxml2's thread test; the OpenRCT2 demo
  for 8 h; SMP gate >= 10 boots on all four images; UBSan.
- **C2 -- kstack cache** (E-1/E-2): no TLBI on the hot path, no KP_ZERO,
  bounded cache. Gate: cpubench thread create/exit latency before/after
  (measured, not estimated); `/ctl/kstack` watermark unchanged; SMP gate.
- **C3 -- per-Proc threads_lock** (E-6): scope `g_proc_table_lock` to the
  tree. Gate: lock-order audit with the new edge written down; `death_wake`
  cfgs; SMP gate; the #811 verified-sound set re-prosecuted.
- **C4 -- thread-directed notes** (F-3, ABI 4): per-thread pending, tkill,
  pthread_kill, SIGCANCEL. Gate: `caught_target.tla` extension; a
  `pouch-hello-signals-threads` E2E (worker interrupted by `pthread_kill`
  while main sleeps; a phenotype twin); audit.
- **C5 -- de_thread (L-2b)**: exec with threads. Gate: exec-probe
  multi-thread leg; `thread_reap.tla` exec-before-dead buggy cfg red; audit.
- **C6 -- limits and identity** (F-5, F-2, ABI 1-3): pool-charged kstacks,
  raised default, spawn-perm raise, 64-bit tid, main tid == pid. Gate: the
  I-32 cap-hit tests per axis; Go `GOMAXPROCS`-storm probe; operator vote
  recorded before the chunk.
- **C7 -- per-tid /proc + one first-entry tail** (F-4, F-7, ABI 5). Gate:
  `debug_stop.tla` + `debug_step.tla` re-run; the debug-probe with a
  multi-M target whose newest M has exited; SMP gate.
- **C8 -- torpor** (F-9, F-10, E-7): per-bucket locks, FIFO, AS key, model.
  Gate: `torpor.tla`; torpor tests + a fairness witness; the go-build
  osyield/futex floor re-measured.
- **C9 -- switch fast paths** (E-4, E-5): same-AS skip; lazy FP restore
  (separate, riskiest: a per-CPU FP-owner protocol). Gate: cpubench; the FP
  note-delivery tests; SMP gate.
- **C10 -- scripture**: ARCH 7.3/7.4/7.9/23.4, IDENTITY 3.8, DEBUG-FS 5c.7,
  the pouch patch header, and `thread.rs` rewritten to the as-built model,
  co-staged with the chunk that changes each.

### 6.6 Risks

- C1 touches every holder of a `Thread *`. Enumerated from this read: the
  `wait_pid_for` walk (`proc.c:6447`), `loom_free`'s join (`loom.c:395`),
  `debug_focus_thread` (pointer identity, `devproc.c:1945`), the test
  harness's lock-free peer walks (`test_thread_spawn.c:139, 236, 403, 425`),
  `cs->prev_to_clear_on_cpu` (cleared before the RELEASE, safe),
  `rendez_blocked_on` and the timerwait/torpor lists (never set for an
  EXITING thread, safe), the caught-note walk and `proc_holds_tid_locked`
  (under the lock, safe). Anything missed is a UAF with a recycled slot, so
  the magic-at-offset-0 check (`thread.h:57`) must stay and the reaper must
  poison a freed slot's tid.
- The reaper must never run inside a `cs->lock` hold or with
  `preempt_count != 0`; it takes `g_proc_table_lock` and buddy locks.
- Lazy FP is the one change that can corrupt silently (a stale owner); it
  gets its own spec-first round or stays eager.
- Raising the thread cap without pool-charging kstacks turns the cap into a
  kernel-memory DoS; C6 must land the charge first.
- Scripture drift: C10 is not optional; the three docs disagreeing about
  `thread_count` is how the freeze shipped.

---

## 7. Verdict on the in-flight fix

The fix ("free EXITING peers from inside the Proc -- at thread create before
the cap check and at thread exit, claimed under `g_proc_table_lock`, freed
outside it; `wait_pid` stays the final sweep; fold CPU time per Proc")
addresses the symptom correctly and has the right instinct about the claim
and the `on_cpu` gate. It is the wrong shape, and as described it has three
holes that must be closed before it can merge.

**Holes.**

1. **Double free against `wait_pid_for`.** The last thread out sets ZOMBIE
   under the lock (`proc.c:4648`), unlocks (`4656`), and only then runs its
   tail -- if the "at thread exit" sweep runs on that last thread after the
   unlock (or on a non-last thread whose exit races the parent), the parent
   on another CPU can already have unlinked the zombie and be walking
   `zombie->threads` lock-free (`proc.c:6447-6455`), spinning `on_cpu` and
   `thread_free`ing. Two freers of one Thread: `thread_free of corrupted or
   already-freed Thread` (`thread.c:670`) if the slot is still poisoned, a
   silent UAF of a recycled slot otherwise. Required: either the sweep runs
   strictly BEFORE the ZOMBIE transition (while ALIVE, when `wait_pid` cannot
   reap) and never on the last-out path after the flip, or `wait_pid`'s walk
   claims under the lock too. The robust form: **the claim is the unlink** --
   move `thread_unlink_from_proc` to the claim step (under the lock, with
   `on_cpu == false` read ACQUIRE under the same hold) and let `thread_free`
   accept an already-unlinked thread. Then every freer (sweep, reaper, wait)
   claims the same way and a thread can be in at most one freer's hands.
2. **`kproc` and joined kthreads.** The SQPOLL kthread marks itself EXITING
   and waits for `loom_free` to `thread_free` it (`loom.c:386-396,
   2746-2757`). A sweep at thread create on kproc (a second
   `loom_start_sqpoll`, any kthread creation) that frees EXITING kproc
   threads would free it under its joiner. The sweep must run only on the
   user spawn paths (`sys_thread_spawn_handler`, `viv_clone_thread`) and
   `thread_exit_self` (which kproc never calls), and must skip any thread
   with a joiner; make that a flag on the Thread (`joined`), not a property
   of the call site.
3. **Pointer identity after the free.** `devproc_focus_thread` validates the
   focus by `th == focus` against the list (`devproc.c:1945-1947`). A freed
   focus thread's slot recycled by a new thread of the same Proc satisfies
   the comparison and the debugger reads the wrong thread's frame. The sweep
   must clear `p->debug_focus_thread` under the lock when it frees that
   thread (or the validation must compare tids).

**Weaknesses that are not holes.**

4. Reclamation keyed to sibling events: a Proc that creates and joins 200
   threads and then blocks keeps 6.4 MiB of kstacks until its next
   create/exit -- the same residue as today minus the cap refusal. The
   honest cap and the prompt reclamation both come from reaping at the
   switch (6.1), where "off-CPU" is known rather than spun for.
5. Latency and TLB cost of the sweep on the spawn path (E-10): up to 255
   frees, each with a system-wide `tlbi vmalle1is`, inside one
   non-preemptible syscall body. Land the kstack cache first or bound the
   sweep per call.
6. The sweep's `thread_free` outside the lock must work from a snapshot taken
   under the lock (an intrusive claimed-list), never by re-walking
   `t->next_in_proc` after the unlock.
7. Tests with lock-free peer walks (`test_thread_spawn.c:139, 236`) become
   racy once a sibling's exit can unlink a node mid-walk; take the lock in
   the tests.
8. The CPU-time fold is sound as described (the last `run_ns` store happens
   before the switch, `sched.c:1518-1521`, so an ACQUIRE of `on_cpu == false`
   sees it); write and read `p->cpu_ns_dead` under the same lock as the walk.
9. Docs: `sub-kernel-proc.md:646`, `inv-i32.md:39`, `proc.h:112` and
   `thread.rs:38-41` must all change in the same commit, in the same
   direction ("`thread_count` counts threads not yet reclaimed; reclamation
   follows the final switch within one create/exit event").

**Is a different shape better?** Yes: reap at the switch (6.1, C1). It is the
Plan 9 (`schedinit` frees Moribund) and Linux (`finish_task_switch` frees
`TASK_DEAD`) answer, it needs no `on_cpu` spin (the resume path IS the
off-CPU point), it is independent of siblings and parents, it keeps
`wait_pid`'s job to the Proc alone, and it is the shape L-2b's de_thread
needs anyway. If the sibling-sweep must ship first for the demo, ship it with
holes 1-3 closed and the "claim is the unlink" rule, then replace it in C1
rather than extend it.

---

## Verified sound (properties checked against the snapshot that survived)

- The EXITING commit and the live-peer count are under one lock; two threads
  cannot both decide they are last (`proc.c:4552-4555, 4651`; `exits_term`
  `4434, 4495`). inv-i24's exactly-once argument holds.
- `sleep_common`/`tsleep_common` register-then-observe under `wait_lock`
  (`sched.c:2009-2117, 2248-2337`) matches `death_wake.tla`'s correct path;
  the cascade reads `rendez_blocked_on` under the same lock and holds it across
  `wakeup` (`proc.c:5177-5182`).
- torpor's post-register die check under `torpor_lock` (`torpor.c:268-272`)
  closes the register-after-walk race; `TSLEEP_INTR` is absorbed to
  `TORPOR_OK` deliberately (`torpor.c:326-335`).
- `thread_free`'s gate order (RUNNING refusal, remove-if-runnable, RUNNING
  re-check, `on_cpu` ACQUIRE spin, unlink, guard restore, free;
  `thread.c:665-756`) and `try_steal`'s claim under the peer lock
  (`sched.c:1233-1236`) close #788/#801 as described.
- `wait_pid_for` unlinks under the lock before its lock-free frees and spins
  `on_cpu` per thread (`proc.c:6419-6455`); the zombie is unreachable to
  `proc_for_each` after the unlink, so today's debug-focus pin argument
  holds (it is what fix hole 3 would break).
- The final-close window's three properties (RUNNING closer, ALIVE Proc,
  no RUNNING peer) hold at both sites; `exit_close_active` is owner-only with
  one setter (`proc.c:4056-4094`).
- An EXITING tail cannot sleep today: the demand-page prot check precedes the
  blocking FILE arm (`fault.c:430-438` vs `:547`), no writable FILE VMA exists,
  and the anon/COW arms allocate without sleeping; a kernel-mode uaccess
  fault that cannot be resolved returns -1 via the fixup, never terminates
  (`exception.c:296-322`).
- `thread_user_trampoline` and `thread_fork_trampoline` die-check before the
  eret with DAIF masked through it (`context.S:300-359`, `vectors.S:411-419`).
- The ptid publish precedes `ready()` (`syscall.c:5347-5352`); `g_next_tid`
  is atomic (`thread.c:79`).
- Syscall-body exits (`SYS_THREAD_EXIT`, `SYS_EXIT_GROUP`, `SYS_EXITS`) are
  non-preemptible in their tails (`in_syscall`, `sched.c:2829`); only the
  kthread-shaped paths are exposed (F-6).
- `kproc` is excluded from group termination (`proc.c:5104`), `thread_exit_self`
  extincts from kproc (`proc.c:4530`), the SQPOLL exit handshake is IRQ-masked
  across EXITING + the joiner wake (`loom.c:2746-2757`).
- `proc_thread_cap_ok`'s overshoot is bounded by concurrent spawners
  (`proc.h:1583-1590`), as documented.
- The caught-note designation is dropped at the EXITING commit under the
  lock (`notes_exit_drop_locked`, `notes.c:1634-1644`), so no walk designates
  an exiting thread.
- `#344`'s multi-waiter `wait_pid` register-then-observe (`proc.c:6491-6516`).
- Lock order consistent across the files read; no path takes a rendez or
  torpor lock and then the table lock.

MODEL(end): Fable 5.1 (claude-fable-5-1)
