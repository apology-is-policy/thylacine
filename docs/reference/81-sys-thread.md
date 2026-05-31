# 81 — `SYS_THREAD_SPAWN` / `SYS_THREAD_EXIT`: multi-thread Procs (P6-pouch-threads sub-chunk 9a)

The kernel-side pthread substrate. `SYS_THREAD_SPAWN` (41) creates a peer Thread in the calling Proc that erets to EL0 at a caller-supplied entry point on a caller-supplied stack; `SYS_THREAD_EXIT` (42) terminates the calling Thread, atomically clearing a registered `clear_child_tid` word and torpor-waking joiners on it. Together with `SYS_SET_TID_ADDRESS` (36, repurposed at this chunk to STORE the tidptr) and the `torpor` primitive (sub-chunk 8), they form the kernel half of POSIX pthread create / exit / join. Pouch's userspace pthread mutex/condvar/rwlock/once layer (sub-chunk 9b) sits on top.

Per `POUCH-DESIGN.md §7 [RESOLVED 7.3]` (the v1.0 design — POSIX threads ARE Thylacine Threads within one Proc) + `ARCHITECTURE.md §11.2` (the syscall table). Audit-trigger surface (`CLAUDE.md` / `ARCHITECTURE.md §25.4`): `kernel/thread.c::thread_create_user`, `arch/arm64/context.S::thread_user_trampoline`, `kernel/syscall.c::sys_thread_spawn_handler` / `sys_thread_exit_handler` / `sys_set_tid_address_handler`, `kernel/proc.c::exits / thread_exit_self / wait_pid`, `arch/arm64/uaccess.S::uaccess_store_u32` (the new u32 store primitive).

---

## Why a kernel-side syscall

POSIX pthreads live in pouch's userspace, but the substrate is kernel-side: a peer Thread sharing the Proc's address space (one `pgtable_root` + ASID, one handle table, one Territory) cannot be constructed without kernel cooperation. The kernel owns:

- The `struct Thread` descriptor + 16 KiB kstack (for handling syscalls / IRQs / faults at EL1 on behalf of the worker).
- The `cpu_switch_context` ABI (which callee-saved-register slots the saved context can use to park the first-dispatch args).
- The eret-to-EL0 transition that takes the new Thread from "just-scheduled-in EL1 trampoline" to "executing the user's entry function at EL0 on the user-provided stack."

These are exactly the things pouch's userspace cannot do for itself. The userspace pthread library can do mutex / condvar / rwlock / once / barrier as plain C code over the `torpor` primitive (sub-chunk 8) — the uncontended fast path is a user-side atomic; only on contention does the lower half call into the kernel. But the new-Thread bootstrap is genuinely kernel-resident; hence two new syscalls.

POUCH-DESIGN.md §7 calls this out:

> The one exception: the thread + `torpor` layer (§7) is *not* filesystem-shaped — it is the single part of pouch's lower half that is a genuine call translation, not a synthetic-FS translation. Files, sockets, `poll`, and signals-as-notes all fit the synthetic-FS framing; threads do not.

---

## Syscall ABI

```c
// kernel/include/thylacine/syscall.h
SYS_THREAD_SPAWN = 41
SYS_THREAD_EXIT  = 42
```

### `SYS_THREAD_SPAWN(entry_va, sp_va, arg, tls_va) → tid / -errno`

| Register | Meaning |
|---|---|
| `x0` | `entry_va` — user-VA of the EL0 entry function. Non-zero; below `USER_VA_TOP` (= `1 << 47`). |
| `x1` | `sp_va` — user-VA of the new Thread's user stack TOP. Non-zero; 16-byte aligned (AAPCS64); below `USER_VA_TOP`. |
| `x2` | `arg` — opaque value passed as x0 (AAPCS64 arg-0) to `entry`. The kernel never dereferences it. |
| `x3` | `tls_va` — initial `TPIDR_EL0` value (TLS base). `0` permitted (no TLS — entry must install one before any TLS deref). Non-zero values must be below `USER_VA_TOP`. |
| `x8` | syscall number = 41 |

