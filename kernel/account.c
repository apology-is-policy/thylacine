// The memory account tree and its physical ledger, MM-1: see <thylacine/memaccount.h>.
#include <thylacine/memaccount.h>

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>

#include "../mm/phys.h"
#include "../mm/slub.h"

static u64 g_account_unpaired;
// Sponsored pages outside the capacity pool, which is what separates the two totals.
static u64 g_account_unpooled;
static struct kmem_cache *g_account_cache;
// Section 6's tree lock. A leaf: nothing is allocated, sleeps or takes another lock inside.
static spin_lock_t g_account_tree_lock = SPIN_LOCK_INIT;

// What an account costs its parent, as a thread's slot costs a whole page: a Proc holding
// a table of handles to empty accounts is a bound on the pool and not a hole in it.
#define ACCOUNT_META_PAGES 1u

_Static_assert(ACCOUNT_CLASSES == 4, "the static accounts name every class's limit");
#define ACCOUNT_LIMITS_UNSET \
    { ACCOUNT_LIMIT_NONE, ACCOUNT_LIMIT_NONE, ACCOUNT_LIMIT_NONE, ACCOUNT_LIMIT_NONE }

static struct MemoryAccount g_account_root = {
    .incarnation = 1,
    .parent = NULL,
    .refs = 1,
    .depth = 0,
    .limit = ACCOUNT_LIMITS_UNSET,
};

static struct MemoryAccount g_account_system_cache = {
    .incarnation = 2,
    .parent = &g_account_root,
    .refs = 1,
    .depth = 1,
    .limit = ACCOUNT_LIMITS_UNSET,
};

struct MemoryAccount *account_root(void) { return &g_account_root; }
struct MemoryAccount *account_system_cache(void) { return &g_account_system_cache; }

struct MemoryAccount *account_current(void) {
    struct Thread *t = current_thread();
    if (!t || !t->proc || !t->proc->account) return &g_account_root;
    return t->proc->account;
}

void account_init(void) {
    if (g_account_cache) extinction("account_init called twice");
    g_account_cache = kmem_cache_create("memory_account", sizeof(struct MemoryAccount), 8, 0);
    if (!g_account_cache) extinction("kmem_cache_create(memory_account) returned NULL");
}

// 1 and 2 are the two static accounts. The counter is 64 bits and only ever rises, so
// no account in a boot's life shares an incarnation with one that came before it.
static u64 g_account_incarnation = 3;

// A payer exempt from the pool's refusal, as in thread_new, is counted and never refused.
static bool account_payer_exempt(void) {
    struct Thread *t = current_thread();
    return t && t->proc && proc_resource_exempt(t->proc);
}

VISIBLE_IF_KERNEL_TESTS
struct MemoryAccount *account_child_as(struct MemoryAccount *parent, bool exempt) {
    if (parent->depth + 1u >= ACCOUNT_MAX_DEPTH) return NULL;
    if (!capacity_charge(ACCOUNT_META_PAGES, exempt)) return NULL;
    struct MemoryAccount *a = kmem_cache_alloc(g_account_cache, KP_ZERO);
    if (!a) {
        capacity_uncharge(ACCOUNT_META_PAGES);
        return NULL;
    }
    a->incarnation = __atomic_fetch_add(&g_account_incarnation, 1u, __ATOMIC_RELAXED);
    a->parent = parent;
    a->refs = 1;
    a->depth = parent->depth + 1u;
    for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++)
        a->limit[cls] = ACCOUNT_LIMIT_NONE;
    account_ref(parent);
    account_charge(parent, CLASS_META, ACCOUNT_META_PAGES);
    return a;
}

struct MemoryAccount *account_child(struct MemoryAccount *parent) {
    return account_child_as(parent, account_payer_exempt());
}

void account_ref(struct MemoryAccount *a) {
    if (__atomic_fetch_add(&a->refs, 1u, __ATOMIC_RELAXED) == 0u)
        extinction("account_ref of a dead account");
}

// A loop, so a chain of last references costs no stack. The static two start with a
// reference nothing drops, so neither reaches the free. A last reference can be a block's,
// so this runs wherever free_pages does, and the slab's lock and the pool's are leaves there.
void account_unref(struct MemoryAccount *a) {
    while (a) {
        u32 was = __atomic_fetch_sub(&a->refs, 1u, __ATOMIC_ACQ_REL);
        if (was == 0u) extinction("account_unref of a dead account");
        if (was != 1u) return;
        struct MemoryAccount *parent = a->parent;
        for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++) {
            if (__atomic_load_n(&a->used[cls], __ATOMIC_RELAXED) != 0u)
                __atomic_fetch_add(&g_account_unpaired, 1u, __ATOMIC_RELAXED);
        }
        kmem_cache_free(g_account_cache, a);
        capacity_uncharge(ACCOUNT_META_PAGES);
        account_refund(parent, CLASS_META, ACCOUNT_META_PAGES);
        a = parent;
    }
}

void account_charge(struct MemoryAccount *a, unsigned cls, u64 pages) {
    for (; a; a = a->parent)
        __atomic_fetch_add(&a->used[cls], pages, __ATOMIC_RELAXED);
}

