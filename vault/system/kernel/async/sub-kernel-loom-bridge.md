---
id: sub-kernel-loom-bridge
type: sub
title: "The private Loom syscall bridge and its qualification lever"
parent: moc-kernel-async
code:
  - kernel/loom_service_syscall.c
  - kernel/include/thylacine/loom_service_syscall.h
  - kernel/test/loom_bridge_fixture.h
  - kernel/test/test_loom_bridge_probe.c
audit: hard
guarded-by: [inv-i5, inv-i7, inv-i29, inv-i30, inv-i32]
validated-by: [spec-loom-sq]
locks: []
hazards: []
abis: [abi-loom-ring, abi-loom-service]
design:
  - "docs/ASYNC-SERVICE-ABI.md"
  - "docs/ASYNC-SERVICE-BUFFERS.md"
created: 2026-10-12
updated: 2026-10-12
---
## Purpose

Until this bridge, the private service stack in [[sub-kernel-loom]] and
[[sub-kernel-loom-pools]] could be reached only from in-kernel fixtures. No
syscall could create a private ring. This file is the one route from the
existing Loom syscalls (SETUP, REGISTER, ENTER) into that stack, so that native
clients can be qualified against the real ABI before activation.

The route exists only in a kernel built with the qualification lever (operator
vote, 2026-10-12). Every default image refuses it.

## Contract

The binding text is docs/ASYNC-SERVICE-ABI.md, section "Qualification bridge
(October 12)".

**The lever.** The build-config symbol `PRIVATE_ASYNC_QUAL` (default n, in no
preset) sets the CMake option `THYLACINE_PRIVATE_ASYNC_QUALIFICATION`, which
defines `LOOM_PRIVATE_QUALIFICATION`. CMake announces "not a release default",
and build.sh builds that kernel into `build/kernel-private-async-qual`, so it never
replaces the default kernel. `tools/test.sh --private-async-qual` boots it and
refuses when its ELF is missing.

**Without the lever**, `loom_private_routed()` is false and every entry in
`loom_service_syscall.c` is a stub that returns -EOPNOTSUPP:

- SETUP with PRIVATE_SERVICE or SERVICE_BUFFERS set;
- REGISTER ops 2..9;
- the private arm of BUFFERS and ENTER, which no ring can reach because no
  private ring can exist.

The test `loom.private_bridge` pins this (default body) and also checks that a
refused setup charges nothing.

**With the lever:**

- **SETUP.** flags must be PRIVATE_SERVICE, alone or with SERVICE_BUFFERS.
  Anything else (SERVICE_BUFFERS alone, SQPOLL, CQSIZE, an unknown bit) is
  -EINVAL. entries is a power of two in 1..LOOM_MAX_ENTRIES, and cq is twice
  entries. Creation refusals map to errno in `loom_create_private_why`:
  - -EAGAIN: the retirer is not up;
  - -EINVAL: geometry or Proc;
  - -EBUSY: the address space is shared, or its private-ring guard count would
    wrap;
  - -ENOMEM: budget or allocation.

  A thread-budget refusal starting the executor is -EAGAIN, and a full handle
  table is -EMFILE.
- **REGISTER.** BUFFERS on a private ring is private registration. HANDLES on a
  private ring is -EOPNOTSUPP. Ops 2..9 take nargs 1 and the record's address.
  The record is copied once, at the size its op fixes
  (`loom_private_record_size`), into a kmalloc'd buffer, and is never read from
  the user again. TARGET, SLOT and POOL publish only after the result has been
  copied back. A faulted copyout rolls the reservation back (burning its
  incarnation) and returns -EFAULT. QUERY and POOL QUERY write only on success.
- **ENTER never sleeps.** min_complete must be 0 and flags may hold only
  NONBLOCK. ENTER submits through `loom_private_submit`, which copies each SQE
  before `loom_sq_release` publishes sq_head ([[spec-loom-sq]]). A caller
  outside the creator image gets -EACCES. Clients park in poll(2).

## Mechanism

### Setup: everything the caller learns is read before the handle exists

`loom_private_setup_for_proc` creates the ring with `loom_create_private_why`,
which charges the ring's pages with its metadata and records that charge on the
ring Burrow. It then maps the ring and tags the VMA `VMA_FLAG_PRIVATE_RING` in
one `as->lock` hold, so no clone can see the mapping untagged: fork omits a
tagged VMA, and exec closes the ring. Next it starts the executor and fills
`*out`, including the receipt geometry in `_resv1[0..2]` when SERVICE_BUFFERS is
set. Only then does it call `handle_alloc`. Once the handle is installed, a
sibling thread may close it, so `l` is not touched again.

On failure, the unwind is a settling detach (`sys_burrow_detach_for_proc`) of the
mapping, if one was made, followed by `loom_unref`. Because the charge is
recorded on the ring, the mapping adds none, and whichever drop ends the ring
settles it exactly once.

### Rollback when params cannot reach the caller

