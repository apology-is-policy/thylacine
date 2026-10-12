---
id: sub-libt-loom-client
type: sub
title: "libt loom_client -- the native C Loom client cores over caller-provided storage"
parent: moc-userspace-runtime
code:
  - usr/lib/libt/include/thyla/loom_client/base.h
  - usr/lib/libt/include/thyla/loom_client/lease_ledger.h
  - usr/lib/libt/include/thyla/loom_client/paired_cq.h
  - usr/lib/libt/include/thyla/loom_client/owned_mapping.h
  - usr/lib/libt/include/thyla/loom_client/ring_payload.h
  - usr/lib/libt/include/thyla/loom_client/request_book.h
  - usr/lib/libt/include/thyla/loom_client/fixed_requests.h
  - usr/lib/libt/include/thyla/loom_client/retirement_executor.h
  - usr/lib/libt/include/thyla/bytes.h
  - usr/lib/libt/src/loom_client/fatal.c
  - usr/lib/libt/src/loom_client/lease_ledger.c
  - usr/lib/libt/src/loom_client/paired_cq.c
  - usr/lib/libt/src/loom_client/owned_mapping.c
  - usr/lib/libt/src/loom_client/ring_payload.c
  - usr/lib/libt/src/loom_client/request_book.c
  - usr/lib/libt/src/loom_client/fixed_requests.c
  - usr/lib/libt/src/loom_client/retirement_executor.c
  - usr/lib/libt/tests/loom_client/host_fatal.c
  - usr/lib/libt/tests/loom_client/test_fixed_fixture.c
  - usr/lib/libt/tests/loom_client/test_ring_payload.c
  - usr/lib/libt/tests/loom_client/test_lease_ledger.c
  - usr/lib/libt/tests/loom_client/test_paired_cq.c
  - usr/lib/libt/tests/loom_client/test_owned_mapping.c
  - usr/lib/libt/tests/loom_client/test_request_book.c
  - usr/lib/libt/tests/loom_client/test_composed_ring.c
  - usr/lib/libt/tests/loom_client/test_fixed_requests.c
  - usr/lib/libt/tests/loom_client/test_deadlines.c
  - usr/lib/libt/tests/loom_client/test_retirement_executor.c
  - usr/lib/libt/tests/loom_client/test_retirement_timers.c
  - usr/lib/libt/tests/loom_client/test_fatal.c
  - usr/lib/libt/CMakeLists.txt
  - tools/test-loom-client.sh
audit: hard
guarded-by: [inv-i29]
validated-by: [prose]
locks: []
hazards: []
abis: [abi-loom-ring]
design:
  - "docs/ASYNC-SERVICE-ABI.md \"Native C client storage (October 12)\""
  - "docs/ASYNC-SERVICE-STATUS.md \"NA-2b\""
created: 2026-10-12
updated: 2026-10-12
---
## Purpose

The client half of a private Loom ring, in native C: the bookkeeping that keeps
a program's view of its requests, buffers and payload leases consistent with
what the kernel has accepted. Astra wrote and host-tested these cores
uninstalled, as C and Rust twins (`work/oct5-as-r9/astra-takeover/owned-clients`).
NA-2b installs the C twins into libt. libt is freestanding (no heap, no memcpy,
no assert), and the operator voted on 2026-10-12 that the cores take
caller-provided storage, so they never allocate. The Rust twins keep ThylaAlloc
(NA-3). None of these cores makes a syscall: the native adapter that wires them
to SYS_LOOM_SETUP, REGISTER and ENTER is still to come.

## Contract

- **Storage is the caller's.** `T_FIXED_STORAGE(entries)` declares one ring
  client's storage: the fixed driver, its ring core (`struct t_ring_core`), the
  request book, 64 request cells, and `entries` book entries. `entries` is the
  CQ size, a power of two of at most 8192. `T_FIXED_CREATE(backend, &s, &d)`
  creates a driver over it. `struct t_owned_mapping` and
  `struct t_retirement_executor` are caller storage of their own.
- **Lifetime.** Storage is zero before its first use (static storage already
  is). It is reused only once it is idle: `t_ring_idle`,
  `t_fixed_storage_idle`, a destroyed mapping, an executor whose references have
  all dropped. Every create checks this and ends the process if the storage is
  live.
- **Full tables refuse before the kernel.** The request book answers
  `T_REQ_FULL` and the 64-member envelope answers `T_LOOM_CLIENT_ENOSPC`. Both
  equal -ENOSPC (-28), the errno the kernel's slot table, pool bank and request
  table answer. A pool registration tells a local refusal from the kernel's by
  `*native_rc`, which is zero when the refusal is local.
- **The executor's FULL stays -1001.** `t_retirement_admit` also returns the
  factory's own errors, and a factory over the kernel can answer -ENOSPC. Only
  `T_RETIREMENT_FULL` says that the factory never ran, so the caller still owns
  the backend the factory would have consumed.
- **Broken invariants are fatal.** Every check that was assert or abort is now
  `T_LOOM_CHECK`, which calls `t_loom_client_fatal(func, line)`. libt's
  definition writes `loom-client: broken invariant in <func>:<line>` to fd 2 and
  then calls `t_exit_group(134)`. The checks are always compiled in: NDEBUG does
  not remove them.
- The other per-function contracts are Astra's, unchanged. They are written at
  each declaration in the headers.

## Mechanism

- **Ring core (ring_payload.c).** `struct t_ring_core` holds every table at its
  ABI bound:
  - `fixed[64]`, one fixed cell per registered buffer;
  - `pool_cells[64]`, the pool headers;
  - `bank` (`struct t_lease_bank`), 64 pool-member cells shared by every pool;
  - `leases[64]`, the payload lease for each bank cell, at the same index;
  - `pools[64]` and `last[64]`, indexed by the kernel's descriptor slot.

  `t_ring_create` zeroes the core and marks it in use. When the last handle
  drops, `maybe_free` destroys the backend and zeroes the core again, which
  returns every header, cell and lease at once.