void account_refund(struct MemoryAccount *a, unsigned cls, u64 pages) {
    for (; a; a = a->parent) {
        u64 cur = __atomic_load_n(&a->used[cls], __ATOMIC_RELAXED);
        do {
            if (cur < pages) {
                __atomic_fetch_add(&g_account_unpaired, 1u, __ATOMIC_RELAXED);
                return;
            }
        } while (!__atomic_compare_exchange_n(&a->used[cls], &cur, cur - pages, true,
                                              __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }
}

VISIBLE_IF_KERNEL_TESTS
u64 account_used(const struct MemoryAccount *a, unsigned cls) {
    return __atomic_load_n(&a->used[cls], __ATOMIC_RELAXED);
}

// Section 4's unconfined default: a RAM class may reach the pool, and nothing bounds
// device storage here.
static u64 class_limit_bytes(unsigned cls) {
    if (cls == CLASS_DEVICE) return ~0ull;
    return (u64)capacity_pool_pages() << PAGE_SHIFT;
}

// The caller holds the tree lock.
static u64 own_limit(const struct MemoryAccount *a, unsigned cls) {
    return a->limit[cls] == ACCOUNT_LIMIT_NONE ? class_limit_bytes(cls) : a->limit[cls];
}

// Every class against every ancestor under one hold, so an ancestor lowered at the same
// moment is ordered with this: before it, this answer sees the lowered ceiling, and after
// it, the lowering leaves a child above its parent, which the chain's room covers.
int account_set_limits(struct MemoryAccount *a, const u64 limit[ACCOUNT_CLASSES]) {
    u64 want[ACCOUNT_CLASSES];
    for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++) {
        want[cls] = limit[cls] == ACCOUNT_LIMIT_NONE ? ACCOUNT_LIMIT_NONE
                                                     : limit[cls] & ~(u64)(PAGE_SIZE - 1u);
    }
    irq_state_t s = spin_lock_irqsave(&g_account_tree_lock);
    for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++) {
        if (want[cls] == ACCOUNT_LIMIT_NONE) continue;
        for (const struct MemoryAccount *up = a->parent; up; up = up->parent) {
            if (want[cls] > own_limit(up, cls)) {
                spin_unlock_irqrestore(&g_account_tree_lock, s);
                return -1;
            }
        }
    }
    for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++)
        a->limit[cls] = want[cls];
    spin_unlock_irqrestore(&g_account_tree_lock, s);
    return 0;
}

// Field by field: the tree has no memset for the compiler to call on a large aggregate.
void account_query_fill(const struct MemoryAccount *a, struct account_query *q) {
    q->h.version = ACCOUNT_VERSION;
    q->h.size = (u32)sizeof(*q);
    q->h.flags = 0u;
    q->h.pad = 0u;
    q->h.incarnation = a->incarnation;
    q->generation = 0u;
    q->level = ACCOUNT_NORMAL;
    q->limiting = CLASS_ALLOC;
    q->reclaim_target = 0u;
    q->retained_after_exit = 0u;
    q->awaiting_device = 0u;
    irq_state_t s = spin_lock_irqsave(&g_account_tree_lock);
    for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++) {
        q->c[cls].used = account_used(a, cls) << PAGE_SHIFT;
        q->c[cls].reserved = 0u;
        q->c[cls].limit = own_limit(a, cls);
    }

    u64 tightest = ~0ull;
    for (const struct MemoryAccount *up = a; up; up = up->parent) {
        for (unsigned cls = 0; cls < ACCOUNT_CLASSES; cls++) {
            u64 limit = own_limit(up, cls);
            u64 used = account_used(up, cls) << PAGE_SHIFT;
            u64 room = limit > used ? limit - used : 0;
            if (room < tightest) {
                tightest = room;
                q->limiting = cls;
            }
        }
    }
    spin_unlock_irqrestore(&g_account_tree_lock, s);

    u64 pool = (u64)capacity_pool_pages() << PAGE_SHIFT;
    u64 charged = (u64)capacity_pool_charged() << PAGE_SHIFT;
    u64 pool_room = pool > charged ? pool - charged : 0;
    q->headroom = pool_room < tightest ? pool_room : tightest;
}

u64 account_unpaired(void) {
    return __atomic_load_n(&g_account_unpaired, __ATOMIC_RELAXED);
}

#ifdef KERNEL_TESTS
void account_unpaired_rewind_for_test(u64 count) {
    __atomic_store_n(&g_account_unpaired, count, __ATOMIC_RELAXED);
}
#endif

// The figures are read a moment apart, so a DMA block landing between them can make
// unpooled the larger of the two for this one reading.
u64 account_pool_share(void) {
    u64 alloc = account_used(&g_account_root, CLASS_ALLOC);
    u64 unpooled = __atomic_load_n(&g_account_unpooled, __ATOMIC_RELAXED);
    return (alloc > unpooled ? alloc - unpooled : 0u) + account_used(&g_account_root, CLASS_META);
}

void account_sponsor_block(struct page *head, struct MemoryAccount *a) {
    u64 pages = 1ull << head->order;
    account_ref(a);
    account_charge(a, CLASS_ALLOC, pages);
    if (!(head->flags & PG_USER))
        __atomic_fetch_add(&g_account_unpooled, pages, __ATOMIC_RELAXED);
    head->holder.sponsor = a;
    head->flags |= PG_SPONSORED;
}

// The caller's order is only compared. The block's own is what was charged, and the pool
// refunds the caller's, so a wrong order leaves the two totals apart as well as counted.
void account_release_block(struct page *head, unsigned order) {
    struct MemoryAccount *a = head->holder.sponsor;
    u64 pages = 1ull << head->order;
    if (order != head->order)
        __atomic_fetch_add(&g_account_unpaired, 1u, __ATOMIC_RELAXED);
    head->holder.sponsor = NULL;
    head->flags &= ~PG_SPONSORED;
    if (!(head->flags & PG_USER))
        __atomic_fetch_sub(&g_account_unpooled, pages, __ATOMIC_RELAXED);
    account_refund(a, CLASS_ALLOC, pages);
    account_unref(a);
}
