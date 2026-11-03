// The durable memory account, docs/SHARED-MEMORY-ACCOUNTING.md sections 3, 4 and 14.
//
// At MM-1 an account only observes. Nothing here refuses an allocation or changes who
// may map what; PROC_SHARED_MAP_MAX_PAGES and the capacity pool still decide. Charges
// land under whatever lock the allocator's caller holds (as->lock in a fault), so they
// are atomics and never a lock. Section 6's transaction lock arrives with the first
// reservation, at MM-2, which takes it before those locks.
#ifndef THYLACINE_MEMACCOUNT_H
#define THYLACINE_MEMACCOUNT_H

#include <thylacine/account.h>
#include <thylacine/types.h>

// Section 6's bound on one transaction: a root and fifteen generations below it.
#define ACCOUNT_MAX_DEPTH 16u

struct page;

// Everything here but `refs` and `used` is immutable once the account is reachable, so
// a charge can walk the ancestors with no lock. The walk is bounded by ACCOUNT_MAX_DEPTH.
struct MemoryAccount {
    u64 incarnation;                // never repeats; a recycled slot gets a new one
    struct MemoryAccount *parent;   // holds a reference; NULL only at the root
    u32 refs;
    u32 depth;                      // the root's is 0
    u64 used[ACCOUNT_CLASSES];      // pages, this account's and every descendant's
};

// The boot account, from which every other descends. Static, so a sponsor is never
// NULL however early an allocation runs, and it is never freed.
struct MemoryAccount *account_root(void);
// Section 3's bounded system cache account: shared file backing, which belongs to no
// one who happened to fault it first.
struct MemoryAccount *account_system_cache(void);
// The current thread's Proc's account, or the root before there is a thread.
struct MemoryAccount *account_current(void);

void account_init(void);            // after slub_init, before the first child

// A new account under `parent`, with one reference for the caller. NULL when the tree
// is already ACCOUNT_MAX_DEPTH deep or memory is short.
struct MemoryAccount *account_child(struct MemoryAccount *parent);
void account_ref(struct MemoryAccount *a);
void account_unref(struct MemoryAccount *a);

// Adds to or takes from `a` and each ancestor. A parent's total covers its child's, so a
// refund past zero is a broken pairing: it is counted and goes no further up, and cannot
// pass as a smaller number. account_underflows also counts an account dying while charged.
void account_charge(struct MemoryAccount *a, unsigned cls, u64 pages);
void account_refund(struct MemoryAccount *a, unsigned cls, u64 pages);
u64 account_used(const struct MemoryAccount *a, unsigned cls);
u64 account_underflows(void);
// What of the ledger the capacity pool should also read: the two share a unit and
// differ only by the sponsored blocks outside the pool, so this equals
// capacity_pool_charged whenever nothing is being freed.
u64 account_pool_share(void);

// The ledger's one way on and off a buddy block, keyed on its head page like PG_USER:
// charges 1 << order to `a`, holds a reference for as long as the block lives, and
// free_pages hands it back. So an account outlives every Proc that used it until its
// last page has really returned, which is section 4's orphan rule.
void account_sponsor_block(struct page *head, struct MemoryAccount *a, unsigned order);
void account_release_block(struct page *head, unsigned order);

#ifdef KERNEL_TESTS
// Tests that break the pairing on purpose put the count back before the runner reads it.
void account_underflows_rewind_for_test(u64 count);
#endif

#endif // THYLACINE_MEMACCOUNT_H
