// The MM-1 ledger: docs/SHARED-MEMORY-ACCOUNTING.md sections 3, 4, 7 and 14.
//
// The suite runs alone, so a delta read across one operation is that operation's,
// as in test_mm0_holders.c. Nothing here is enforcement. Those tests still pin today's
// limits, and they must stay green while this ledger only watches.

#include "test.h"

#include "../../mm/phys.h"

#include <thylacine/burrow.h>
#include <thylacine/dma_handle.h>
#include <thylacine/memaccount.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

static u64 alloc_of(const struct MemoryAccount *a) { return account_used(a, CLASS_ALLOC); }
static u64 meta_of(const struct MemoryAccount *a) { return account_used(a, CLASS_META); }

// Only a test rewrites a Proc's account: section 4 keeps it for the Proc's life.
static void adopt(struct Proc *p, struct MemoryAccount *a) {
    account_ref(a);
    account_unref(p->account);
    p->account = a;
}

void test_account_rounded_block_charges_its_whole_order(void) {
    struct MemoryAccount *root = account_root();
    struct MemoryAccount *parent = account_child(root);
    TEST_ASSERT(parent != NULL, "a child of the root");
    struct MemoryAccount *leaf = account_child(parent);
    TEST_ASSERT(leaf != NULL, "a child of that");
    u64 root0 = alloc_of(root);
    u32 pool0 = capacity_pool_charged();

    struct page *p = alloc_user_pages_for(leaf, 3, KP_ZERO, false);
    TEST_ASSERT(p != NULL, "an order 3 block");
    TEST_EXPECT_EQ(alloc_of(leaf), 8u, "the sponsor pays for the whole block");
    TEST_EXPECT_EQ(alloc_of(parent), 8u, "so does its parent");
    TEST_EXPECT_EQ(alloc_of(root), root0 + 8u, "and the root");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 8u, "the pool moves by the same pages");
    TEST_ASSERT(p->flags & PG_SPONSORED, "the head page names its sponsor");
    TEST_ASSERT(p->holder.sponsor == leaf, "and it is the one asked for");

    free_pages(p, 3);
    TEST_EXPECT_EQ(alloc_of(leaf), 0u, "the free refunds the sponsor");
    TEST_EXPECT_EQ(alloc_of(parent), 0u, "and its parent");
    TEST_EXPECT_EQ(alloc_of(root), root0, "and the root");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "and the pool");
    account_unref(leaf);
    account_unref(parent);
}

// Section 4: an orphan stays charged to its ancestors until the last of its backing
// retires. Whichever of the sponsor's owner and its parent's owner lets go first.
void test_account_orphan_stays_charged_until_its_last_page(void) {
    // A base of this test's own, since other work moves the root's references.
    struct MemoryAccount *base = account_child(account_root());
    TEST_ASSERT(base != NULL, "a base");

    struct MemoryAccount *parent = account_child(base);
    TEST_ASSERT(parent != NULL, "a parent");
    struct MemoryAccount *leaf = account_child(parent);
    TEST_ASSERT(leaf != NULL, "a sponsor");
    struct page *p = alloc_user_pages_for(leaf, 1, KP_ZERO, false);
    TEST_ASSERT(p != NULL, "its block");

    // The parent's owner goes first, then the sponsor's: only the block is left.
    account_unref(parent);
    TEST_EXPECT_EQ(alloc_of(parent), 2u, "a parent let go still carries the charge");
    account_unref(leaf);
    TEST_EXPECT_EQ(alloc_of(leaf), 2u, "an orphan keeps its charge");
    TEST_EXPECT_EQ(__atomic_load_n(&leaf->refs, __ATOMIC_RELAXED), 1u, "the block alone holds it");
    TEST_EXPECT_EQ(alloc_of(base), 2u, "the base still sees it");
    TEST_EXPECT_EQ(__atomic_load_n(&base->refs, __ATOMIC_RELAXED), 2u, "and the chain still holds it");

    free_pages(p, 1);
    TEST_EXPECT_EQ(alloc_of(base), 0u, "the last page refunds up the whole chain");
    TEST_EXPECT_EQ(__atomic_load_n(&base->refs, __ATOMIC_RELAXED), 1u, "and the chain let go of it");

    // The other order: the sponsor's owner goes first, the parent's last.
    parent = account_child(base);
    TEST_ASSERT(parent != NULL, "a second parent");
    leaf = account_child(parent);
    TEST_ASSERT(leaf != NULL, "a second sponsor");
    p = alloc_user_pages_for(leaf, 0, KP_ZERO, false);
    TEST_ASSERT(p != NULL, "its page");
    account_unref(leaf);
    TEST_EXPECT_EQ(alloc_of(leaf), 1u, "still charged");
    free_pages(p, 0);
    TEST_EXPECT_EQ(alloc_of(parent), 0u, "refunded through a parent still held");
    TEST_EXPECT_EQ(__atomic_load_n(&parent->refs, __ATOMIC_RELAXED), 1u,
                   "the sponsor died and let go of its parent");
    account_unref(parent);
    TEST_EXPECT_EQ(__atomic_load_n(&base->refs, __ATOMIC_RELAXED), 1u, "back to the start");
    account_unref(base);
}

