// The MM-1 ledger: docs/SHARED-MEMORY-ACCOUNTING.md sections 3, 4, 7 and 14.
//
// The suite runs alone, so a delta read across one operation is that operation's,
// as in test_mm0_holders.c. Nothing here is enforcement. Those tests still pin today's
// limits, and they must stay green while this ledger only watches.

#include "test.h"

#include "../../arch/arm64/exception.h"
#include "../../mm/phys.h"

#include <thylacine/burrow.h>
#include <thylacine/dma_handle.h>
#include <thylacine/handle.h>
#include <thylacine/image.h>
#include <thylacine/memaccount.h>
#include <thylacine/page.h>
#include <thylacine/pagemap.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/syscall.h>
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
    // A full pool reclaims idle images before it refuses, which would move this point.
    (void)image_cache_evict_idle_for_test();
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
    u64 before = account_unpaired();
    u64 root0 = alloc_of(account_root());
    account_charge(a, CLASS_ALLOC, 2);
    account_refund(a, CLASS_ALLOC, 3);
    TEST_EXPECT_EQ(account_unpaired() - before, 1u, "counted once");
    TEST_EXPECT_EQ(alloc_of(a), 2u, "with the account left as it was");
    TEST_EXPECT_EQ(alloc_of(account_root()), root0 + 2u, "and nothing taken from its parent");
    account_refund(a, CLASS_ALLOC, 2);
    TEST_EXPECT_EQ(alloc_of(a), 0u, "the right refund still lands");
    account_unref(a);
    account_unpaired_rewind_for_test(before);
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

