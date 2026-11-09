---
id: sub-kernel-accounts
type: sub
title: "Memory accounts -- the durable tree and its physical ledger"
parent: moc-kernel-memory
code: [kernel/account.c, kernel/include/thylacine/memaccount.h, kernel/test/test_account.c]
audit: hard
guarded-by: [inv-i7, inv-i32]
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/SHARED-MEMORY-ACCOUNTING.md"]
created: 2026-11-03
updated: 2026-11-04
---
## Purpose

Say who paid for every page a user induced, in a tree that outlives the Procs that
used it. The 128 MiB limit can only be replaced once this is known to be exact, so MM-1
builds the ledger and lets it watch. Nothing here refuses an allocation.

## Contract

MM-1a and MM-1b booted; MM-1c written, not booted. `docs/SHARED-MEMORY-ACCOUNTING.md`
section 14 is the as-built statement; sections 3 and 4 are the design it serves.

- Every buddy block the capacity pool counts, and every DMA block, carries a sponsor on
  its head page from allocation to free. The sponsor is never NULL.
- A charge is the whole block, including the buddy's rounding.
- `account_pool_share()` equals `capacity_pool_charged()` except while a block is landing
  or leaving. `account.ledger_reconciles_with_the_pool` checks that at three instants, and
  `capacity-probe` checks it on the live machine, where every class of page is held.
- ALLOC is the sponsored blocks that exist, and the pool is the PG_USER blocks plus thread
  pages: `account.blocks_that_exist_are_the_ledger` walks the frames to say so.
- A block freed at the wrong order refunds its own order to the ledger and the caller's to
  the pool. Giving both the caller's would leave them equal and the fault invisible.
- An account lives until the last of its Procs, threads and blocks has gone, so an
  orphan stays charged to its ancestors. Its parent never changes.
- A Proc's account never changes, and a fork inherits it, so no fork resets anything.
- `account_query_fill` reports bytes and decides nothing. `SYS_ACCOUNT`'s query and
  `/proc/<pid>/account` both render it, so they differ only by when they read.
- An account's control is a `KOBJ_ACCOUNT` handle in the Proc that made it, and it goes
  nowhere else ([[sub-kernel-handle]]), and an exec closes it whatever its flag says. A spawn
  tail names the handle and the incarnation, as an operation's record does; an incarnation
  that is another account's answers `-ESTALE` in both (operator vote 76), and 0 on a handle
  `-EINVAL` ([[abi-memory-account]]). `SET_LIMITS` needs the handle, so nobody adjusts the
  account they are charged to.
- `account_set_limits` replaces every class or none, and refuses a figure above any
  ancestor's. A limit decides nothing until MM-2.
- A child costs its parent one `CLASS_META` page and the pool one page, until its last
  reference goes.

## Mechanism

`alloc_user_pages` sponsors a block to the current thread's Proc's account;
`alloc_user_pages_for` names one. `free_pages` is the one way off, as it is for the
pool: `account_release_block` refunds the chain and drops the block's reference, before
PG_USER is cleared, because the ledger reads that flag to tell pool pages from raw ones.

File pages take the static system cache account, on both the single and the cluster
fault paths, so a shared binary's text is not whoever faulted it first. A thread's pool
pages are `CLASS_META`, recorded on the thread with a reference, because the thread's
reaper can run long after the charge. kproc's threads cost nobody, as they cost the pool
nothing.

A Proc is placed under an account only at birth: `rfork_spawn_under` sets `child->account`
before the child is published ([[sub-kernel-proc]]), from a handle named in the spawn's
`SPAWN_EXT_ACCOUNT` tail ([[sub-kernel-syscall-dispatch]]). Login makes the session's account
([[sub-stratum-session]]) and joey the resident services' ([[sub-stratum-boot]]).

## Data structures

`struct MemoryAccount`: incarnation, parent, refs, depth, `used[ACCOUNT_CLASSES]` in
pages, `limit[ACCOUNT_CLASSES]` in bytes with `ACCOUNT_LIMIT_NONE` for unset. Each level's
`used` includes its descendants'. The root and the system cache are
static, with incarnations 1 and 2, and start with a reference nothing drops.