void test_account_refused_allocation_leaves_the_ledger(void) {
    struct MemoryAccount *a = account_child(account_root());
    TEST_ASSERT(a != NULL, "an account");
    u32 pool0 = capacity_pool_charged();

    struct page *p = alloc_user_pages_for(a, MAX_ORDER + 1u, KP_ZERO, false);
    TEST_ASSERT(p == NULL, "past the largest block");
    TEST_EXPECT_EQ(alloc_of(a), 0u, "nothing charged");
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 1u, "no reference kept");

    // The pool's refusal, one variable from a block that lands.
    u32 room = capacity_pool_pages() - pool0;
    capacity_pool_park_for_test(room);
    p = alloc_user_pages_for(a, 0, KP_ZERO, false);
    TEST_ASSERT(p == NULL, "a full pool refuses");
    TEST_EXPECT_EQ(alloc_of(a), 0u, "and charges nothing");
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 1u, "and keeps no reference");
    capacity_pool_unpark_for_test(room);

    p = alloc_user_pages_for(a, 0, KP_ZERO, false);
    TEST_ASSERT(p != NULL, "the control: the same request with room");
    TEST_EXPECT_EQ(alloc_of(a), 1u, "is charged");
    free_pages(p, 0);
    account_unref(a);
}

void test_account_depth_stops_at_sixteen(void) {
    struct MemoryAccount *chain[ACCOUNT_MAX_DEPTH];
    chain[0] = account_root();
    TEST_EXPECT_EQ(chain[0]->depth, 0u, "the root is the first level");
    for (u32 i = 1; i < ACCOUNT_MAX_DEPTH; i++) {
        chain[i] = account_child(chain[i - 1]);
        TEST_ASSERT(chain[i] != NULL, "every level up to the bound");
        TEST_EXPECT_EQ(chain[i]->depth, i, "records how deep it is");
    }
    u32 last_refs = __atomic_load_n(&chain[ACCOUNT_MAX_DEPTH - 1]->refs, __ATOMIC_RELAXED);
    TEST_ASSERT(account_child(chain[ACCOUNT_MAX_DEPTH - 1]) == NULL, "one more is refused");
    TEST_EXPECT_EQ(__atomic_load_n(&chain[ACCOUNT_MAX_DEPTH - 1]->refs, __ATOMIC_RELAXED), last_refs,
                   "and the refusal keeps no reference");
    for (u32 i = ACCOUNT_MAX_DEPTH - 1; i > 0; i--)
        account_unref(chain[i]);
}