// A path that charged one side and not the other stays in the difference, as does a block
// freed at the wrong order, since the ledger refunds the block's own and the pool the
// caller's. A charge and return that both happened cancel here, and the next test looks
// at what is left.
void test_account_ledger_reconciles_with_the_pool(void) {
    u64 ledger = 0;
    u32 pool = 0;
    bool settled = ledger_settled(&ledger, &pool);
    TEST_EXPECT_EQ(ledger, (u64)pool, "the ledger's share is the pool, to the page");
    TEST_ASSERT(settled, "and they held still");
    TEST_ASSERT(pool != 0u, "the control: a boot has charged the pool by now");
    TEST_EXPECT_EQ(account_unpaired(), 0u, "and no refund has ever gone past zero");

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

// Counters agreeing with each other prove less than counters agreeing with the blocks.
// Section 10's reconciliation: every sponsored block, rounded, is ALLOC and nothing else is;
// every PG_USER block and every thread's pool pages are the pool and nothing else is.
void test_account_blocks_that_exist_are_the_ledger(void) {
    struct Burrow *b = burrow_create_anon(4 * PAGE_SIZE, false);
    struct KObj_DMA *k = kobj_dma_create(3 * PAGE_SIZE);
    u64 alloc = 0, meta = 0, sponsored = 0, pooled = 0;
    u32 charged = 0;
    bool steady = false;
    for (u32 tries = 0; b && k && !steady && tries < 64u; tries++) {
        alloc = alloc_of(account_root());
        meta = meta_of(account_root());
        charged = capacity_pool_charged();
        sponsored = phys_block_pages_for_test(PG_SPONSORED);
        pooled = phys_block_pages_for_test(PG_USER);
        steady = alloc == alloc_of(account_root()) && meta == meta_of(account_root()) &&
                 charged == capacity_pool_charged();
        if (!steady) sched();
    }
    if (k) kobj_dma_unref(k);
    if (b) burrow_unref(b);
    TEST_ASSERT(b && k, "a rounded pool block and a rounded raw one");
    TEST_ASSERT(steady, "the counters held still across a walk");
    TEST_EXPECT_EQ(sponsored, alloc, "ALLOC is the sponsored blocks, to the page");
    TEST_EXPECT_EQ(pooled + meta, (u64)charged, "the pool is the PG_USER blocks and thread pages");
    TEST_ASSERT(sponsored >= pooled + 4u, "control: a raw block is sponsored and outside the pool");
}

// What free_pages(p, 0) would do to an order-2 block, stopped before the pool and the
// buddy see it.
void test_account_wrong_order_release_is_counted(void) {
    struct MemoryAccount *a = account_child(account_root());
    TEST_ASSERT(a != NULL, "an account");
    u64 before = account_unpaired();
    struct page *p = alloc_user_pages_for(a, 2, 0, false);
    TEST_ASSERT(p != NULL, "an order-2 block");
    u64 charged = alloc_of(a);
    account_release_block(p, 0);
    u64 counted = account_unpaired() - before;
    u64 left = alloc_of(a);
    account_unpaired_rewind_for_test(before);
    free_pages(p, 2);
    account_unref(a);
    TEST_EXPECT_EQ(charged, 4u, "the control: four pages charged");
    TEST_EXPECT_EQ(counted, 1u, "the wrong order is counted");
    TEST_EXPECT_EQ(left, 0u, "and what was charged is what comes back");
}

// Growth allocates nodes as a fault does: the map's sponsor pays when it has one, as a file
// map does, and whoever is growing it otherwise.
void test_account_grown_node_has_the_maps_sponsor(void) {
    struct MemoryAccount *a = account_child(account_root());
    TEST_ASSERT(a != NULL, "an account");
    spin_lock_t lk = SPIN_LOCK_INIT;
    struct page *pg = alloc_user_pages(0, KP_ZERO, true);
    TEST_ASSERT(pg != NULL, "a page");
    struct pagemap pm;
    TEST_EXPECT_EQ(pagemap_init(&pm, 4), 0, "an inline map");
    pm.node_sponsor = a;
    TEST_EXPECT_EQ(pagemap_install(&pm, &lk, 0, pg, NULL, true, NULL), 0, "a resident page");
    TEST_EXPECT_EQ(pagemap_grow(&pm, &lk, 40, NULL, true), 0, "inline to radix");
    TEST_EXPECT_EQ(pm.nodes, 1u, "one node");
    u64 charged = alloc_of(a);
    bool named = pa_to_page(kva_to_pa(pm.root))->holder.sponsor == a;

    struct page *taken = NULL;
    struct page *fr[PAGEMAP_MAX_DEPTH];
    u32 nf = 0;
    (void)pagemap_take_next(&pm, &lk, 0, 40, NULL, &taken, fr, &nf);
    for (u32 k = 0; k < nf; k++) free_pages(fr[k], 0);
    if (taken) free_pages(taken, 0);
    pagemap_destroy(&pm, NULL, NULL);
    TEST_EXPECT_EQ(alloc_of(a), 0u, "and it comes back with the node");
    account_unref(a);
    TEST_EXPECT_EQ(charged, 1u, "the map's sponsor pays for the node");
    TEST_ASSERT(named, "and the node's head page names it");
}

void test_account_query_reports_bytes_up_the_chain(void) {
    struct MemoryAccount *parent = account_child(account_root());
    struct MemoryAccount *child = parent ? account_child(parent) : NULL;
    struct MemoryAccount *sibling = parent ? account_child(parent) : NULL;
    TEST_ASSERT(parent && child && sibling, "three accounts");

    const u64 pool_pages = capacity_pool_pages();
    const u64 pool = pool_pages << PAGE_SHIFT;
    const u64 five_pages = 5ull << PAGE_SHIFT;

    // Each reading is taken with its charge on and judged after all of them are off, so a
    // red assert leaves the root as it found it.
    struct account_query plain, by_alloc, by_meta, by_pool;

    account_charge(child, CLASS_ALLOC, 3);
    account_charge(child, CLASS_META, 2);
    account_query_fill(child, &plain);
    account_refund(child, CLASS_META, 2);
    account_refund(child, CLASS_ALLOC, 3);

    // A sibling's use reaches the child only through their ancestors, so these need the
    // walk: the root's class comes to five pages short of the pool, and the pool itself
    // has more room than that because the charge never touched it.
    u64 fake = pool_pages - alloc_of(account_root()) - 5u;
    account_charge(sibling, CLASS_ALLOC, fake);
    account_query_fill(child, &by_alloc);
    account_refund(sibling, CLASS_ALLOC, fake);

    fake = pool_pages - meta_of(account_root()) - 5u;
    account_charge(sibling, CLASS_META, fake);
    account_query_fill(child, &by_meta);
    account_refund(sibling, CLASS_META, fake);

    // The pool's own room is the other bound. A park reaches the pool and not the ledger,
    // and the runner releases it should anything below go wrong.
    u64 ledger = alloc_of(account_root());
    u64 charged = capacity_pool_charged();
    u32 extra = (u32)(ledger > charged ? ledger - charged : 0u) + 16u;
    capacity_pool_park_for_test(extra);
    u64 pool_room = (pool_pages - capacity_pool_charged()) << PAGE_SHIFT;
    account_query_fill(child, &by_pool);
    capacity_pool_unpark_for_test(extra);

    account_unref(sibling);
    account_unref(child);
    account_unref(parent);

    TEST_EXPECT_EQ(plain.h.version, ACCOUNT_VERSION, "the record names its version");
    TEST_EXPECT_EQ(plain.h.size, (u32)sizeof(plain), "and its size");
    TEST_ASSERT(plain.h.incarnation > 2u, "and an account that is neither static one");
    TEST_EXPECT_EQ(plain.c[CLASS_ALLOC].used, 3ull << PAGE_SHIFT, "used crosses in bytes");
    TEST_EXPECT_EQ(plain.c[CLASS_META].used, 2ull << PAGE_SHIFT, "per class");
    TEST_EXPECT_EQ(plain.c[CLASS_RETAIN].used, 0ull, "nothing charges RETAIN yet");
    TEST_EXPECT_EQ(plain.c[CLASS_DEVICE].used, 0ull, "nothing charges DEVICE yet");
    TEST_EXPECT_EQ(plain.c[CLASS_ALLOC].limit, pool, "a RAM class may reach the pool");
    TEST_EXPECT_EQ(plain.c[CLASS_RETAIN].limit, pool, "every RAM class");
    TEST_EXPECT_EQ(plain.c[CLASS_META].limit, pool, "every RAM class");
    TEST_EXPECT_EQ(plain.c[CLASS_DEVICE].limit, ~0ull, "nothing bounds device storage here");
    TEST_EXPECT_EQ(plain.level, (u32)ACCOUNT_NORMAL, "no pressure machinery yet");
    TEST_EXPECT_EQ(plain.generation, 0ull, "so no generation either");

    TEST_EXPECT_EQ(by_alloc.headroom, five_pages, "the tightest ancestor bounds the child");
    TEST_EXPECT_EQ(by_alloc.limiting, (u32)CLASS_ALLOC, "and the class it is tight in is named");
    TEST_EXPECT_EQ(by_meta.headroom, five_pages, "META's room bounds it now");
    TEST_EXPECT_EQ(by_meta.limiting, (u32)CLASS_META, "and META is named");
    TEST_EXPECT_EQ(by_pool.headroom, pool_room, "the pool's room bounds every account");
}

static s64 account_call(u64 handle, u64 op, u64 va, u64 size) {
    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[0] = handle;
    ctx.regs[1] = op;
    ctx.regs[2] = va;
    ctx.regs[3] = size;
    ctx.regs[8] = SYS_ACCOUNT;
    TEST_SYSCALL_DISPATCH(&ctx);
    return (s64)ctx.regs[0];
}

// Section 13: each account pays META, here a page on its parent's chain and in the pool, for
// as long as anything holds it.
void test_account_child_costs_its_parent_meta(void) {
    struct MemoryAccount *parent = account_child(account_root());
    TEST_ASSERT(parent != NULL, "a parent");
    u64 root0 = meta_of(account_root());
    u32 pool0 = capacity_pool_charged();

    struct MemoryAccount *c = account_child(parent);
    TEST_ASSERT(c != NULL, "a child");
    TEST_EXPECT_EQ(meta_of(parent), 1u, "the child costs its parent a page");
    TEST_EXPECT_EQ(meta_of(account_root()), root0 + 1u, "and the chain above it");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 1u, "and the pool that page");
    TEST_EXPECT_EQ(meta_of(c), 0u, "and itself nothing");

    struct MemoryAccount *g = account_child(c);
    TEST_ASSERT(g != NULL, "a grandchild");
    TEST_EXPECT_EQ(meta_of(c), 1u, "its parent pays");
    TEST_EXPECT_EQ(meta_of(parent), 2u, "and the grandparent's total covers it");
    account_unref(g);
    TEST_EXPECT_EQ(meta_of(c), 0u, "its last reference refunds its parent");
    TEST_EXPECT_EQ(meta_of(parent), 1u, "up the chain");
    account_unref(c);
    TEST_EXPECT_EQ(meta_of(parent), 0u, "the child's last reference too");
    TEST_EXPECT_EQ(meta_of(account_root()), root0, "the root is back");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "and so is the pool");

    // The bound on breadth is the pool's, so a full pool refuses and charges nothing.
    (void)image_cache_evict_idle_for_test();
    u32 room = capacity_pool_pages() - capacity_pool_charged();
    capacity_pool_park_for_test(room);
    struct MemoryAccount *refused = account_child_as(parent, false);
    u64 meta_refused = meta_of(parent);
    struct MemoryAccount *exempt = account_child_as(parent, true);
    u64 meta_exempt = meta_of(parent);
    capacity_pool_unpark_for_test(room);
    if (exempt) account_unref(exempt);
    TEST_ASSERT(refused == NULL, "a full pool refuses an ordinary payer");
    TEST_EXPECT_EQ(meta_refused, 0u, "and charges nothing");
    TEST_ASSERT(exempt != NULL, "control: the TCB is counted, never refused");
    TEST_EXPECT_EQ(meta_exempt, 1u, "so it does land");
    TEST_EXPECT_EQ(meta_of(parent), 0u, "and comes back");
    account_unref(parent);
}

