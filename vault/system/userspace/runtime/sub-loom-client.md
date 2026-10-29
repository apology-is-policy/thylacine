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
updated: 2026-10-13
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
would run nowhere. None of the cores makes a syscall. The native adapter
([[sub-loom-native-adapter]]) composes them over a public `Kernel` trait that
libthyla-rs implements, as with `thyla-heap`. The cores stay crate-private. The
adapter does not compose the futures over fixed I/O, the retirement executor or
the task loop; only their tests use them, so `lib.rs` still allows dead code
outside test builds.

## Contract

- **The public surface is `service_abi` and the adapter, `native`.** The mirror
  moved here from libthyla-rs, which re-exports both as `loom::service_abi` and
  `loom::native`. `owned_mapping::Setup` is public because `Kernel::setup` takes
  it; every core is `pub(crate)`.
- **The adapter's seams are three traits.** `RingBackend` (ring_payload) and
  `Mapping` (owned_mapping) are `unsafe trait`s whose `# Safety` sections are the
  adapter's obligations; `WaitSource` (local_wait) is the event source a turn
  parks on. Their contracts are Astra's, at each declaration.
- **Errors are typed.** No two outcomes share a value, so the C cores'
  collisions cannot arise. The full-table outcomes match C: the request book's
  `Error::Full` is C's `T_REQ_FULL` (-ENOSPC); the 64-member envelope answers
  `ring_payload::Error::Full` (C: -ENOSPC); the executor's `Error::Full` stays
  apart from a factory's error (C: -1001); a future driver's full registration
  table answers `Storage(Full)`. The adapter maps `Full` to ENOSPC.
- **Allocation precedes admission.** `RingClient::register_pool` boxes the
  pool's whole state (`prepare`) before the backend's `register_pool`, and binds
  it afterwards without allocating. Allocation failure follows Rust's global
  policy (abort); nothing here recovers from it.
- **One creator thread.** The cores that own state hold `Rc` and `RefCell`,
  so they are neither `Send` nor `Sync`. Plain value types (`LeaseLedger`,
  `Completion`, `Record`, `Delivery`, `Setup`) are both; a ledger changes only
  through `&mut`. The only piece shared across threads is a task's waker flag
  (`local_tasks`).

## Mechanism

- **lease_ledger** -- one ledger per pool: receipt validation, lease state per
  member, owner ids from a process-wide counter that is never reused.
  `refuse_read` runs `accept_read`'s checks for a payload no one will hold
  (its ring or pool is closing), advances the member's lease order and hands
  the receipt back for RETURN; `Pool::refuse` wraps it, so a replayed record
  can never RETURN a member a live payload still reads.
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
  process-wide counter. At whole-owner retirement `clear_retired` discards only
  abandoned entries (since NA-4b); a live owner takes or drops its own. `Kind::Slot` (NA-4b) is CONNECT's and WALK's: their
  success result is the reserved destination slot, which the book requires
  exactly; `Kind::Control` keeps accepting no positive result.
- **fixed_requests**, **fixed_futures** -- typed fixed I/O over a `RingClient`
  and a `RequestBook`, and `Future`s over it. Since NA-4b the fixed driver is the
  ring's one request driver: `request` reserves control, slot and pooled entries
  (`BookRequest`, whose drop cancels or abandons and requests the scope abort),
  `register_pool` reaches the client, and `route` dispatches each accepted CQE by
  kind. `receive` is `route` for callers that hold only fixed requests.
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

Lookups scan at most 64 cells, except the request book's, which scan its
entries: one per CQ slot, at most 8192. A pool costs a few small allocations at
`prepare`; nothing on the completion path allocates.

## Prosecution

- **The install is mechanical.** `work/oct12-na3/na3_install.py --check` proved
  every installed core and test equal to rustfmt applied to Astra's original
  plus a listed edit set. A code edit may only swap or extend one path token, so
  no edit can delete a statement, and a doc edit must stay one `//!` line. The
  edits: "uninstalled" leaves seven doc lines; `Error::Full` is added and the
  envelope and the future driver's registration table answer it, and two ring
  tests expect it;
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
  and `tests/request_book/seams.rs` cover them, with a fourth for the
  registration table's `Full` (141 tests in all), and
  `work/oct12-na3/seam-sabotage.py` breaks each path in turn: only its new test
  fails.
- **Mutants.** `work/oct12-na3/na3-mutants.py` carries all 110 of Astra's Rust
  mutants through the same derivation and counts one only when a test from the
  set her driver named fails, with her expected panic text where she gave one.
  The verdict comes from a run of that set alone, serially, as her binaries
  held only her own file's tests: in the whole suite, a test outside the set
  that panics during unwinding aborts the binary first. A no-op mutant must
  come out SURVIVED through the same path. Her 59 cases run as probes inside
  the module that holds their fixtures, after the prelude her runner put before
  them: 44 must fail with her rustc code or message at the probe's own lines,
  and 15 positive controls must compile and pass.
- **The ABI twins.** `tools/check-loom-service-abi.py` checks `owned_mapping::Setup`
  and `paired_cq::Completion` field by field against the ring layout table.

## Seams

- **The native adapter** ([[sub-loom-native-adapter]]) implements `RingBackend`,
  `Mapping` and `WaitSource` over its `Kernel` trait. It sets the book's
  `entries` to the ring's `cq_entries`, maps `Full` to ENOSPC, and parks in
  poll(2): a private ENTER never sleeps (operator vote 2026-10-12). It uses
  `RingOwner::abort_scope` for an abandoned control or pooled request and
  `Pool::identity` for a stream's SQE.

## Caveats

1. The cores are compiled into every guest that links libthyla-rs, but no guest
   program calls them yet. Only the host runs them.
2. The equivalence with Astra's originals was NA-3's evidence, not a standing
   gate. NA-4b's edits (`Kind::Slot`, the request driver's `route` and
   `BookRequest`, `RingOwner::abort_scope`, `Pool::identity`, the public `Setup`,
   `clear_retired` keeping a live owner's entry)
   end it, deliberately: `na3_install.py --check` and `na3-mutants.py` are NA-3's
   historical evidence and no longer apply to the tree. The NA-4b audit close
   ran rustfmt (edition 2024) over the whole crate: it reflowed one line of
   `route` and removed a doubled blank line in `service_abi.rs`; the crate is
   rustfmt-clean, which no gate checks yet.
3. `cargo test` runs the tests on parallel threads, which share the three
   process-wide counters. No test depends on a specific id.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
