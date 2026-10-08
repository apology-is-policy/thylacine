---
id: sub-kernel-birth-hold
type: sub
title: "The birth hold: a spawned child parked before its first instruction"
parent: moc-kernel-execution
code: [kernel/test/test_birth_hold.c]
audit: hard
guarded-by: [inv-i39, inv-i24, inv-i9]
validated-by: [spec-debug-stop, prose, gate-smp]
locks: [lock-proc-table, lock-wait]
hazards: []
abis: []
design: ["docs/DEBUG-FS-DESIGN.md section 5f", "docs/DELVE-PORT-DESIGN.md section 8c-4"]
created: 2026-09-29
updated: 2026-10-06
---
## Purpose

A debugger that launches a program needs it stopped before its first
instruction, or the first thing it sees is a program already running. Before
the birth hold nothing in Thylacine could stop a child that early: a spawned
child entered EL0 through a hand-rolled `eret` with no frame and no stop
check, and the debugger's attach and `stop` could only land once the child was
already running. Delve found the race: the probe's child reached its loop
before the stop landed, so an entry breakpoint never fired
(DELVE-PORT-DESIGN 8c-4, closure (b), "it bit").

The birth hold closes it. A spawn that asks for `SPAWN_DEBUG_HELD` returns
only once the child has loaded its image and parked in front of its first
instruction. The debugger then attaches and either converts the hold into an
ordinary debug stop or lets the child go. The shape is the heritage one: Plan
9's `hang` (a process marked to stop at its next exec), macOS
`POSIX_SPAWN_START_SUSPENDED`, Windows `CREATE_SUSPENDED`, and Fuchsia's split
between creating a process and starting it. Linux's `PTRACE_TRACEME` is the
same idea turned around, with the child asking to be traced.

Both shape questions were voted by the operator on 2026-09-29: the fix is a
spawn flag (not an attach-time mechanism), and a held child whose spawner dies
before taking it over is killed.

## Contract

- **The ask.** `struct sys_spawn_args` spends its last forward-compat slot, at
  offset 100, as `debug_flags`, with `SPAWN_DEBUG_HELD = 1 << 0`. Any other bit
  is refused with -1. The struct stays 104 bytes, so every caller that
  zero-fills it is unchanged. The ask is ungated: it restricts only the
  spawner's own child and grants no access to it. Reading or controlling the
  child still takes an attach through the I-39 gate.
- **The return.** A held spawn returns the pid only when the child has parked
  at its birth, had its hold released, stopped being ALIVE, or left the
  caller's children. When it returns because the child parked, the child's
  image is loaded and it has executed none of it. A late exec failure reports
  as the child's exit status, as it does for any spawn.
- **Taking it over.** The attached owner's `stop` converts the hold into an
  ordinary debug stop without the child running. `start` and an explicit
  `detach` release it. Closing the ctl fd without `detach` leaves the hold in
  place. `waitstop` alone takes nothing over: it waits for a debug stop, and a
  hold is not one.
- **Death wins.** `kill`, a group termination and the orphan rule end a held
  child without it executing an instruction.
- **The orphan rule.** When the spawner becomes a ZOMBIE while the hold is
  still set, neither converted nor released, the child is terminated with
  "launcher exited".
- **Nothing else changes.** A spawn without the flag takes the same path it
  always did, `userland_enter` included.

## Mechanism

### The mark, and why it is published with the child

`Proc.debug_birth_hold` is a `u32` in the Proc's tail pad at offset 404,
pinned by a static assert. It holds NONE, UNBORN (held, still loading) or
PARKED (held, at the birth park). `rfork_spawn_held` passes the hold into
`rfork_internal`, which stores UNBORN under `g_proc_table_lock` in the same
hold that links the child into the table, the way the debug taint is stored.
No reparent, sweep or `/proc` walk can therefore find the child linked but
unmarked.

After that, the mark changes only through `proc_birth_hold_set_locked`, which
three setters in `proc.c` call under the table lock: the arrival
(`proc_birth_hold_mark_parked_locked`), the conversion and the release. Every
write wakes the parent's `child_waiters`. That wake is what the synchronous
return depends on: a write out of UNBORN that did not
wake the spawner would strand its wait. The wake is unconditional. A spurious
one costs the waiter a re-scan, and a test for "is anyone waiting" would be a
second place that has to agree with the waiter. The mark is never inherited by
a later `rfork`, and once released it is never set again.

### The synchronous return