static void limits_read(const struct MemoryAccount *a, u64 out[ACCOUNT_CLASSES]) {
    struct account_query q;
    account_query_fill(a, &q);
    for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++) out[cls] = q.c[cls].limit;
}

static void limits_clear(u64 lim[ACCOUNT_CLASSES]) {
    for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++) lim[cls] = ACCOUNT_LIMIT_NONE;
}

// Section 4: vector-atomic, and never above an ancestor.
void test_account_limits_change_whole_or_not_at_all(void) {
    const u64 pool = (u64)capacity_pool_pages() << PAGE_SHIFT;
    const u64 page = PAGE_SIZE;
    struct MemoryAccount *parent = account_child(account_root());
    struct MemoryAccount *child = parent ? account_child(parent) : NULL;
    TEST_ASSERT(parent && child, "two accounts");
    u64 lim[ACCOUNT_CLASSES], got[ACCOUNT_CLASSES];

    limits_read(child, got);
    TEST_EXPECT_EQ(got[CLASS_ALLOC], pool, "an unset RAM class reads as the pool");
    TEST_EXPECT_EQ(got[CLASS_DEVICE], ~0ull, "and an unset device class as none");

    limits_clear(lim);
    lim[CLASS_ALLOC] = 16u * page;
    TEST_EXPECT_EQ(account_set_limits(parent, lim), 0, "a parent's ceiling");

    // A good class beside a bad one: the first in the vector would land if this were two
    // writes, so the bad one is the later class.
    limits_clear(lim);
    lim[CLASS_RETAIN] = 8u * page;
    lim[CLASS_META] = pool + page;
    TEST_EXPECT_EQ(account_set_limits(child, lim), -1, "above the pool, which is the root's ceiling");
    limits_read(child, got);
    TEST_EXPECT_EQ(got[CLASS_RETAIN], pool, "and the good class beside it did not change");
    limits_clear(lim);
    lim[CLASS_RETAIN] = 8u * page;
    lim[CLASS_ALLOC] = 32u * page;
    TEST_EXPECT_EQ(account_set_limits(child, lim), -1, "above the parent's ceiling");
    limits_read(child, got);
    TEST_EXPECT_EQ(got[CLASS_RETAIN], pool, "nor beside this one");
    TEST_EXPECT_EQ(got[CLASS_ALLOC], pool, "nor the bad one");

    lim[CLASS_ALLOC] = 16u * page;
    TEST_EXPECT_EQ(account_set_limits(child, lim), 0, "control: at the ceiling is within it");
    limits_read(child, got);
    TEST_EXPECT_EQ(got[CLASS_ALLOC], 16u * page, "the class lands");
    TEST_EXPECT_EQ(got[CLASS_RETAIN], 8u * page, "with the one beside it");
    TEST_EXPECT_EQ(got[CLASS_META], pool, "and an unset one stays the default");

    // Bytes round down to whole pages: a ceiling that rounds up would pass its own check.
    lim[CLASS_ALLOC] = 2u * page + 1u;
    TEST_EXPECT_EQ(account_set_limits(child, lim), 0, "not a whole number of pages");
    limits_read(child, got);
    TEST_EXPECT_EQ(got[CLASS_ALLOC], 2u * page, "rounds down");

    // Lowering an ancestor leaves a descendant above it, and raising that descendant is
    // then refused, at the ceiling that is left.
    limits_clear(lim);
    lim[CLASS_ALLOC] = page;
    TEST_EXPECT_EQ(account_set_limits(parent, lim), 0, "a parent lowered past its child");
    limits_read(child, got);
    TEST_EXPECT_EQ(got[CLASS_ALLOC], 2u * page, "the child keeps its own figure");
    limits_clear(lim);
    lim[CLASS_ALLOC] = 3u * page;
    TEST_EXPECT_EQ(account_set_limits(child, lim), -1, "and cannot be raised past the parent");

    // Every level is asked, not only the nearest: the grandparent's is the tight one here.
    limits_clear(lim);
    TEST_EXPECT_EQ(account_set_limits(parent, lim), 0, "the parent's ceiling comes off");
    struct MemoryAccount *leaf = account_child(child);
    TEST_ASSERT(leaf != NULL, "a third level");
    lim[CLASS_ALLOC] = 3u * page;
    TEST_EXPECT_EQ(account_set_limits(child, lim), 0, "a ceiling one level up");
    limits_clear(lim);
    lim[CLASS_ALLOC] = 2u * page;
    TEST_EXPECT_EQ(account_set_limits(parent, lim), 0, "a tighter one two levels up");
    limits_clear(lim);
    lim[CLASS_ALLOC] = 3u * page;
    TEST_EXPECT_EQ(account_set_limits(leaf, lim), -1, "the leaf answers to the one two up");
    account_unref(leaf);

    // ACCOUNT_LIMIT_NONE takes the account's own ceiling off, whatever is above it.
    limits_clear(lim);
    TEST_EXPECT_EQ(account_set_limits(child, lim), 0, "none is always allowed");
    limits_read(child, got);
    TEST_EXPECT_EQ(got[CLASS_ALLOC], pool, "and reads as the default again");
    account_unref(child);
    account_unref(parent);
}