`struct page` keeps its 48 bytes. `holder` is a union of `slab_cache` and `sponsor`, and
the flags say which: PG_SLAB or PG_SPONSORED. A slab comes from `alloc_pages` and is
never sponsored. `struct Proc` gained `account`, and `struct Thread` `pool_sponsor`.

## Concurrency

No lock on a charge. It lands under whatever the allocator's caller holds, `as->lock` in a
fault, and section 6 forbids taking an account lock there. Every field but `refs`, `used`
and `limit` is immutable once the account can be reached, so the walk up needs nothing, and
depth bounds it. The counters are relaxed atomics: nothing decides on them yet. MM-2's
first reservation brings section 6's transaction lock, taken before those locks.

`g_account_tree_lock` guards `limit` alone: `account_set_limits` and `account_query_fill`
take it, a leaf with IRQs masked, and `format_account` reaches it under `g_proc_table_lock`.
Holding it across every class and every ancestor is what orders a limit with an ancestor
lowered at the same moment.

A reader of the ledger and the pool can see them a moment apart, because `free_pages`
returns the two in sequence. The reconcile test retries until the pool holds still.

## Invariants enforced

None yet. [[inv-i32]] is still enforced by the pool and the budgets, and [[inv-i7]]
decides when a page can go; the ledger only follows.

## Error paths

`account_child` answers NULL past `ACCOUNT_MAX_DEPTH`, when the pool refuses or when
memory is short, with no reference kept and no charge left. An allocation that fails charges nothing and keeps no reference. A
refund past zero is counted in `account_unpaired` and goes no further up; an account dying
charged and a block freed at the wrong order are counted there too. None stops the kernel,
because nothing depends on this ledger yet, so the test runner reddens the test during
which one happened.

## Performance

Two relaxed atomics per level per allocation, with a tree at most 16 deep, beside a page
zero. The page struct does not grow. `cpubench faults` measured MM-1a and MM-1b on HVF, medians
of 12 sets: +7.2% one thread, +10.8% threads of one space, +4.1% separate spaces, with a
tree of one level. A session's chain is one level deeper, and that is not measured.

## Prosecution

Round owed; see the row in `docs/AUDIT-TRIGGERS.md`. Questions for it: a block freed at
another order than it was allocated at; `flags` overwritten between alloc and free; a
page reaching `free_pages` after its sponsor died; a kthread working for a Proc charging
the root; and whether `account_unref`'s free is safe under every lock a `free_pages`
caller can hold, which MM-1c makes a production path.

## Seams

`kernel/test/test_mm0_holders.c` still pins today's limits: [[sub-kernel-retention]].
`CLASS_RETAIN` is unused until MM-2.

## Caveats

1. A page table is sponsored by whoever faulted it, which is right: it dies with that
   space. A FILE Burrow's map nodes are the system cache's, through `pm.node_sponsor`.
2. A kthread that allocates user pages for a Proc charges the root, since its Proc is
   kproc. `thread_create_kthread_for` already pays by its owner for the thread itself.
   Not yet inventoried for pages.
3. `kstack_alloc`, `proc_pgtable_create` and slabs are not sponsored. The thread's stack
   is in the ledger through its pool pages; the rest are section 12's metadata rows.

4. A class's `limit` reads its nearest limit, the account's own or an ancestor's, and `headroom`
   reads each class up to that limit, and the pool only for a RAM class that
   met none (vote 65). No account has a limit today, so what the rest of the machine holds
   shows in every query. An account with a limit in each RAM class sees only itself, and then
   its room can exceed what its ancestors or the pool have left. What a diorama Proc reaches
   is still to be measured (vote 59).
5. If an account's creator exits, nobody can change its limits. They stay as they are.
6. `headroom` can read low: `CLASS_ALLOC` counts DMA blocks outside the pool, against a limit
   that is the pool. And high, by caveat 4. MM-2 must not admit on it, and its refusal has to
   read every ancestor and the pool.
7. A DMA create that fails part way releases sponsors through `dma_free_blocks`, with no test
   or injection reaching that unwind. MM-2 routes DMA through the pool, which makes it
   injectable.

## Provenance

MM-1a and MM-1b, operator votes 47 and 48 for the interface they serve. Written while the
mac was queued, on top of an unbooted AS-4. MM-1c, votes 58, 59 and 65.