void test_account_incarnation_never_repeats(void) {
    TEST_EXPECT_EQ(account_root()->incarnation, 1u, "the root");
    TEST_EXPECT_EQ(account_system_cache()->incarnation, 2u, "the system cache");
    TEST_ASSERT(account_system_cache()->parent == account_root(), "under the root");

    struct MemoryAccount *a = account_child(account_root());
    TEST_ASSERT(a != NULL, "an account");
    u64 first = a->incarnation;
    TEST_ASSERT(first > 2u, "above the static two");
    account_unref(a);
    struct MemoryAccount *b = account_child(account_root());
    TEST_ASSERT(b != NULL, "a second account");
    TEST_ASSERT(b->incarnation > first, "a later account gets a later incarnation, even in a reused slot");
    account_unref(b);
}

// Refunding past zero must not pass as a smaller number.
void test_account_refund_past_zero_is_counted(void) {
    struct MemoryAccount *a = account_child(account_root());
    TEST_ASSERT(a != NULL, "an account");
    u64 before = account_underflows();
    u64 root0 = alloc_of(account_root());
    account_charge(a, CLASS_ALLOC, 2);
    account_refund(a, CLASS_ALLOC, 3);
    TEST_EXPECT_EQ(account_underflows() - before, 1u, "counted once");
    TEST_EXPECT_EQ(alloc_of(a), 2u, "with the account left as it was");
    TEST_EXPECT_EQ(alloc_of(account_root()), root0 + 2u, "and nothing taken from its parent");
    account_refund(a, CLASS_ALLOC, 2);
    TEST_EXPECT_EQ(alloc_of(a), 0u, "the right refund still lands");
    account_unref(a);
    account_underflows_rewind_for_test(before);
}

// The grandchild runs under the account its parent adopted, so a default of "the root"
// cannot pass for inheritance.
static struct MemoryAccount *volatile g_fk_account;
static struct MemoryAccount *volatile g_fk_seen;
static volatile u64 g_fk_grew, g_fk_back, g_fk_meta;
static volatile int g_fk_status;

static void fk_grandchild(void *arg) {
    (void)arg;
    struct MemoryAccount *a = current_thread()->proc->account;
    g_fk_seen = a;
    g_fk_meta = meta_of(a);
    u64 before = alloc_of(a);
    struct Burrow *b = burrow_create_anon(4 * PAGE_SIZE, false);
    g_fk_grew = b ? alloc_of(a) - before : ~0ull;
    if (b) burrow_unref(b);
    g_fk_back = alloc_of(a) - before;
    exits("ok");
}

static void fk_child(void *arg) {
    (void)arg;
    adopt(current_thread()->proc, g_fk_account);
    int pid = rfork(RFPROC, fk_grandchild, NULL);
    if (pid <= 0) exits("rfork");
    int status = -1;
    if (wait_pid(&status) != pid) exits("wait");
    g_fk_status = status;
    exits("ok");
}

void test_account_fork_keeps_the_account(void) {
    struct MemoryAccount *a = account_child(account_root());
    TEST_ASSERT(a != NULL, "an account");
    g_fk_account = a;
    g_fk_seen = NULL;
    g_fk_grew = g_fk_back = g_fk_meta = ~0ull;
    g_fk_status = -1;

    int pid = rfork(RFPROC, fk_child, NULL);
    TEST_ASSERT(pid > 0, "rfork");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), pid, "wait_pid");
    TEST_EXPECT_EQ(status, 0, "the child exited cleanly");
    TEST_EXPECT_EQ(g_fk_status, 0, "and so did the grandchild");

    TEST_ASSERT(g_fk_seen == a, "a fork inherits the account");
    TEST_EXPECT_EQ(g_fk_grew, 4u, "what the grandchild allocates lands on it");
    TEST_EXPECT_EQ(g_fk_back, 0u, "what it frees comes back");
    TEST_EXPECT_EQ(g_fk_meta, (u64)THREAD_POOL_PAGES, "its thread is charged to it too");

    TEST_EXPECT_EQ(meta_of(a), 0u, "the reaper refunded the thread");
    TEST_EXPECT_EQ(alloc_of(a), 0u, "every page came back with the Procs");
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 1u, "no Proc still holds it");
    account_unref(a);
}