void test_account_limit_bounds_the_room(void) {
    const u64 page = PAGE_SIZE;
    struct MemoryAccount *parent = account_child(account_root());
    struct MemoryAccount *child = parent ? account_child(parent) : NULL;
    struct MemoryAccount *sibling = parent ? account_child(parent) : NULL;
    TEST_ASSERT(parent && child && sibling, "three accounts");
    u64 lim[ACCOUNT_CLASSES];
    struct account_query own, above, tighter, over, other;

    // Each reading is taken with its charge on and judged after all of them are off.
    limits_clear(lim);
    lim[CLASS_ALLOC] = 8u * page;
    int rc_child = account_set_limits(child, lim);
    account_charge(child, CLASS_ALLOC, 3);
    account_query_fill(child, &own);

    limits_clear(lim);
    lim[CLASS_ALLOC] = 6u * page;
    int rc_parent = account_set_limits(parent, lim);
    account_query_fill(child, &above);
    account_charge(sibling, CLASS_ALLOC, 2);
    account_query_fill(child, &tighter);

    // Below what is in use: allowed, it takes nothing back, and the room stops at zero.
    limits_clear(lim);
    lim[CLASS_ALLOC] = 2u * page;
    int rc_over = account_set_limits(parent, lim);
    account_query_fill(child, &over);

    // Another class is the tight one now.
    limits_clear(lim);
    int rc_off = account_set_limits(parent, lim);
    lim[CLASS_META] = page;
    int rc_meta = account_set_limits(child, lim);
    account_query_fill(child, &other);

    account_refund(sibling, CLASS_ALLOC, 2);
    account_refund(child, CLASS_ALLOC, 3);
    account_unref(sibling);
    account_unref(child);
    account_unref(parent);

    TEST_EXPECT_EQ(rc_child, 0, "the child's ceiling");
    TEST_EXPECT_EQ(rc_parent, 0, "the parent's");
    TEST_EXPECT_EQ(rc_over, 0, "lowering below use is allowed");
    TEST_EXPECT_EQ(rc_off, 0, "off again");
    TEST_EXPECT_EQ(rc_meta, 0, "and META's");
    TEST_EXPECT_EQ(own.headroom, 5u * page, "an account's own ceiling bounds its room");
    TEST_EXPECT_EQ(own.limiting, (u32)CLASS_ALLOC, "and names the class");
    TEST_EXPECT_EQ(above.headroom, 3u * page, "a parent's ceiling bounds it too");
    TEST_EXPECT_EQ(tighter.headroom, page, "and a sibling's use reaches it through the parent");
    TEST_EXPECT_EQ(over.headroom, 0ull, "over its limit the room is none");
    TEST_EXPECT_EQ(over.c[CLASS_ALLOC].used, 3u * page, "and what is in use stays");
    TEST_EXPECT_EQ(other.headroom, page, "META's ceiling is the tightest room");
    TEST_EXPECT_EQ(other.limiting, (u32)CLASS_META, "and is named");
}

