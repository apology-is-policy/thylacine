// The durable memory account, docs/SHARED-MEMORY-ACCOUNTING.md sections 3, 4 and 14.
//
// At MM-1 an account only observes. Nothing here refuses an allocation or changes who
// may map what; PROC_SHARED_MAP_MAX_PAGES and the capacity pool still decide, and a limit
// is recorded and reported, not enforced. Charges land under whatever lock the allocator's
// caller holds (as->lock in a fault), so they are atomics and never a lock. Section 6's
// tree lock guards the limits today and takes the first reservation at MM-2, before those
// locks, never under them.
#ifndef THYLACINE_MEMACCOUNT_H
#define THYLACINE_MEMACCOUNT_H

#include <thylacine/account.h>
#include <thylacine/types.h>
#include <thylacine/visibility.h>

// Section 6's bound on one transaction: a root and fifteen generations below it.
#define ACCOUNT_MAX_DEPTH 16u

struct page;

// Everything here but `refs`, `used` and `limit` is immutable once the account is reachable,
// so a charge can walk the ancestors with no lock. The walk is bounded by ACCOUNT_MAX_DEPTH.
struct MemoryAccount {
    u64 incarnation;                // never repeats; a recycled slot gets a new one
    struct MemoryAccount *parent;   // holds a reference; NULL only at the root
    u32 refs;
    u32 depth;                      // the root's is 0
    u64 used[ACCOUNT_CLASSES];      // pages, this account's and every descendant's
    u64 limit[ACCOUNT_CLASSES];     // bytes, ACCOUNT_LIMIT_NONE if unset; under the tree lock
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

// A new account under `parent`, with one reference for the caller, which a KOBJ_ACCOUNT
// handle takes over. It costs `parent` one page of CLASS_META and the pool that page, until
// its last reference goes. NULL when the tree is already ACCOUNT_MAX_DEPTH deep or the pool
// or memory is short.
struct MemoryAccount *account_child(struct MemoryAccount *parent);

void account_ref(struct MemoryAccount *a);
void account_unref(struct MemoryAccount *a);

// Section 4's limit change: the whole vector or nothing, and -1 when a class would exceed
// the ceiling of any ancestor. Lowering below what is in use is allowed and leaves the
// account over its limit, which refuses nothing yet. Rounded down to pages.
int account_set_limits(struct MemoryAccount *a, const u64 limit[ACCOUNT_CLASSES]);

// Adds to or takes from `a` and each ancestor. A parent's total covers its child's, so a
// refund past zero is a broken pairing: it is counted and goes no further up, and cannot
// pass as a smaller number. account_unpaired also counts an account dying while charged
// and a block freed at another order than it has.
void account_charge(struct MemoryAccount *a, unsigned cls, u64 pages);
void account_refund(struct MemoryAccount *a, unsigned cls, u64 pages);
// Section 13's query, in bytes. A charge can land between two figures, so this informs and
// decides nothing: section 4's admission rechecks live state. Takes the tree lock, so the
// limits it reports are one vector.
void account_query_fill(const struct MemoryAccount *a, struct account_query *q);
u64 account_unpaired(void);
// What of the ledger the capacity pool should also read: the two share a unit and
// differ only by the sponsored blocks outside the pool, so this equals
// capacity_pool_charged except while a block is landing or leaving.
u64 account_pool_share(void);

// The ledger's one way on and off a buddy block, keyed on its head page like PG_USER:
// charges the whole block to `a`, holds a reference for as long as the block lives, and
// free_pages hands it back. So an account outlives every Proc that used it until its
// last page has really returned, which is section 4's orphan rule.
void account_sponsor_block(struct page *head, struct MemoryAccount *a);
void account_release_block(struct page *head, unsigned order);

#ifdef KERNEL_TESTS
// account_child with the payer's exemption named, since the suite's own Proc is exempt.
struct MemoryAccount *account_child_as(struct MemoryAccount *parent, bool exempt);
// Tests that break the pairing on purpose put the count back before the runner reads it.
void account_unpaired_rewind_for_test(u64 count);
u64 account_used(const struct MemoryAccount *a, unsigned cls);
#endif

#endif // THYLACINE_MEMACCOUNT_H
