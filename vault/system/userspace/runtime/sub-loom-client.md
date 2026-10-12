---
id: sub-loom-client
type: sub
title: "loom-client -- the Rust Loom client cores, host-tested"
parent: moc-userspace-runtime
code:
  - usr/lib/loom-client/Cargo.toml
  - usr/lib/loom-client/src/lib.rs
  - usr/lib/loom-client/src/service_abi.rs
  - usr/lib/loom-client/src/lease_ledger.rs
  - usr/lib/loom-client/src/paired_cq.rs
  - usr/lib/loom-client/src/owned_mapping.rs
  - usr/lib/loom-client/src/ring_payload.rs
  - usr/lib/loom-client/src/request_book.rs
  - usr/lib/loom-client/src/fixed_requests.rs
  - usr/lib/loom-client/src/fixed_futures.rs
  - usr/lib/loom-client/src/retirement_executor.rs
  - usr/lib/loom-client/src/local_tasks.rs
  - usr/lib/loom-client/src/local_wait.rs
  - usr/lib/loom-client/src/task_loop.rs
  - usr/lib/loom-client/src/tests/mod.rs
  - usr/lib/loom-client/src/tests/ring.rs
  - usr/lib/loom-client/src/tests/ring/composed.rs
  - usr/lib/loom-client/src/tests/ring/deadlines.rs
  - usr/lib/loom-client/src/tests/ring/executor.rs
  - usr/lib/loom-client/src/tests/ring/local_progress.rs
  - usr/lib/loom-client/src/tests/lease_ledger.rs
  - usr/lib/loom-client/src/tests/owned_mapping.rs
  - usr/lib/loom-client/src/tests/paired_cq.rs
  - usr/lib/loom-client/src/tests/request_book.rs
  - usr/lib/loom-client/src/tests/ring/seams.rs
  - usr/lib/loom-client/src/tests/request_book/seams.rs
audit: hard
guarded-by: [inv-i29]
validated-by: [prose]
locks: []
hazards: []
abis: [abi-loom-service, abi-loom-ring]
design:
  - "docs/ASYNC-SERVICE-ABI.md \"Native Rust client cores (October 12)\""
created: 2026-10-12
updated: 2026-10-12
---
## Purpose

The Rust twins of the native Loom client cores: the bookkeeping that keeps a
program's view of its requests, buffers and payload leases consistent with what
the kernel has accepted. Astra wrote and host-tested them uninstalled
(`work/oct5-as-r9/astra-takeover/owned-clients`). NA-3 installs them as the
crate `loom-client`, `no_std` with `alloc`; its test build is std, as the
original test binaries were. Unlike the C twins
([[sub-libt-loom-client]]) they keep the heap: libthyla-rs has `ThylaAlloc`, and
the caller-storage vote of 2026-10-12 concerned freestanding C.

They are a crate of their own, not modules of libthyla-rs, because libthyla-rs
cannot be built for the host (its `_start` is ELF assembly) and a test inside it
would run nowhere. None of the cores makes a syscall. As with `thyla-heap`, the
kernel calls arrive through traits the native adapter (NA-4) will implement in
libthyla-rs terms. Until it exists the cores are crate-private and only their
tests use them; `lib.rs` allows dead code outside test builds for that reason.

## Contract

- **The public surface is `service_abi`.** It moved here from libthyla-rs, which
  re-exports it as `loom::service_abi`. Everything else is `pub(crate)`.
- **The adapter's seams are three traits.** `RingBackend` (ring_payload) and
  `Mapping` (owned_mapping) are `unsafe trait`s whose `# Safety` sections are the
  adapter's obligations; `WaitSource` (local_wait) is the event source a turn
  parks on. Their contracts are Astra's, at each declaration.
- **Errors are typed.** No two outcomes share a value, so the C cores'
  collisions cannot arise. The full-table outcomes match C: the request book's
  `Error::Full` is C's `T_REQ_FULL` (-ENOSPC); the 64-member envelope answers
  `ring_payload::Error::Full` (C: -ENOSPC); the executor's `Error::Full` stays
  apart from a factory's error (C: -1001). The adapter maps `Full` to ENOSPC.
- **Allocation precedes admission.** `RingClient::register_pool` boxes the
  pool's whole state (`prepare`) before the backend's `register_pool`, and binds
  it afterwards without allocating. Allocation failure follows Rust's global
  policy (abort); nothing here recovers from it.
- **One creator thread.** The cores hold `Rc` and `RefCell` and are neither
  `Send` nor `Sync`. The only thread-safe piece is a task's waker flag
  (`local_tasks`).

## Mechanism

- **lease_ledger** -- one ledger per pool: receipt validation, lease state per
  member, owner ids from a process-wide counter that is never reused.
- **paired_cq** -- the single CQ consumer: reads a CQE and its receipt after an
  acquire load of the tail, publishes the head with release.
- **owned_mapping** -- validates the setup record's geometry (receipt region
  included) and the ring header before building the reader, and closes the
  mapping on any failure.
- **ring_payload** -- `RingOwner`, `RingClient`, `Pool`, `Payload` and `FixedIo`
  over one `Rc<Core>`. `State` holds `fixed[64]`, `pools[64]`, `last[64]` and
  the member count; the envelope refuses a pool that would pass 64 members. An
  uncertain registration quarantines the pool and closes the owner.