// A control handle holds one reference, has no dup, closes on exec and at the Proc's end.
void test_account_handle_is_the_control(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "a Proc");
    struct MemoryAccount *a = account_child(account_root());
    TEST_ASSERT(a != NULL, "an account");
    account_ref(a);     // this test's, so the count can be read once the handle has gone
    hidx_t h = handle_alloc_cloexec(p, KOBJ_ACCOUNT, RIGHT_READ | RIGHT_WRITE, a, true);
    TEST_ASSERT(h >= 0, "a handle takes over the creator's reference");
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 2u, "the handle and this test");

    struct Handle hh;
    TEST_EXPECT_EQ(handle_get(p, h, &hh), 0, "a lookup");
    TEST_EXPECT_EQ((int)hh.kind, (int)KOBJ_ACCOUNT, "finds the kind");
    TEST_ASSERT(hh.obj == a, "and the account");
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 3u, "and holds it for the borrow");
    handle_put(&hh);
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 2u, "the put gives that back");

    TEST_EXPECT_EQ((int)handle_dup(p, h, RIGHT_READ), -1, "the kind has no dup");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, h, 0, false), -T_E_ACCES, "nor a lowest-free one");
    TEST_EXPECT_EQ(handle_dup_to_checked(p, h, 40, false), -T_E_ACCES, "nor one onto a chosen slot");
    hidx_t other = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ, NULL);
    TEST_ASSERT(other >= 0, "a transferable kind beside it");
    TEST_EXPECT_EQ(handle_dup_to_checked(p, other, 40, false), 40, "control: that kind dups onto the slot");
    TEST_ASSERT(handle_dup_posix_checked(p, other, 0, false) >= 0, "control: and to the lowest free");
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 2u, "and a refused one holds nothing");

    TEST_EXPECT_EQ(handle_get_cloexec(p, h), 1, "made close-on-exec");
    TEST_EXPECT_EQ(handle_close_on_exec(p), 1, "so an exec closes it");
    TEST_EXPECT_EQ(handle_get(p, h, &hh), -1, "it is gone");
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 1u, "and let go of the account");

    // Userspace clearing the flag keeps nothing past an exec.
    account_ref(a);
    h = handle_alloc_cloexec(p, KOBJ_ACCOUNT, RIGHT_READ | RIGHT_WRITE, a, false);
    TEST_ASSERT(h >= 0, "a handle with the flag clear");
    TEST_EXPECT_EQ(handle_get_cloexec(p, h), 0, "reads clear");
    TEST_EXPECT_EQ(handle_close_on_exec(p), 1, "an exec closes it anyway");
    TEST_EXPECT_EQ(handle_get(p, h, &hh), -1, "it is gone too");
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 1u, "and let go of the account too");

    // The end of the Proc: the last handle it holds lets go the same way.
    account_ref(a);
    h = handle_alloc_cloexec(p, KOBJ_ACCOUNT, RIGHT_READ | RIGHT_WRITE, a, false);
    TEST_ASSERT(h >= 0, "a second handle");
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
    TEST_EXPECT_EQ(__atomic_load_n(&a->refs, __ATOMIC_RELAXED), 1u, "a Proc's end closes it");
    account_unref(a);
}