- **Pool admission (prepare).** The check `n > 64 - members` answers ENOSPC
  first. Then `claim_pool` takes a free header and `t_lease_prepare` claims `n`
  bank cells. The cross-pool and fixed-cell overlap checks run after that and,
  on a refusal, `release_pool` returns both claims. A header can always be
  found, because each claimed header holds at least one counted member. The bank
  can always supply the cells, because cells in use equal `members`. So a claim
  failing is a broken invariant, not an error. The backend's `register_pool`
  (the kernel registration) runs only after both claims.
- **Member lookup (lease_ledger.c).** A bank cell records the owning ledger's
  unique owner id (from an atomic counter that is never reused) and its ordinal.
  `member()` scans the 64 cells for (owner, ordinal), as the kernel's
  `loom_pool_bank` does. `t_lease_cell` gives the index, which also indexes
  `leases[]`. `t_lease_release` frees the ledger's cells, charged or not. It
  sets count to 0, so no later lookup reaches the scan.
- **Request cells (fixed_requests.c).** A request cell is claimed only after its
  book entry and its fixed cell are both reserved, so a refusal claims nothing.
  The cell holds that fixed handle until write completion, read-result drop or
  request drop. A ring has 64 fixed cells and every claimed cell holds a live
  one, so a free cell always exists. A completed read's result lives inside its
  request cell, and `t_fixed_read_drop` releases the cell.
- **Executor (retirement_executor.c).** Each cell embeds its managed client.
  `client_alive` and `&cell->client == c` guard `t_managed_drop`. Progress zeroes
  a reclaimed cell.
- **Byte helpers.** `t_bytes_copy` and `t_bytes_zero` (`thyla/bytes.h`) are
  plain loops. With `-fno-builtin` they never become calls.

## Data structures

| Object | Size bound | Notes |
|---|---|---|
| `struct t_ring_core` | fixed (64-entry tables) | one per ring; zeroed at create and at final release |
| `struct t_lease_bank` | 64 cells | owner 0 = free; shared by every pool of the ring |
| `struct t_fixed_storage` | fixed + 64 request cells | plus `entries` book entries from `T_FIXED_STORAGE` |
| `struct t_request_entry[entries]` | CQ size, at most 8192 | zeroed by `t_request_create` |
| `struct t_owned_mapping` | fixed | `live` from create to destroy |
| `struct t_retirement_executor` | 64 cells | live while `refs` is nonzero |

## Concurrency

Unchanged from the uninstalled cores: single creator thread, no reentry from
callbacks. The two process-wide counters, the lease owner ids and the request
cookies, are atomics. They are lowered inline as LL/SC at the ARMv8.0 floor, and
the toolchain does not use outline atomics.

## Invariants enforced

- **I-29 (client side).** The request book holds at most the CQ size in
  outstanding requests, so a client cannot submit more completions than its CQ
  can hold. Exactly one terminal per request: `take_terminal` frees the entry,
  and a second take is STALE.
- **Pre-reserved local metadata.** No local claim follows a kernel registration.
  The ring test asserts this: at the moment the registration callback runs, the
  claims already equal `before + 1 + n`.
- **No allocation, no mem\* call.** The freestanding stage of
  `tools/test-loom-client.sh` checks it with `llvm-nm`. The `-O0` control shows
  that the check can fail.

## Error paths

- A local refusal (geometry, ENOSPC, STATE) leaves the storage unchanged. This
  is asserted by claim counts and by `t_fixed_occupied` / `used_requests`.
- A definite kernel refusal of a pool registration releases its header and
  cells. An uncertain one quarantines them until the core's final release.
- A create that fails consumes the backend (Astra's contract). The storage stays
  idle.

## Performance

Lookups are 64-cell scans (members, request cells, headers), bounded by the ABI.
Create zeroes about 30 KB of core plus the book entries.

## Prosecution

- **Storage reuse.** A create on live storage ends the process. `test_fatal.c`
  witnesses this for each of the four storage types, and witnesses a double
  owner drop and a drop of a consumed request. A child passes only by exiting 77
  from `t_loom_client_fatal` in the named function, so a test assert (SIGABRT)
  cannot pass it.
- **Bank aliasing.** A ledger freed and reclaimed in the middle of the bank
  maps every ordinal to its own cell (`test_lease_ledger.c`, the
  `t_lease_cell == 0, 1, 4` case). A pool re-registered into a destroyed pool's
  header and cell reads its own bytes (`test_ring_payload.c`, slot 5
  incarnation 2).
- **Idle accounting.** Every host test ends by asserting that each storage
  object it created is idle.

## Seams

- **The native adapter** (backend callbacks over SYS_LOOM_*) is not written.
  Until it is, `t_ring_backend` and `t_mapping_backend` are only test fixtures.
- **The Rust twins** (NA-3) keep the old local codes (`T_REQ_FULL` = -2,
  `T_LEASE_BUSY` for the envelope), so C and Rust differ until NA-3 aligns
  them.

## Caveats

1. libt's `t_loom_client_fatal` is compiled and linked by the freestanding
   stage, but no boot executes it. The host tests use their own definition
   (`host_fatal.c`).
2. At -O0, clang lowers by-value struct copies (the backend, the setup record,
   receipts) to memcpy. The toolchain builds -O2, and `--no-undefined` refuses
   such a link. The runner's control proves that refusal.
3. A stale handle used after its storage is re-created aliases the new object.
   The `alive`, `live` and `client_alive` checks catch a stale use only until
   the storage is reused.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