**Returns** (Linux/musl-numeric, decoded by pouch's `syscall_ret.c` as `-errno`):

| Return | Meaning |
|---|---|
| `> 0` | new Thread's tid |
| `-22 (-EINVAL)` | bad alignment / out-of-bound `entry_va` / out-of-bound `sp_va` / out-of-bound `tls_va` / caller is `kproc` |
| `-12 (-ENOMEM)` | Thread cache alloc fail / kstack alloc fail |

### `SYS_THREAD_EXIT() → never returns`

| Register | Meaning |
|---|---|
| `x8` | syscall number = 42 |

No arguments. Never returns to userspace — any return is a kernel bug. The kernel atomically:

1. If `t->clear_child_tid` is non-zero (the calling Thread registered one via `SYS_SET_TID_ADDRESS`): `uaccess_store_u32(t->clear_child_tid, 0)` + `sys_torpor_wake_for_proc(p, t->clear_child_tid, UINT32_MAX)`. Best-effort — an unmapped tidptr skips the wake but does NOT extinct (userspace bug, not ours).
2. Marks `t->state = THREAD_EXITING` under `g_proc_table_lock`.
3. If this Thread was the LAST live (non-EXITING) Thread in the Proc: transitions the Proc to `PROC_STATE_ZOMBIE` with `exit_status = 0` (mirrors `exits("ok")`) + wakes parent's `child_done` Rendez. Outside the lock: `srv_proc_exit_notify(p)` + `cap_proc_exit_notify(p)`.
4. yields via `sched()`.

### `SYS_SET_TID_ADDRESS(tidptr) → tid / -1` (extended at this chunk)

Pre-sub-chunk-9 the tidptr was accepted but not stored. The chunk extends it to actually persist the tidptr on the calling Thread, so `SYS_THREAD_EXIT` (and `exits()`) can deliver the join handshake:

| Register | Meaning |
|---|---|
| `x0` | `tidptr` — user-VA of a 4-byte word. `0` clears the field. Non-zero must be 4-byte aligned + below `USER_VA_TOP`. |
| `x8` | syscall number = 36 |

**Returns**: the calling Thread's `tid` (positive int) on success, `-1` on validation failure (non-zero tidptr that is not 4-byte-aligned / outside user VA / null Proc).

The returned `tid` is the calling Thread's per-Thread monotonic identity (`g_next_tid` at allocation time — atomic u32, F4 audit close). For the MAIN thread of a Proc this is NOT the pid in general — pre-sub-chunk-9 the SVC returned the pid (a single-threaded approximation that aliased the Linux thread-group-leader convention); the real Thread tid is what `pthread_self` / `pthread_join` need to compare against, so the return changed in lockstep with the storage.

**ABI compatibility note (F11 audit close)**: this is a silent semantic change. Binaries built against pre-sub-chunk-9 kernels that compare the SYS_SET_TID_ADDRESS return to a known pid (e.g. "I am pid N" debug logs) will now print the Thread tid instead. The breakage is benign for musl-via-pouch (which treats the return as the calling thread's tid agnostic to the integer value) but visible to any direct-syscall caller. The change is necessary — the v1.0 single-Proc-many-Thread world makes the pid an ambiguous identifier when there are multiple Threads in a Proc.

---

## Implementation

### `thread_create_user` (`kernel/thread.c`)

Same body as `thread_create_internal` (used by `thread_create` / `thread_create_with_arg`), except `ctx.lr = thread_user_trampoline` (not `thread_trampoline`) and the four user-mode args are parked in callee-saved ctx slots that `cpu_switch_context` reloads before `ret`:

```c
ctx.x19 = user_sp_va    // SP_EL0 install
ctx.x20 = user_arg      // moved to x0 (AAPCS64 arg-0) before eret
ctx.x21 = user_entry_va // ELR_EL1 target
ctx.x22 = user_tls_va   // TPIDR_EL0 init (0 = no TLS, valid)
ctx.lr  = thread_user_trampoline
ctx.sp  = top of kstack          // SP_EL1 — kernel-mode SP during the
                                 // brief EL1 trampoline window
ctx.ttbr0 = (asid << 48) | pgtable_root  // caller's Proc — same address space
```

The kstack layout (16 KiB usable + 16 KiB guard) is identical to the EL1-side helpers. The new Thread is RUNNABLE on return; the caller (the syscall handler) immediately `ready()`'s it.

### `thread_user_trampoline` (`arch/arm64/context.S`)

The user-mode variant of `thread_trampoline`. Identical `sched_finish_task_switch` + `DAIF` unmask discipline, differing only in the tail:

```asm
thread_user_trampoline:
    bti     c
    bl      sched_finish_task_switch    // release prev's run-tree lock
    msr     daifclr, #2                  // unmask IRQs

    msr     elr_el1,   x21        // ELR_EL1 = user_entry_va
    msr     spsr_el1,  xzr        // PSTATE = EL0t, DAIF clear
    msr     sp_el0,    x19        // SP_EL0 = user_sp_va (non-current bank at EL1h)
    msr     tpidr_el0, x22        // TPIDR_EL0 = user_tls_va

    mov     x0, x20               // user_arg → AAPCS64 arg-0 (the user entry's x0)
    mov     x1..x30, xzr          // zero every other GPR
    eret
```

The discipline is the same as `arch/arm64/userland.S::userland_enter` (which `exec_setup` + the spawn handlers use to bootstrap a fresh Proc's main thread); the difference is doing it inline so `x0 = user_arg` survives the GPR-zeroing sweep. Mirrors Linux's `ret_from_fork` / `kernel_clone` discipline.

SP_EL0 install at EL1h: the kernel runs uniformly at EL1h (SPSel=1, invariant I-21), so `sp` is `SP_EL1` (the kstack). `msr sp_el0, x19` is the architecturally well-defined non-current-bank write (not the CONSTRAINED UNPREDICTABLE write-the-current-SP form that would fire at EL1t).

### `sys_thread_spawn_handler` (`kernel/syscall.c`)

Validates the four args, calls `thread_create_user`, `ready()`s the new Thread, returns the tid. Argument validation:

- `entry_va`: non-NULL; below `UACCESS_USER_VA_TOP` (= `1 << 47`). No alignment requirement.
- `sp_va`: non-NULL; 16-byte aligned (AAPCS64); below `UACCESS_USER_VA_TOP`.
- `tls_va`: `0` permitted; non-zero must be below `UACCESS_USER_VA_TOP`. No alignment requirement (TLS layout is libc-defined).
- Caller must NOT be `kproc()` and must have `pgtable_root != 0` (i.e. a userspace Proc with an installed address space). Defense-in-depth: kproc threads never execute SVC, but the check fail-closes if somehow a kproc thread reaches the handler.

On any validation failure: `-EINVAL` (`-22`). On `thread_create_user` returning NULL (Thread cache or kstack alloc fail): `-ENOMEM` (`-12`).

### `thread_exit_self` (`kernel/proc.c`)

The body of `SYS_THREAD_EXIT`. Distinct from `exits()` in two ways:

1. `exits()` REQUIRES the caller to be the last live Thread (extincts if any peer is non-EXITING) and accepts a caller-specified exit message. `thread_exit_self()` does NOT — it cleanly exits a non-last Thread without touching Proc state, OR cleanly transitions the Proc to ZOMBIE with `exit_status = 0` if this happens to be the last live Thread.
2. Both do the clear-child-tid handoff for the calling Thread (the kernel atomically clears `*clear_child_tid` and `torpor_wake`s on it); any peer Thread that registered its own tidptr will see its own handoff fire on its own exit.

The implementation walks `p->threads` under `g_proc_table_lock` to count live peers, decides `become_zombie = (live_peers == 0)`, then under the same lock writes `t->state = THREAD_EXITING` (and the Proc-zombie transition + parent wake if `become_zombie`). Outside the lock: `srv_proc_exit_notify` + `cap_proc_exit_notify` if zombie. Then `sched()`.

### `exits()` extension (`kernel/proc.c`)

The v1.0 single-threaded Proc constraint (`p->thread_count != 1` extinction) is replaced with a peer-Threads-all-EXITING check. `exits()` REQUIRES every non-self Thread in `p->threads` to be `THREAD_EXITING` at the moment of the call — programs must `pthread_join` (or otherwise account for) every spawned Thread before main returns. Cross-thread shootdown (Linux's CLONE_THREAD-style `exit_group`) is a v1.x extension; at v1.0 a live peer Thread at exits-time is a programming error (extinction with a clear message).

Practical note for ports: this matches what well-written modern POSIX programs already do — POSIX itself says the program's behavior with non-joined non-detached threads at program exit is undefined; v1.0 simply makes the "undefined" path explicit (panic) rather than silently shooting down workers.

### `wait_pid` extension (`kernel/proc.c`)

The reap path walks `zombie->threads` (instead of capturing a single Thread) and frees each. For each Thread:

1. Sanity-check `state == THREAD_EXITING` (under the proc-table lock, mirroring the pre-sub-chunk-9 invariant).
2. Capture `next = ct->next_in_proc` BEFORE `thread_free` (which unlinks it).
3. Spin on `ct->on_cpu` (P2-Cf SMP wait/wake race close — the EXITING Thread might still be mid-switch on a peer CPU).
4. `thread_free(ct)` — releases the kstack + the Thread descriptor.

Loop terminates when `zombie->threads == NULL`. `thread_count` reaches 0; `proc_free`'s precondition (`!thread_count && !threads`) holds.

### `uaccess_store_u32` (`arch/arm64/uaccess.S`)

The producer-side equivalent of `uaccess_load_u32` (sub-chunk 8). A single `str w1, [x0]` with a fixup-table entry covering translation/permission faults; the dispatcher catches the fault and routes to the fault label which returns `-1`. Plain STR (not STLR) — the consumer's later load runs under `torpor_lock` whose acquire pairs with the lock-release following this store.

Alignment is NOT validated in the asm; the C caller (here `thread_clear_child_tid_handoff`) checks 4-byte alignment at storage time (SYS_SET_TID_ADDRESS validates). An unaligned STR triggers an alignment fault that the fixup table does NOT catch — the caller MUST validate.

---

## Lifecycle

### Spawn-to-exit per-Thread

1. **Userspace**: pouch's `pthread_create` (or any caller via `t_thread_spawn`) allocates a stack region (via `SYS_BURROW_ATTACH`) + a `pthread_t` struct containing the tid storage + any per-pthread state. Calls `SYS_THREAD_SPAWN(entry, sp_top, arg, tls)`. Returns the new Thread's tid.

2. **Kernel**: `sys_thread_spawn_handler` validates → `thread_create_user` allocates Thread + kstack + lays out ctx → `ready()` inserts in run-tree → returns tid.

3. **First dispatch**: scheduler picks the new Thread. `cpu_switch_context` reloads x19-x22 + sp + ttbr0 + tpidr_el0 + fp_v + pstate, then `ret`s into `thread_user_trampoline`. The trampoline installs SP_EL0 + TPIDR_EL0 + ELR_EL1 + x0 = arg, zeroes other GPRs, `eret`s to EL0.

4. **EL0 execution**: the user's entry function runs. AAPCS64-compliant; receives `arg` in x0; can call any syscalls; can register a tidptr via `SYS_SET_TID_ADDRESS`.

5. **Exit**: the entry function (or pouch's `pthread_exit` wrapping it) calls `SYS_THREAD_EXIT`. Kernel `thread_exit_self`:
   - If a tidptr was registered: `uaccess_store_u32(0)` + `torpor_wake(UINT32_MAX)` on tidptr — pthread_join wakes.
   - Marks `state = THREAD_EXITING` under `g_proc_table_lock`.
   - If last live Thread: transitions Proc → ZOMBIE with status 0 + wakes parent.
   - `sched()` yields to a peer Thread (or idle on this CPU).

6. **Reap**: when the Proc exits (last Thread → ZOMBIE), the parent's `wait_pid` walks `p->threads` and `thread_free`s each. The dead-Thread descriptor + kstack live in the Proc's lifetime — per-Thread reaping is a v1.x extension.

### Join handshake — the clear-child-tid path

The standard Linux/musl pattern adapted to Thylacine. Producer-consumer roles:

- **Worker** (producer of the tidptr clear + torpor wake):
  1. At Thread startup, calls `t_set_tid_address(&pthread_t.tid)` to register the tidptr. Pouch's `__pthread_setup` does this; native callers can do it directly.
  2. Initialises `*tidptr = self_tid` (or any non-zero sentinel value).
  3. Runs the body.
  4. Calls `t_thread_exit()` → `SYS_THREAD_EXIT`. Kernel atomically clears `*tidptr` to 0 + torpor-wakes on it.

- **Joiner** (consumer):
  1. Reads `*tidptr` — if zero, the worker has already exited; join is immediate.
  2. Otherwise calls `t_torpor_wait(&tidptr, expected_value, -1)`. Three orderings (see `docs/reference/80-torpor.md` proof sketch):
     - Worker exits BEFORE joiner's wait: kernel's store + wake happen; joiner's wait fast-paths (load == 0 ≠ expected_value → return 0). Re-check confirms `*tidptr == 0`.
     - Worker exits DURING joiner's wait: joiner is registered; kernel's wake delivers; joiner re-checks the word.
     - Spurious wake: joiner re-checks and re-arms. (None expected at v1.0 — the only producer is `thread_clear_child_tid_handoff` and `exits()`'s own clear-then-wake.)

Pouch's `pthread_join` wraps this loop with the `pthread_t` bookkeeping; the worked example is `/thread-probe` (`usr/thread-probe/thread-probe.c`).

---

## SYS_EXIT_GROUP — cross-thread shootdown (`= 60`)

`SYS_EXIT_GROUP(status)` terminates the WHOLE Proc (POSIX `exit_group(2)`), cascading termination to every peer Thread — the v1.x lift named in the caveats below, landed for #809 (the #808-audit F3 de-flake). It replaces the v1.0 behavior where `_Exit` / `abort` / a mallocng assert in a multi-thread pouch Proc routed `__NR_exit_group -> SYS_EXITS` and EXTINCTED the kernel ("exits with live peer threads"). Design: ARCH §7.9.1 + invariant I-24.

**Model — flag-and-self-terminate at the EL0-return checkpoint** (Plan 9 / Linux / Zircon convergent; seL4's synchronous cross-core stall rejected). The caller flags the Proc + wakes/kicks its Threads; each Thread kills ITSELF at its next return-to-EL0. No Thread is force-torn-down from outside; the IPI is a latency accelerant, not a synchronous stop.

**Mechanism** (`kernel/proc.c::proc_group_terminate`):
1. CAS-publish `p->group_exit_msg` (a `const char *` NULL-sentinel; set-once, first msg wins; `__ATOMIC_RELEASE`). NULL = not terminating; non-NULL = the recorded last-Thread-out ZOMBIE status (`"ok" -> 0`, else `1`).
2. `torpor_wake_all_for_proc(p)` — wake every futex (torpor) sleeper of `p` so it returns from `torpor_wait` to its die-check.
3. `smp_resched_others()` — broadcast `IPI_RESCHED` to other CPUs so a peer spinning in userspace traps + hits its IRQ-from-EL0 die-check (Linux's `kick_process`; the periodic preemption timer is the floor if the IPI is missed).

`proc_group_terminate` acquires NO `g_proc_table_lock` (only `torpor_lock` + via `wakeup` the rendez/cs locks, all below it), so it is safe to call either holding `proc_table_lock` (the cross-Proc `kill` walk_cb, where the target is alive under the lock) or not (`SYS_EXIT_GROUP` on `self->proc`).

**The die-check** (`kernel/proc.c::el0_return_die_check`) runs at every return-to-EL0: the sync-from-EL0 tail (`exception_sync_lower_el_impl`: the SVC + fault-handled paths) AND the new IRQ-from-EL0 tail (`vectors.S` 0x480 -> `bl el0_return_die_check`). If `current->proc->group_exit_msg != NULL` it calls `thread_exit_self()` (noreturn — `sched()`s away). **#713-safe**: it runs BEFORE the DAIF-masked `ELR_EL1`-set..`eret` window, and the die path never reaches the `eret`. The LAST Thread out transitions the Proc -> ZOMBIE with the group status; `wait_pid` reaps the multi-Thread zombie (the `on_cpu`-spin per #788, unchanged).

**Lost-wakeup close (I-9):** `sys_torpor_wait_for_proc` re-checks `group_exit_msg` AFTER registering its waiter, under `torpor_lock` — the same lock `torpor_wake_all_for_proc` walks. A peer that registers after the wake-all walk observes the set flag here and does not sleep; a peer that registered before is found by the walk. Register-then-observe, airtight.

**Consumers.** `SYS_EXIT_GROUP = 60` calls it on `self->proc` (pouch rewires `__NR_exit_group` 0 -> 60 in `0001-pouch-syscall-seam.patch`; libt `t_exit_group` + libthyla-rs `t_exit_group` export it natively). A cross-Proc `kill` of a multi-thread target calls it (`sys_postnote`, both the walk_cb + self-post paths) instead of the prior `-EIO` refusal (closes 13b R1-F9). A single-thread Proc gets `exits(status)`-equivalent semantics.

**v1.0 envelope (ARCH OPEN Q 7.9.A).** A peer blocked INDEFINITELY in a non-torpor / non-note kernel sleep (at v1.0 only a bare pipe read awaiting data/EOF) dies at its blocking call's completion, not instantly — the chosen consumers (stratumd's futex / 9P / running workers; typically single-thread killed Procs) do not hit it. Universal death-interruptibility of every kernel sleep (the Plan 9 `error(Eintr)`-from-`sleep` lift) is the v1.x arc. The multi-thread FAULT path (`proc_fault_terminate`) is a tracked follow-up (#810), not this chunk.

**Tests.** `kernel/test/test_proc.c::proc.group_terminate_smoke` (kernel-side: set-once CAS, NULL no-op, die-check no-op on a non-terminating Proc, isolation). End-to-end: `/pouch-hello-exitgroup` (joey smoke) spawns 2 live un-joined workers (one cond-wait torpor sleeper, one userspace spinner), calls `_Exit(0)` -> `exit_group`, and joey reaps rc == 0 with zero extinction — the Proc only zombies once BOTH peers died, so rc == 0 IS proof the full cascade completed. Verified default(smp4) + UBSan + smp8 (the smp8 run exercises the cross-CPU IPI-kick).

---

## Tests

### Kernel unit tests (`kernel/test/test_thread_spawn.c`)

| Test | Coverage |
|---|---|
| `thread.create_user_ctx_layout` | `thread_create_user` populates `ctx.x19..x22` + `ctx.lr=thread_user_trampoline` + `ctx.ttbr0=(asid<<48)\|pgtable_root` + state=RUNNABLE + `clear_child_tid=0`. |
| `thread.exit_self_marks_exiting` | rfork child with 2 peer Threads; each peer calls `thread_exit_self`; main waits until both reach `THREAD_EXITING`; exits cleanly. Verifies the peer-state transition + the Proc-stays-ALIVE-if-peers-remain path + the wait_pid multi-Thread reap. |
| `thread.exit_self_last_thread_zombies` | rfork child where the FIRST thread to call `thread_exit_self` IS the only Thread. Verifies the "this is the last live Thread" path: Proc → ZOMBIE with `exit_status=0`. |
| `proc.multi_thread_reap` | Larger reap — 5 peer Threads. Verifies `wait_pid`'s drain-loop walks the full list (on_cpu spin + `thread_free` per Thread). |

### End-to-end via `/thread-probe` + joey smoke

`usr/thread-probe/thread-probe.c` is a libt-based ELF Joey spawns; it exercises the full pipeline at EL0:

- main allocates a 4 KiB worker stack (`.bss`-aligned) + a tidptr word + a counter.
- main calls `t_thread_spawn(worker_entry, sp_top, &args, 0)`.
- Worker: `t_set_tid_address(&tidptr)` + atomic store `42` to `*counter` + `t_thread_exit()`.
- main: `t_torpor_wait(&tidptr, SENTINEL, -1)` → kernel wakes when tidptr is cleared. Verify `*tidptr == 0` and `*counter == 42`. Exit 0.

joey's main runs this on every boot via a small `t_spawn("thread-probe")` + `t_wait_pid()` orchestration; failure exits non-zero and the boot doesn't reach `Thylacine boot OK`. The joey smoke also exercises three SVC-dispatch fast-path negative cases: null entry / misaligned sp / out-of-bound entry → `-EINVAL`.

### Coverage at sub-chunk close

| Layer | Coverage |
|---|---|
| Argument validation | joey smoke (3 negative cases — null entry, misaligned sp, OOB entry) |
| ctx layout | `thread.create_user_ctx_layout` kernel unit |
| Single-Thread exit | `thread.exit_self_last_thread_zombies` + the `/thread-probe` worker (an EL0 exit) |
| Multi-Thread exit | `thread.exit_self_marks_exiting` (kernel-mode peers) + `/thread-probe` (1 EL0 peer) |
| Multi-Thread reap | `proc.multi_thread_reap` (5 peers) + `/thread-probe` (1 peer) |
| `exits()` peer-EXITING gate | `thread.exit_self_marks_exiting` (verifies the kernel-mode peer path matches `/thread-probe`'s EL0 path through the same `exits()`) |
| Clear-child-tid handoff | `/thread-probe` (the full chain — register tidptr, kernel clears + wakes, joiner observes) |
| Spec-to-code | Suspended (CLAUDE.md "Spec-to-code FULLY suspended", 2026-05-23). The invariants are validated by careful prose reasoning in the file headers + this reference + the audit + the runtime tests. |

---

## Known caveats / footguns

1. **No per-Thread reaping at v1.0.** A Thread that calls `SYS_THREAD_EXIT` leaves its Thread descriptor + 32 KiB allocation (16 KiB kstack + 16 KiB guard) live in the Proc's `threads` list until the Proc dies. For short-lived programs (libsodium tests) the leak is bounded; for long-running daemons (stratumd, future Thylacine-native daemons) the thread count must be bounded at the program level — typical pthread pool patterns work. A v1.x sub-chunk can add a per-Thread reap path (idle-CPU reaper walking a per-Proc dead-Thread list) when a workload requires it.

2. **`exits()` (SYS_EXITS) requires all peer Threads EXITING; `SYS_EXIT_GROUP` does the shootdown.** The cross-thread shootdown that terminates running peers landed for #809 as `SYS_EXIT_GROUP` (= 60; see the section above; ARCH §7.9.1 / I-24) — pouch routes `_Exit` / `exit` / `abort` / `exit_group` through `__NR_exit_group -> SYS_EXIT_GROUP`, so a multi-thread pouch Proc (stratumd) now exits cleanly instead of extincting. The bare `exits()` / `SYS_EXITS` path KEEPS its all-peers-EXITING contract: it is the non-cascade single-Thread / last-Thread exit, and a live peer at `exits()`-time still extincts with "exits with live peer threads" (well-formed programs reach the group exit via `exit_group`, not raw `SYS_EXITS`). v1.0 envelope: a peer blocked INDEFINITELY in a non-torpor / non-note kernel sleep dies at its call's completion, not instantly (ARCH OPEN Q 7.9.A); the multi-thread FAULT path (`proc_fault_terminate`) is the tracked follow-up #810.

3. **`clear_child_tid` clear-and-wake is best-effort.** If a joiner munmap'd the worker's tidptr page before the worker exits, `uaccess_store_u32` returns -1 and `torpor_wake` is skipped — the kernel does NOT extinct. This is a programming error (a joiner that unmaps a worker's tidptr without joining the worker) but v1.0 silently degrades rather than panicking; no kernel-side recovery is possible.

4. **The new Thread shares the caller's HANDLE TABLE.** SYS_THREAD_SPAWN does NOT take a fd-list (unlike SYS_SPAWN_WITH_FDS): the new Thread IS in the same Proc, so it sees the same open fds. Closing a fd in one Thread is visible to all peers (Linux pthread / Plan 9 rfork(0) semantics). For per-Thread fd isolation, use `rfork(RFPROC)` (a separate Proc with its own handle table).

5. **TPIDR_EL0 initial value is the spawn arg `tls_va`.** No kernel TLS allocation — the caller's pthread layer (pouch / native) is responsible for arranging the TLS layout the entry function expects. The trampoline writes `tls_va` verbatim into `TPIDR_EL0`; entry sees `mrs xN, tpidr_el0` returns `tls_va` (or 0 if caller passed 0, which is the "no TLS yet" sentinel — entry must install one before any TLS deref).

6. **AAPCS64 stack alignment is the CALLER's responsibility.** `sp_va` MUST be 16-byte aligned at the moment of spawn (the kernel validates this at the syscall layer). The pouch pthread layer arranges this; native callers must too. A misaligned `sp_va` returns `-EINVAL`.

7. **Worker entry signature.** The user-side `entry` is called with `x0 = arg` per AAPCS64. The signature is `void (*)(void *)`. The function MUST NOT return — there is no kernel-side fall-through to a noreturn marker; if the function returns, control falls into whatever's at the return PC on the user stack, which is uninitialised. Always call `t_thread_exit()` (or `pthread_exit` from pouch) at the end.

---

## Status

**Implemented at P6-pouch-threads sub-chunk 9a** (2026-05-23). Audit-bearing — focused opus-prosecutor round complete: **0 P0 / 3 P1 / 3 P2 / 7 P3, all dispositioned**. The three P1s — F1 list-mutation race, F2 entry_va misalignment ELE, F3 clear_child_tid handoff ordering race — were the audit's load-bearing surface; each is fixed in the close commit with documentation, defensive code, and (for F2) a deterministic regression. Closed-list: `memory/audit_p6_pouch_threads_9a_closed_list.md`.

`SYS_THREAD_SPAWN` (= 41) and `SYS_THREAD_EXIT` (= 42) live alongside `SYS_TORPOR_WAIT` / `SYS_TORPOR_WAKE` (39 / 40) as the four kernel-side primitives Thylacine's pthread implementation needs. POUCH-DESIGN.md §13's "Multithreaded test (N threads, shared mutex-protected counter, join) passes" exit criterion is partially closed by 9a (`/thread-probe` proves spawn + exit + clear-tid join); the full pthread mutex/condvar layer lands at 9b; the multi-thread proving binary with TSan validation lands at 9c.

**Sub-chunk 8 (`pouch-wait-addr`)** is the substrate this chunk consumes. Per the CLAUDE.md "Spec-to-code suspended" broadening (2026-05-23): no `specs/futex.tla` for sub-chunk 8, no `specs/pthread.tla` for sub-chunk 9 — invariants validated by prose + audit + tests.