// Section 13: a fork's table copy does not take the kind, so control cannot leave the Proc
// that holds it. handle_table_copy_into is that copy; an entry-shape rfork makes none.
void test_account_fork_does_not_copy_the_handle(void) {
    struct Proc *parent = proc_alloc();
    struct Proc *child = proc_alloc();
    struct MemoryAccount *a = account_child(account_root());
    hidx_t acct = -1, control = -1;
    int copied = -1, got_account = -99, got_control = -99;
    if (parent && child && a) {
        acct = handle_alloc_cloexec(parent, KOBJ_ACCOUNT, RIGHT_READ | RIGHT_WRITE, a, false);
        // A transferable kind in the same table, so a copy that copies nothing cannot pass.
        control = handle_alloc(parent, KOBJ_PROCESS, RIGHT_READ, NULL);
        copied = handle_table_copy_into(child, parent);
        struct Handle hh;
        got_control = handle_get(child, control, &hh);
        if (got_control == 0) handle_put(&hh);
        got_account = handle_get(child, acct, &hh);
        if (got_account == 0) handle_put(&hh);
    }
    if (child) { child->state = PROC_STATE_ZOMBIE; proc_free(child); }
    if (parent) { parent->state = PROC_STATE_ZOMBIE; proc_free(parent); }
    if (a && acct < 0) account_unref(a);
    TEST_ASSERT(parent && child && a, "the fixture");
    TEST_ASSERT(acct >= 0 && control >= 0, "both handles");
    TEST_EXPECT_EQ(copied, 1, "one slot copied");
    TEST_EXPECT_EQ(got_control, 0, "control: a transferable handle is copied");
    TEST_EXPECT_EQ(got_account, -1, "an account's is not");
}