The SETUP handler copies params out byte by byte, holding its own reference
on the ring from the held setup of either route. A fault calls
`sys_loom_setup_rollback`, which closes the descriptor only while its slot
still names this ring (`handle_close_obj`), detaches only the mapping that is
still exactly the ring (`sys_loom_ring_detach`), and then drops the reference.
The descriptor and the mapping were published before the copyout, so a sibling
thread may have reused the number or the VA by then; until 2026-10-13 the
rollback closed by number and detached by range, and so closed or unmapped
whatever was there. The rollback serves the legacy and the private ring alike.
Its detach goes through the charge-settling core, never `burrow_unmap`, which
drops the mapping with no payer: when that is the ring's final drop, an
unsettled drop loses the charge record and leaks the ring's pages from the
caller's budget. That was a live P3 on main (OPEN-BUGS 08:43Z, 2026-10-12).

### The legacy setup reference

`sys_loom_setup_for_proc` used to write `l->owner`, `ring_pages`, the charge
record and `*out` after `handle_alloc` had adopted the Loom's only reference. A
sibling's close of the new fd in that window freed the Loom synchronously, so
those writes landed in freed memory: an EL0-reachable kernel UAF write, latent
P1 (OPEN-BUGS 08:46Z, 2026-10-12). The setup now takes `loom_ref` before
`handle_alloc` and drops it as its last act. The test hook
`loom_setup_installed_for_test` sits at the window, and `loom.setup_close_race`
closes the fd there.

### Register: publish after copyout, through one helper

`finish_reservation` copies out the result reference and publishes only if the
copyout succeeded. If either step fails, it rolls back. A reservation is the
calling thread's alone until it is published, so its rollback cannot meet a
competitor, and a failed rollback is a broken invariant rather than an input
error: it extincts with "private register lost its reservation rollback". The
syscall-side record copy and copyout live in `sys_loom_register_private` and
`loom_record_copyout_user` (kernel/syscall.c). This file takes kernel copies and
a caller-supplied copyout, so fixtures drive it with a faulting copyout.

### The EL0 probe (NA-2a)

The fixtures call the inners directly, with a kernel-side copyout. Only a real
syscall from EL0 reaches the handlers' argument decoding and their byte-wise
usercopy. `usr/loom-bridge-probe` is a native C program over libt's
`thyla/loom.h` ([[sub-kernel-syscall-abi]]), shipped in every ramfs. The test
`loom.bridge_probe` spawns it and holds it to the verdict the kernel's own
lever implies: exit 70 without the lever, 71 with it, anything else a failure.
It may skip only a kernel booted without userspace: when the ramfs carries
`bin/joey` but not the probe, the ramfs is stale and the test fails.
The probe infers its mode from its first private setup. Because the expected
code comes from the kernel's build and not from the probe, a kernel that routes
when it should refuse fails the test, and so does the reverse.

Its fault pages are one `SYS_BURROW_RESERVE` cut by `SYS_BURROW_PROTECT` into
RW, R and none, never unmapped. The setup that is meant to fault creates and
maps a ring first. If the fault page were a hole, that ring could land in it
and the copyout would succeed.

What it checks, by image:
- **Default.** Each private setup returns -EOPNOTSUPP without writing past
  `flags`, and register ops 2..9 are refused before any fd lookup. Afterwards
  an ordinary ring gets the same fd and address as before the refusals.
- **Qualification.** It checks:
  - the setup refusals;
  - the geometry the kernel wrote, against the mirror. Every setup runs over
    params pre-filled with 0xA5, so a zero read back (the reserved words) is the
    kernel's. `flags` is the probe's own IN word, so its check pins the grant.
    A fresh ring's header words are zeroed memory; `hdr.flags` and `overflow`
    never become non-zero here, so their offsets rest on the static asserts and
    `tools/check-loom-service-abi.py`.
  - two copyout faults. Read-only params fault at byte 0 and leave nothing.
    Params running into a read-only page leave bytes 0..15, whose `sq_entries`
    and `cq_entries` show the ring existed. After both, the next setup must get
    the same fd and address.
  - a SLOT record's copyin fault, which burns no incarnation, against its
    result-copyout fault, which burns exactly one. The copyin is one loop shared
    by every op. Result-copyout faults are also driven for QUERY, POOL and POOL
    QUERY; TARGET's needs a registry and is faulted in-kernel;
  - the argument refusals (nargs, null, kernel address, closed fd, a pipe, an
    unknown version, op 10, which answers -EINVAL in both images since vote 6);
  - QUERY, REAP and ABORT, a TARGET with an absent registry fd, and the pool
    round trip;
  - ENTER's refusals and three rejected SQEs. An unknown opcode completes
    -EOPNOTSUPP and a NOP without an incarnation -EINVAL, which pins the
    incarnation word. A CLUNK with an offset completes -EINVAL from the CLUNK
    arm alone, which pins the opcode byte: an opcode read as zero would answer
    -EOPNOTSUPP. The CQEs are reaped after poll(2) reports POLLIN, and poll must
    report nothing once they are reaped.

The test counts the private rings created and retired across the probe's run
(`loom_private_created` beside `loom_private_retired`). It waits until every
ring created has retired, then requires exactly four created in the
qualification image and none in the default one. A count that only waited for
the retired total to reach four would pass a fifth ring that retired late.

## Data structures