// Section 7: what was taken, not the logical length, and outside the pool.
void test_account_dma_charges_whole_blocks_outside_the_pool(void) {
    struct MemoryAccount *root = account_root();
    u64 root0 = alloc_of(root);
    u32 pool0 = capacity_pool_charged();
    struct KObj_DMA *k = kobj_dma_create(3 * PAGE_SIZE);
    TEST_ASSERT(k != NULL, "three pages of DMA");
    TEST_EXPECT_EQ(k->blk[0].order, 2u, "rounded up to a block of four");
    TEST_EXPECT_EQ(alloc_of(root), root0 + 4u, "the ledger pays for four");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "and the pool still sees none");
    TEST_ASSERT(k->blk[0].pages->holder.sponsor == root, "the caller's account sponsors it");
    kobj_dma_unref(k);
    TEST_EXPECT_EQ(alloc_of(root), root0, "the free refunds the four");
}

static void never_entry(void *arg) { (void)arg; }

void test_account_thread_charges_meta_to_its_payer(void) {
    struct MemoryAccount *a = account_child(account_root());
    TEST_ASSERT(a != NULL, "an account");
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "a Proc");
    adopt(p, a);
    u64 root0 = meta_of(account_root());
    struct Thread *t = thread_create_with_arg(p, never_entry, NULL);
    TEST_ASSERT(t != NULL, "a thread");
    TEST_EXPECT_EQ(meta_of(a), (u64)THREAD_POOL_PAGES, "its kernel memory is the payer's");
    TEST_EXPECT_EQ(meta_of(account_root()), root0 + THREAD_POOL_PAGES, "and the root's");
    TEST_ASSERT(t->pool_sponsor == a, "the thread keeps who paid");
    thread_free(t);
    TEST_EXPECT_EQ(meta_of(a), 0u, "the free refunds it");

    // kproc's threads are the reserve's and cost nobody.
    struct Thread *k = thread_create_with_arg(kproc(), never_entry, NULL);
    TEST_ASSERT(k != NULL, "a kernel thread");
    TEST_EXPECT_EQ(meta_of(account_root()), root0, "the root pays nothing for it");
    TEST_ASSERT(k->pool_sponsor == NULL, "and no one is recorded");
    thread_free(k);

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 1u, "the Proc let go");
    account_unref(a);
}

// A reaper on another CPU returns the pool and the ledger a moment apart, so a reading
// counts only when the pool held still across it.
static bool ledger_settled(u64 *ledger, u32 *pool) {
    for (u32 tries = 0; tries < 1000u; tries++) {
        *pool = capacity_pool_charged();
        *ledger = account_pool_share();
        if (*ledger == *pool && capacity_pool_charged() == *pool) return true;
        sched();
    }
    return false;
}

// Whole-boot, so every allocation since the first is in it: a path that charges the
// pool and not the ledger, or a block freed at the wrong order, lands here.
void test_account_ledger_reconciles_with_the_pool(void) {
    u64 ledger = 0;
    u32 pool = 0;
    bool settled = ledger_settled(&ledger, &pool);
    TEST_EXPECT_EQ(ledger, (u64)pool, "the ledger's share is the pool, to the page");
    TEST_ASSERT(settled, "and they held still");
    TEST_ASSERT(pool != 0u, "the control: a boot has charged the pool by now");
    TEST_EXPECT_EQ(account_underflows(), 0u, "and no refund has ever gone past zero");

    struct Burrow *b = burrow_create_anon(4 * PAGE_SIZE, false);
    TEST_ASSERT(b != NULL, "pool pages");
    struct KObj_DMA *k = kobj_dma_create(3 * PAGE_SIZE);
    TEST_ASSERT(k != NULL, "raw pages");
    settled = ledger_settled(&ledger, &pool);
    TEST_EXPECT_EQ(ledger, (u64)pool, "the share follows pool pages and ignores raw ones");
    TEST_ASSERT(settled, "and they held still");
    kobj_dma_unref(k);
    burrow_unref(b);
    settled = ledger_settled(&ledger, &pool);
    TEST_EXPECT_EQ(ledger, (u64)pool, "and still agrees after both have gone");
    TEST_ASSERT(settled, "and they held still");
}