// What needs a record in user memory is capacity-probe's, from EL0. A test thread has none,
// and that is the control here: a kernel address passes every gate before the range check,
// so each refusal below is one variable away from reaching it.
void test_account_syscall_refuses_by_name(void) {
    struct account_query rec;
    const u64 va = (u64)&rec;
    const u64 size = sizeof(rec);
    const u64 self = (u64)(s64)ACCOUNT_SELF;
    u64 op;

    TEST_EXPECT_EQ(account_call(self, ACCOUNT_QUERY, va, size), (s64)-T_E_FAULT,
                   "control: a well-formed call stops at the range check");
    TEST_EXPECT_EQ(account_call(self, ACCOUNT_QUERY, 0, size), (s64)-T_E_FAULT,
                   "a null record is no record");
    TEST_EXPECT_EQ(account_call(self, 0, va, size), (s64)-T_E_INVAL, "zero is no operation");
    TEST_EXPECT_EQ(account_call(self, ALLOCATE_FUNDED + 1u, va, size), (s64)-T_E_INVAL,
                   "nor is one past the last");
    for (op = VOUCHER_CREATE; op <= ALLOCATE_FUNDED; op++) {
        TEST_EXPECT_EQ(account_call(self, op, va, size), (s64)-T_E_OPNOTSUPP,
                       "an operation with no code behind it says so");
    }
    TEST_EXPECT_EQ(account_call(self, ACCOUNT_CHILD, va, sizeof(struct account_child_rec)),
                   (s64)-T_E_FAULT, "control: a child's record stops at the range check too");
    TEST_EXPECT_EQ(account_call(self, ACCOUNT_CHILD, va, size), (s64)-T_E_INVAL,
                   "each operation has its own record size");
    TEST_EXPECT_EQ(account_call(self, ACCOUNT_SET_LIMITS, va, sizeof(struct account_limits_rec)),
                   (s64)-T_E_ACCES, "nobody adjusts their own account");
    TEST_EXPECT_EQ(account_call(self, ACCOUNT_SET_LIMITS, va, size), (s64)-T_E_INVAL,
                   "and the record's shape comes before that");
    TEST_EXPECT_EQ(account_call(3, ACCOUNT_QUERY, va, size), (s64)-T_E_BADF,
                   "a number naming no account is no handle");
    TEST_EXPECT_EQ(account_call(0xffffffffull, ACCOUNT_QUERY, va, size), (s64)-T_E_BADF,
                   "the self selector is sign-extended, compared at full width");
    TEST_EXPECT_EQ(account_call(self, ACCOUNT_QUERY, va, size - 1u), (s64)-T_E_INVAL,
                   "a short record");
    TEST_EXPECT_EQ(account_call(self, ACCOUNT_QUERY, va, size + 1u), (s64)-T_E_INVAL,
                   "a long record");
}