The spawner waits in `spawn_await_birth`, which is the vfork park generalized.
`await_child_release` holds the waiting discipline once for both callers: scan
the children under the table lock, and if the child is not released, register
a waiter on `child_waiters` in the same lock hold and sleep. They differ only
in the release predicate. `spawn_birth_released` is true when the child is
missing from the list, no longer ALIVE, or anything but UNBORN. A missing child
counts as released for the vfork reason: hanging a parent that cannot recover
is the worse outcome.

The wait breaks early only when its sleep returns `SLEEP_INTR`, and the sleep
is `sleep_death_only` (DEBUG-FS-DESIGN 5g), which returns it only when the
caller's group is dying. Its death then runs the orphan rule, which kills the
child. The caller's own terminate latch wakes the wait, which re-checks and
sleeps again. That latch is revocable, since a peer thread can install a
handler or open the notes file, so a wait that returned for it could hand its
caller a pid whose child is still loading. `birth_wait_survives_latch` is the
witness: a launcher's held child interrupts the launcher mid-wait, sees it
switched in and asleep again, and only then releases itself. The parent the body waits on is the
calling Proc, `current_thread()->proc`, because `rfork` forks the current Proc.
A kernel test that spawns on behalf of another Proc would otherwise wait on the
wrong children list.

The return is synchronous on purpose. With an asynchronous return, the
debugger's `stop` could land while the child is still inside `exec_setup`.
The sleep detour would park the child in whatever sleep it was in, with no EL0
frame, and the stop would settle. The debugger's first register read would
then return nothing. Waiting for the birth park closes that window for the
launcher. The cost is that the spawner waits while its child loads.

### The birth tail: a frame first, then the ordinary return

A held child's thunk calls `userland_enter_held` where every other spawn calls
`userland_enter`. The difference is the whole mechanism. `userland_enter` erets
straight from registers. `userland_enter_held` carves an exception frame below
the thunk's stack, zeroes it, and writes three fields: the return address is
the image's entry, the saved processor state is EL0 with interrupts clear, and
the EL0 stack pointer is the user stack. That is the frame the first
instruction would have been interrupted with. It then masks and runs the
ordinary EL0-return sequence over that frame: the preempt check, the
die-check, the park, which here is `el0_birth_park`, and then note delivery,
so a note posted while the child is held meets it as the park returns
(DEBUG-FS-DESIGN 4.2).
The thread finally leaves through the shared `.Lexception_return`, so
`KERNEL_EXIT` erets from the frame. A register the debugger wrote while the
child was parked, or a step it armed, takes effect.

The routine lives in `vectors.S`, beside the fork trampoline, for that
trampoline's reason: it ends at a label local to that file, the one audited
return to EL0. [[sub-kernel-exception]] treats it as the fifth way into
userspace and the second one that adds no `eret`. Because the frame is carved
below the thunk's stack, the kernel stack pointer after the `eret` is exactly
where `userland_enter` would have left it, and every later EL0 entry of the
thread lands its frame at the usual place.

`el0_birth_park` announces the arrival first: under the table lock it moves
the mark from UNBORN to PARKED, which wakes the spawner. When the hold was
released or converted before the child got there, the move is a no-op. It then
runs the shared park loop, `el0_stop_park`, with the birth wake condition. The loop publishes `debug_trapframe` as the frame, so `regs`,
`step` and `hwbreak` all address the first instruction.

The thread is not yet on its rendez when the spawner wakes, so a debugger's
`stop` can land before the park registers. That is covered by the stop's own
wait (DEBUG-FS-DESIGN 5e), which waits for the target to settle: every thread
registered on its own `debug_rendez`, sleeping and off-CPU.

### The hold is not a stop owner

`proc_stop_requested` reads the debug flag and the job flag (and, since 5g,
whether the group is dying), never the hold. That predicate drives the
`sleep()` detour and the 9P client's stop park (`client_stop_pending`). If the
hold were in it, an unborn thread would park inside `exec_setup` wherever it
slept, with no frame, and the birth wait would never be released. So the hold is
read in exactly one place, the birth park's wake condition. There the thread
holds no lock and no reader role, and it makes no syscall while held, so no
syscall-time park ever needs to see the hold.

### Conversion and release

Converting a hold and releasing one differ only in the order of two writes,
all under `g_proc_table_lock`:

- **`stop` converts.** It delivers the stop as usual (`proc_debug_stop_deliver`
  sets `debug_stop_req` and wakes the target's blocked threads), and only then
  clears the hold.
- **`start` and an explicit `detach` release.** They clear the hold first, and
  then run `proc_debug_resume`, whose wake finds the hold already gone.

The conversion, `proc_birth_hold_convert_locked`, refuses when no stop is
pending and leaves the hold standing. There is nothing to turn the hold into,
and clearing it would let the child run. The refusal also makes a stop verb
that cleared before it delivered visible, as a hold that outlives the verb,
where it would otherwise be an instant no test could catch. The release,
`proc_birth_hold_release_locked`, is its own setter and clears unconditionally.

`birth_park_wake_cond` reads the hold first, with ACQUIRE, and the stop flags
after. Both stores are RELEASE. A park that reads the hold as clear therefore
also reads the stop that was delivered before the clear, so at no instant can
it see neither and run. Reading the stop flags first would lose exactly that
case. The delivery's wake may rouse the parked thread, but it re-parks without
leaving the birth tail. From then on the child is in an ordinary debug stop.

The implicit release, the ctl fd closing without `detach`, clears the stop but
not the hold (`devproc_debug_release_cb`). The hold belongs to the spawner,
not to the attach slot, so a debugger that crashes after attaching does not set
the child running.

### Death wins, at every way out of the park

The park checks `group_exit_msg` at the top of every pass, and again after its
wake condition passes. The first check handles the ordinary case: `kill`, a
group termination and the orphan rule all wake the parked thread, and it dies
there. The second check exists because a release can follow a terminate. The
EXITKILL release terminates the group and only then clears the stop, and a
`start` sent after a `kill` clears after the kill. A thread that passed the
first check just before the terminate would read the cleared flags and `eret`.
Both clears are RELEASE stores ordered after the terminate, so the ACQUIRE
re-check sees it, and the thread dies without reaching EL0.

The first draft had only the first check. The spec found the gap, on the clean
held configuration's first run ([[spec-debug-stop]], `BUGGY_NO_DEATH_RECHECK`).
The window was never specific to the birth hold. The tail's ordinary stop park
runs the same loop, so the fix also closes it for every stopped thread,
debugger or job-control, and `debug_stop_buggy_no_death_recheck_tail.cfg`
keeps that half.

The park has no latch leg any more (5g). Its sleep returns early for group
death alone, which the loop's checks turn into the thread's exit. A latched
interrupt's wake never reaches it, and neither does a second stop's: those
walks pass a thread in a stop park by. (The leg it replaced re-read
`group_exit_msg`, because `thread_die_pending` reports group death and a latch
alike, and a kill landing after the top-of-pass check made it true: audit
round 1, F8. A sleep that reads group death alone cannot confuse the two.)

### The orphan rule

`proc_become_zombie_locked` calls `proc_birth_hold_orphan_rule_locked` before
it reparents the dying Proc's children, beside the PTY orphan rule. Every
ALIVE child whose hold is still set is `proc_group_terminate`d with the
message "launcher exited". That is safe under the table lock (the
`devproc_kill_walk_cb` idiom) and idempotent over a child already dying. The
trigger is the hold alone, not the attachment: a debugger that attached but
did not stop has not taken the hold over, so that child dies with its spawner
too. A converted hold is an ordinary debug stop, and EXITKILL governs it from
then on.

### A latched interrupt at the birth park

A held child stays held when an interrupt arrives (DEBUG-FS-DESIGN 5g, the
operator's vote of 2026-09-30). An interrupt that nothing catches arms the
LS-5c terminate latch, and its post's wake walk
(`proc_interrupt_terminate_wake`) passes the parked thread by: the park sleeps
death-only, so the wake could only be absorbed, and a thread run to absorb it
would read as unsettled to the debugger. The child is still ALIVE, still
parked, its mark still PARKED; a stop still converts it, and a start or detach
still releases it. Once it runs, it meets the note at its first note
checkpoint, the synchronous tail of its first syscall, where the default
terminate ends it with the note's name. A note latched while the child is
still loading is taken by the birth tail's own delivery before the park, and
the child dies before its first instruction. Leg (b) of
`held_spawn_death_wins` is the witness: the parked /hello child is not
switched in once across a 100 ms window after the post, stays ALIVE and held,
and once released dies "interrupt" at the return of libt `_start`'s first
syscall, `SYS_NOTE_MASK`.

The birth tail is therefore straight-line: the park returns only to proceed,
and nothing in it consults note delivery. Two earlier answers are gone. The
first draft re-ran the checkpoint in place and let note delivery consume the
latch. But note delivery declines a frame whose stack pointer it does not
trust, and the debugger can write that pointer while the child is parked. The
masked re-run then never ended. It took its CPU and every thread queued on
that CPU, and on a single CPU the whole machine (audit round 1, F1).
Sabotaged back in, it went round 34 billion times in the boot's 300 seconds
and took the test runner with it: the runner had woken the child onto its own
CPU and yielded to it. Its replacement ended the child inside the park with
the note's name (`birth_park_terminate`, since deleted). Leg (c) of
`held_spawn_death_wins` keeps the frame case: once it has asserted that the
stop was delivered and converted the hold, a converted child whose frame SP is
0 is not switched in by the stop's delivery nor, across a 100 ms window, by
the latch's post, and a kill still ends it there. [[spec-debug-stop]] keeps the
wrong answers as buggy configurations: `birth_latch_erets` (the park erets a
held child, against `NoEL0WhileHeld`) and `latch_ends_stop` (the park ends it,
against `ParkEndsOnlyInDeath`). `birth_latch_rerun` and `LatchedHeldChildEnds`
retired with the old rule.

## Data structures

- **`Proc.debug_birth_hold`**, a `u32` at offset 404: NONE (0), UNBORN (1),
  PARKED (2). Written only under `g_proc_table_lock`, always RELEASE. Read with
  ACQUIRE by the birth park's wake condition and by the spawner's release
  predicate.
- **`sys_spawn_args.debug_flags`**, a `u32` at offset 100 of the 104-byte spawn
  record. Four copies of the record live outside the kernel header: libt,
  libthyla-rs, the pouch process patch and the Go fork.
  `tools/check-spawn-args-mirrors.py` checks all four on every build
  ([[sub-kernel-syscall-abi]]).
- **The birth frame**, an `exception_context` carved on the child's kernel
  stack below the thunk's stack pointer. `debug_trapframe` points at it while
  the child is parked and is cleared on every way out of the park.

## Concurrency

Everything that decides the hold runs under `g_proc_table_lock`: the publication
store, both setters, the spawner's scan and waiter registration, the ctl
verbs, and the orphan rule. The lock never needs to be held across a sleep,
because each side registers its waiter in the same lock hold as the scan that
found the state still pending, which is the [[spec-death-wake]]
register-then-observe shape.

Two orderings carry the rest, and each is a RELEASE store paired with an
ACQUIRE load:

- **deliver-then-clear against hold-first.** A conversion stores the stop and
  then clears the hold. The wake condition reads the hold and then the stop.
- **terminate-then-clear against the re-check.** A release after a terminate
  stores `group_exit_msg` before the flag the wake condition reads, so the
  re-check after that read sees the terminate.

The park itself is the audited stop park. `sleep()` registers the thread on its
own `debug_rendez` under its wait lock and re-checks the wake condition there,
serialized against the clears' wake walks. Each thread parks on its own rendez.
A held child is single-threaded until it runs, but the park is the multi-thread
one regardless.

## Invariants enforced

**[[inv-i39]]** — execution control stays stopped-only: a held child runs
nothing until its hold is converted and resumed, or released, by the attached
owner. Die-with-launcher extends from the attach slot to the spawn itself: a
hold nobody took over dies with its spawner. The ask confers no access, so the
two-axis gate is untouched.

**[[inv-i24]]** — no EL0 after the group termination: the birth tail's
die-check, and the park's death checks at the top of each pass and after its
wake condition, the second of which also serves the tail's ordinary stop park.
The park's sleep returns early for group death alone.

**[[inv-i9]]** — no lost wake: every write out of UNBORN wakes the spawner
under the lock that its scan holds, and the park's sleep is the audited
register-then-observe.

## Error paths

- A bit outside `SPAWN_DEBUG_FLAGS_ALL` is refused with -1 twice: by the
  record validator at the syscall boundary, and again at the top of the spawn
  body, which kernel tests call directly. Both run before anything is
  allocated.
- If the `rfork` fails, the spawn unwinds exactly as an unheld one does and
  returns -1. There is no child, so there is no wait.
- If the child fails to load, the thunk exits before reaching the birth park.
  The child's ZOMBIE transition wakes the spawner, and the failure arrives as
  the child's exit status.
- If the caller is killed while waiting, the wait unwinds (#811), and the
  caller's own death runs the orphan rule on the child.
- A corrupt thread or Proc at the birth park extincts. The ordinary tail can
  skip a stop for a corrupt thread, but here a return would `eret` a held
  child.

## Performance

A spawn without the flag pays one boolean test. The ordinary EL0-return tail is
untouched: `el0_return_stop_check` keeps its fast path and calls the shared
park loop only when a stop is requested. A held spawn costs the spawner a wait
as long as the child's `exec_setup`, and the child one frame build and one
park.

## Prosecution

- **The hold must never join `proc_stop_requested`.** The detour would park an
  unborn thread mid-load with no frame and strand the birth wait.
- **Every write of the mark goes through the setters.** A write that skips the
  wake strands a held spawn.
- **Keep the two orders.** A conversion delivers and then clears. A release
  clears and then resumes. The wake condition reads the hold first. Swap any
  one of them and there is an instant where the park sees nothing holding it
  (`convert_clears_first`).
- **The park must re-check death after its wake condition.** A release that
  follows a terminate is legal on every path: EXITKILL, and `start` after
  `kill` (`no_death_recheck`, `no_death_recheck_tail`).
- **The birth tail must never `eret` while held, and the park returns only to
  proceed.** A latched interrupt's wake passes the park by, and would be
  absorbed by its death-only sleep if it reached it; the child stays held (5g). Anything in the park that consulted
  note delivery would reopen F1: delivery declines a frame whose stack pointer
  a debugger wrote, and a masked re-run spins forever.
- **The park neither consumes nor clears a latch.** The note belongs to the
  child's first checkpoint after the release, and it is delivered there with
  whatever disposition the child then has.
- **A conversion needs a pending stop.** Without one it must leave the hold
  standing, or a stop verb that clears before it delivers opens an instant
  with nothing holding the child.
- **The orphan rule runs before the reparent and keys on the hold alone.** After
  the reparent the children are no longer the dying Proc's to find
  (`orphan_hold_strands`).
- **The frame is zeroed before its three fields are written.** This path builds
  an EL0 context from nothing, so any field it does not write must be zero, or
  kernel state crosses into EL0.
- **A new field in the spawn record needs its own offset assert.** The mirror
  check reads the layout from those asserts and stops the build on a field that
  has none.

## Seams

- **A third party can stop a held child mid-load.** The sleep detour does not
  exclude an UNBORN thread. A debugger holding I-39 authority over the child,
  but not its spawner, can attach and `stop` it while it is still in
  `exec_setup`. The stop converts even an UNBORN hold, the child detour-parks
  with no frame, and a `regs` read returns nothing until it reaches the birth
  park. This is sound: nothing runs at EL0 before a deliberate release. But it
  is degraded, and the synchronous return closes the window only for the
  launcher. The same holds for every spawned child, held or not.
- **A spawner that execs keeps its children held.** The orphan rule fires at
  ZOMBIE. A spawner that replaces its image instead of exiting (a vfork child, a
  Linux-phenotype exec) leaves its held children held until it finally exits.
  That is bounded by the spawner's lifetime.
- **A revoked latch let a held spawn return early (CLOSED 2026-09-30, 5g).**
  The wait broke on its caller's own terminate latch, and a peer thread that
  revoked the latch (a handler installed, or the notes file opened) left the
  spawner alive with a child that might still be loading; the vfork park
  shared the wait, with the parent on a stack its child still borrowed. The
  wait now sleeps death-only, and `birth_wait_survives_latch` is the witness.
- **The held launch is every build's.** The Go fork's
  `SysProcAttr.DebugHeld` sets the flag, and ambush's `Launch` sets it. It
  rode a build tag (`thylacine_held`) while some trees' kernels lacked the hold
  and refused the flag. Main carries the hold since its aux-3 merge, so since
  ambush 073faaa an untagged build compiles the held launch. `tools/build.sh` refuses an older
  fork, whose untagged build would launch running ([[sub-substrate-build]]).
  `Launch` writes `exitkill` before `stop`: the stop ends the
  orphan rule's cover, so the mark has to be in place first. `/ambush-probe`
  stage C witnesses the held launch: its init script prints `regs` at the launch
  stop, and the PC must be the program's ELF entry. A launch that raced has
  always left the entry behind. A child spawned running is stopped at its first
  trap, because `userland_enter` does not look for a stop, so it reads at the
  entry only when an interrupt is already pending at its first eret. A held
  child that dies loading comes back from the spawn already dead, and the kernel
  refuses to kill a dead Proc, so a failed launch reaps it without waiting for a
  kill; stage D launches such a program and requires the reap.

## Caveats

- **The held spawn blocks.** The spawner waits for the child's whole load. A
  large image over 9P delays the spawn's return by exactly that load.
- **`/ctl/procs` does not show the hold.** A held child reads as an ordinary
  Proc in its listing. Adding a column was left out of scope.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
