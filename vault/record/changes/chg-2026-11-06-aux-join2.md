---
id: chg-2026-11-06-aux-join2
type: chg
title: "The aux join: aux-3 (the thread-model arc through ENOEXEC) reaches main through corona's branch"
date: 2026-11-06
arc: arc-phase6-pouch
commits: ["f48e20df4", "5b571b098", "672708939", "f5fe6d8e4", "d9898e61c", "f81f1f217", "6d2c65f84", "6d8af1673", "87a56ff3c", "b441f5c35"]
touched:
  - sub-kernel-filelock
  - sub-kernel-handle
  - sub-kernel-dev
  - sub-kernel-ninep-dev9p
  - sub-kernel-syscall-abi
  - sub-kernel-thread
  - sub-kernel-syscall-dispatch
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-06
---
Operator vote 34: aux-3 reaches main through corona's join. Branch aux-3-join2, off corona/async-memory at 9e0c6b87e; the merge is f48e20df4.

**The merge.** To compile, NOTE_TABLE moved to bit 25 and three session tests were brought under the frame bound (5b571b098). F_SETLKW restarts under SA_RESTART, and a pre-lock flush that a caught note cancelled answers EINTR (672708939). The native host checker pins ceiling 141 and the three collisions above 138 (f5fe6d8e4).

**Audit.** Fable 5.1 reviewed each round (start == end every time).
- Round 1, on f5fe6d8e4: 0/0/0/3. F1, a cancelled flush of another open let the take go out unsynced: the release sync now stops at it for a take (d9898e61c, narrowed in f81f1f217 so an unlock always goes on). F2, the libt / libthyla-rs / Pouch copies of the renumbered syscalls were pinned by nothing: a derived check now holds every mirrored number to the kernel enum by name. F3, the cancelled-flush witness now fsyncs, so it can tell a run kept staged from one latched.
- Round 2, on f81f1f217: 0/0/1/2. F1 [P2], the post-grant re-check's unlock could be cancelled on the wire under a claim the take left held, leaving the Proc's lock on the server for its life while the call answered EBADF (on main since B-3h): the work after the wait loop now runs uninterruptible (6d8af1673; that commit's message calls it "F2", which is a mislabel). F2, the Pouch read missed defines with a trailing comment: a census now fails on any unread define. F3, a vacuous control conjunct.
- Round 3, on 6d8af1673: 0/0/0/2, clean. F1: nothing would go red if the dispatcher's exit clear of `note_interruptible` were deleted. `filelock.wait`'s assert, which the reviewer took for its witness, was vacuous from B-3h2 (d1853e821): no note table governs the harness's Proc. `notes.native_class` now presets the flag and dispatches SYS_GETPID through the real wrapper; its control is the handler alone (87a56ff3c). F2 is tracked: a re-check unlock refused for a reason other than a cancellation keeps the granted entry. Dropping the entry would release a lock a peer thread took after the close. The fix is an owed unlock kept on the entry (OPEN-BUGS).

**Lease 1** (f5fe6d8e4): green 2420 plus guest legs; 36 of 37 sabotages red at their wants; E07 is the equivalent mutant the walkerr landing recorded; regreen ELF == green.

**Lease 2** (87a56ff3c): green 2421 plus guest legs. The narrow control (filelock., dev9p., notes.native_class) ran 142 tests green. J13-J25 were each red at their wants; J20 and J21 also reddened neighbouring latch tests. Regreen ELF == green, and test-fault passed 23/23. The two scenarios booted the Halcyon image test-fault had baked, so the harness refused them as IMAGE-FAIL (the lease script's order). The SMP matrix died at the UBSan link: image plus firmware reservation came to 0x810000 against the 8 MiB L3 window. b441f5c35 moves four test statics to the heap (0x7b0000, 320 KiB headroom), with no layout or KASLR change.

**Lease 3** (b441f5c35): green 2421 plus guest legs; viv-run PASS and r5f9-ash PASS (36 steps, not its skip); test-fault 23/23. The full SMP matrix (operator vote 37, this join only, N=10) was 10/10 PASS with 0 corruption in each of default-smp1, -smp4 and -smp8 and ubsan-smp4 and -smp8.

**Docs.** Six places the thread chapter's reading found out of date (6d2c65f84): ARCHITECTURE 8.12, 7.6, 7.6.2, 7.6.6, 7.9.1, 8.4.1, and five sub-kernel-thread pages.