- **request_book** -- cookie correlation of CQEs to requests, cookies from a
  process-wide counter.
- **fixed_requests**, **fixed_futures** -- typed fixed I/O over a `RingClient`
  and a `RequestBook`, and `Future`s over it.
- **retirement_executor** -- a bounded queue (at most 64) of drivers awaiting
  retirement; admission checks capacity before it calls the factory.
- **local_tasks**, **local_wait**, **task_loop** -- at most 64 cooperative
  tasks with a rotating dispatch; one bounded wait turn (arm, recheck, wait);
  and their composition into one loop turn.

## Data structures

Per ring, `ring_payload::State`: `fixed: [Option<FixedState>; 64]`,
`pools: [Option<Box<PoolState>>; 64]`, `last: [u64; 64]`, a quarantine slot and
the member and live counts. A `PoolState` holds its ledger, its ranges and its
pending returns, all boxed at `prepare`. `RequestBook` holds a boxed slice of
entries, one per CQ slot.

## Concurrency

Single creator thread, enforced by `!Send`. Three process-wide atomics hand out
unique ids with `fetch_update` and `checked_add` at `Relaxed`, which is enough
for uniqueness: `NEXT_OWNER` (lease ledger), `NEXT_COOKIE` (request book),
`NEXT_TASK` (local tasks). A task waker sets an `AtomicBool` with release; the
dispatcher swaps it with acquire-release before it polls.

## Invariants enforced

- **I-29 (client side)**, as in C: the book admits at most `entries` outstanding
  requests and takes each terminal once. It bounds requests, not completions.
- **No allocation after admission.** The tests' counting allocator starts
  counting when the backend's `register_pool` returns and stops when the
  client's does; any allocation in that window fails the test. Bounded progress,
  a ready future and a wait turn are counted the same way.

## Error paths

- A local refusal (geometry, `Full`, `Busy`, `Stale`, `Closing`) returns before
  the backend is called and leaves the state unchanged.
- A definite kernel refusal of a pool registration restores the member count and
  returns `Backend(rc)`. An uncertain one quarantines the pool, closes the owner
  and returns `Uncertain(rc)`.

## Performance

Lookups scan at most 64 cells. A pool costs a few small allocations at
`prepare`; nothing on the completion path allocates.

## Prosecution

- **The install is mechanical.** `work/oct12-na3/na3_install.py --check` proved
  every installed core and test equal to rustfmt applied to Astra's original
  plus a listed edit set. A code edit may only swap or extend one path token, so
  no edit can delete a statement. The edits: "uninstalled" leaves six doc lines;
  `Error::Full` is added and the envelope answers it, and two ring tests expect it;
  one test allows `unreachable_code`, for a factory that must panic before it
  builds the future its type needs.
- **No test lost a line.** Astra's test files are cumulative copies (650 test
  definitions, 137 distinct). The install keeps `test_task_loop.rs` whole as
  `tests/ring.rs` and adds only the other files' missing lines as child modules;
  `--check` proves that every original test's lines are present in its
  installed namesake.
- **Three paths no original test reached.** Each original test binary had
  `#![allow(dead_code)]` at its crate root, which hid that no test called
  `ReadFuture::request_cancel`, built `WakeReason::Event` or read
  `Delivery::kind`. In the crate that allowance covers only its own module, so
  the test build reported all three. NA-3's own tests in `tests/ring/seams.rs`
  and `tests/request_book/seams.rs` cover them (140 tests in all), and
  `work/oct12-na3/seam-sabotage.py` breaks each path in turn: only its new test
  fails.
- **Mutants.** `work/oct12-na3/na3-mutants.py` carries all 110 of Astra's Rust
  mutants through the same derivation and counts one only when a test from the
  set her driver named fails, with her expected panic text where she gave one.
  The verdict comes from a run of that set alone, serially, as her binaries
  held only her own file's tests: in the whole suite, a test outside the set
  that panics during unwinding aborts the binary first. A no-op mutant must
  come out SURVIVED through the same path. Her 59 compile-fail cases run as
  probes inside the module that holds their fixtures, after the prelude her
  runner put before them, and must fail with her rustc code or message at the
  probe's own lines.
- **The ABI twins.** `tools/check-loom-service-abi.py` checks `owned_mapping::Setup`
  and `paired_cq::Completion` field by field against the ring layout table.

## Seams

- **The native adapter (NA-4)** implements `RingBackend`, `Mapping` and
  `WaitSource` over SYS_LOOM_*. It sets the book's `entries` to the ring's
  `cq_entries`, maps `Full` to ENOSPC, and parks in poll(2): a private ENTER
  never sleeps (operator vote 2026-10-12). The dead-code allowance in `lib.rs`
  goes when the adapter uses the cores.

## Caveats

1. The cores are compiled into every guest that links libthyla-rs, but no guest
   program calls them yet. Only the host runs them.
2. The equivalence with Astra's originals is NA-3's evidence, not a standing
   gate: the first later edit to a core ends it, deliberately.
3. `cargo test` runs the tests on parallel threads, which share the three
   process-wide counters. No test depends on a specific id.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