None of its own. The record buffer is kmalloc'd per call (the largest record,
POOL create, is 1568 bytes), not stack-allocated.

## Concurrency

- Setup takes `as->lock` for the gap search, map and tag. It holds no other lock
  across `handle_alloc`.
- Register and ENTER run with the Loom retained by `handle_get` for the whole
  call. The private stack takes `l->lock` itself.
- The bridge adds no wait. ENTER never sleeps, and poll's park on a private ring
  registers before it rechecks (`loom_poll`), which is the I-9 shape.

## Invariants enforced

- **I-32.** Every ring a setup creates is settled exactly once on every unwind:
  the create's own refusals, executor start, `handle_alloc`, and the params
  copyout fault. `loom.setup_rollback_settles` and both bridge bodies assert
  the exact page count after rollback; `loom.setup_rollback_identity` and
  `loom.private_setup_rollback_identity` assert that a rollback after a sibling
  reused the number or the VA leaves the sibling's file and region alone.
- **I-5.** The private-ring VMA tag is set under the same lock hold as the map.
  The tag is what keeps the ring in its creator's image: fork omits it, and
  `SYS_WEFT_SHARE` refuses it (audit close S1).
- **I-29 / I-30.** ENTER copies before release, through the single sq_head writer.

## Error paths

The errno set is fixed by the ABI section: EOPNOTSUPP, EINVAL, EAGAIN, EBUSY,
ENOMEM, EMFILE, EFAULT, EACCES, ECANCELED and EBADF. No errno is added. The
legacy path keeps returning -1.

## Performance

Off the data path. The extra cost is one kmalloc per register record.

## Prosecution

- Is anything touched after `handle_alloc` in either setup? (The legacy fix is
  the precedent.)
- Can a refused or unwound setup leave a charge, a mapping, an executor or a
  handle behind?
- Can a default image reach any private route?
- Is a reservation ever visible before its result copyout?

Mutants M3-M6 in `work/oct12-na1/na1-mutants.py`:

- M3: release before the copy;
- M4: no own reference in the legacy setup;
- M5: the unsettled unmap;
- M6: publish before copyout.

Each is predicted red by name in `na1-run.sh`.

## Seams

- **NA-2.** NA-2a (the EL0 probe and libt's ring mirror and stubs) is above.
  NA-2b installs the owned-client C cores over those stubs. They allocate and
  native C has no heap, so how they get storage is an open design question.
- **NA-3.** The Rust adapter.
- **NA-4.** poll wake/park and creator/control wiring.
- Activation of the whole arc is a separate gate. This lever is a
  qualification vehicle.

## Caveats

- **NA-2a audit and guest run (2026-10-12).** Round 1 (Fable 5.1 on Opus 5.5)
  returned 0 P0, 0 P1, 1 P2 and 6 P3, plus one self-found P3; all are closed in
  dc7feeb0f except the Rust half of F5, which moves to NA-3. The legs ran green
  on caf3e2b19 (`work/oct12-na2a/run-20261012T144128Z/summary.txt`): both
  controls 1870/1870, and eight mutants, each failing exactly `loom.bridge_probe`
  at its own message. SMP and the sanitizers are not covered (the Mac-gate
  residual). The ramfs hash differs per bake because each bake mints a new pool
  key into `bin/system.key`; the runner proves the probe by extracting it.

- **NA-1 audit round 1 (2026-10-12).** Fable 5.1 reviewed Opus 5.5's work and found
  0 P0, 0 P1, 0 P2 and 5 P3; I found one P2 myself.
  - **S1 (P2).** `SYS_WEFT_SHARE` admitted a private ring's ANON mapping, so a
    CAP_HW_CREATE holder could export its ring. Fixed by refusing the VMA tag.
    Witness: the qualification body; mutant M7.
  - **F1.** test.sh's hint built a kernel without the suite. Fixed: the hint now
    names `--config ci`.
  - **F2 (closed, not fixed).** The rollback closes by fd number and detaches by
    VA, and a sibling thread can reuse either before it runs. That harms only
    the Proc racing its own syscall, every ledger still settles once, and the
    fix would be an identity-checked close.
  - **F3.** M4 is nondeterministic if the freed page is reallocated in the
    window. Noted in its leg.
  - **F4.** Stale "0 / -1" contracts in loom.h. Fixed.
  - **F5.** The qualification body now drives an ordinary ring into both private
    inners.
- **Before the close:** both kernels built from 6308d5b2b and each passed one
  boot at 1869/1869 (`work/oct12-na1/prebuild-20261012T093313Z`). Nothing in a
  boot log shows which kernel ran. M6's extinction is the proof that the
  qualification kernel booted.
- **Guest legs on the close commit 3f816c8a4 are green**
  (work/oct12-na1/run-20261012T094440Z):
  - both controls passed 1869/1869;
  - mutants M3 through M7 each discriminated by name, and M6's extinction proves
    which kernel booted;
  - both kernels rebuilt byte-identical;
  - the sq_head guard and the production shape both hold.
- **Still unproven:** EL0 usercopy (NA-2), SMP and sanitizers. The Mac-axis
  residual is recorded per the 2026-10-07 vote.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
